#include "RenderSystem.h"
#include "Scene.h"
#include "SceneNode.h"
#include "MeshComponent.h"
#include "DefaultTextures.h"
#include "TextureUnits.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cmath>
#include <unordered_map>
#include <limits>

namespace {
	constexpr GLuint kGlobalTransformBufferBinding = 6;
	constexpr uint32_t kTransformFlagRenderable = 1u << 0;
	constexpr uint32_t kTransformFlagSkinned = 1u << 1;
	constexpr uint32_t kTransformFlagInvalid = 1u << 31;

	bool MatricesMatchExact(const glm::mat4& lhs, const glm::mat4& rhs)
	{
		for (int column = 0; column < 4; ++column) {
			for (int row = 0; row < 4; ++row) {
				if (lhs[column][row] != rhs[column][row]) {
					return false;
				}
			}
		}
		return true;
	}

	float ExtractMaxScale(const glm::mat4& transform)
	{
		const glm::vec3 basisX(transform[0]);
		const glm::vec3 basisY(transform[1]);
		const glm::vec3 basisZ(transform[2]);
		return std::max({ glm::length(basisX), glm::length(basisY), glm::length(basisZ), 1e-6f });
	}

	glm::vec3 ComputeMeshCenterWS(const glm::mat4& worldTransform, const MeshComponent& mesh)
	{
		return glm::vec3(worldTransform * glm::vec4(mesh.boundingCenter, 1.0f));
	}

	float ComputeMeshRadiusWS(const glm::mat4& worldTransform, const MeshComponent& mesh)
	{
		return mesh.boundingRadius * ExtractMaxScale(worldTransform);
	}

	glm::vec3 ComputeTransformedLocalCenter(const MeshComponent& mesh, const glm::mat4& localTransform)
	{
		return glm::vec3(localTransform * glm::vec4(mesh.boundingCenter, 1.0f));
	}

	float ComputeTransformedLocalRadius(const MeshComponent& mesh, const glm::mat4& localTransform)
	{
		return mesh.boundingRadius * ExtractMaxScale(localTransform);
	}

	template <typename Fn>
	void ForEachRenderableMesh(const RenderableComponent& renderable, Fn&& fn)
	{
		if (!renderable.model) {
			return;
		}

		if (renderable.renderWholeModel) {
			for (const auto& mesh : renderable.model->meshes) {
				fn(mesh);
			}
			return;
		}

		for (uint32_t meshIndex : renderable.meshIndices) {
			if (meshIndex < renderable.model->meshes.size()) {
				fn(renderable.model->meshes[meshIndex]);
			}
		}
	}

	int ResolveRenderableNodeIndex(const RenderableComponent& renderable)
	{
		return renderable.nodeIndex;
	}

	glm::mat4 ResolveRenderableMeshLocalTransform(const RenderableComponent& renderable, const MeshComponent& mesh)
	{
		if (!renderable.renderWholeModel || mesh.sourceNodeIndex < 0 || !renderable.model) {
			return glm::mat4(1.0f);
		}

		const int referenceNodeIndex = ResolveRenderableNodeIndex(renderable);
		if (referenceNodeIndex < 0 || referenceNodeIndex == mesh.sourceNodeIndex) {
			return mesh.localTransform;
		}

		const auto& nodeWorldTransforms = renderable.model->GetNodeWorldTransforms();
		if (referenceNodeIndex >= static_cast<int>(nodeWorldTransforms.size()) ||
			mesh.sourceNodeIndex >= static_cast<int>(nodeWorldTransforms.size())) {
			return mesh.localTransform;
		}

		glm::mat4 referenceInverse = glm::inverse(nodeWorldTransforms[referenceNodeIndex]);
		for (int c = 0; c < 4; ++c) {
			for (int r = 0; r < 4; ++r) {
				if (!std::isfinite(referenceInverse[c][r])) {
					return mesh.localTransform;
				}
			}
		}

		return referenceInverse * mesh.localTransform;
	}

	glm::mat4 ComposeRenderItemWorldTransform(const glm::mat4& entityWorldTransform, const RenderSystem::RenderItem& item)
	{
		return entityWorldTransform * item.localTransform;
	}

	glm::mat4 ResolveAnimatedNodeLocalTransform(const Scene& model, int nodeIndex, const AnimationComponent* animation)
	{
		if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
			return glm::mat4(1.0f);
		}

		if (animation &&
			(animation->isPlaying || animation->isPaused) &&
			animation->currentAnimationIndex >= 0 &&
			animation->currentAnimationIndex < static_cast<int>(model.animations.size())) {
			const Animation& activeAnimation = model.animations[animation->currentAnimationIndex];
			return activeAnimation.GetBoneTransformForNode(nodeIndex, animation->animationTime, model);
		}

		return model.nodes[nodeIndex].localTransform;
	}

	glm::mat4 ComputeAnimatedNodeWorldTransform(const Scene& model,
		int nodeIndex,
		const AnimationComponent* animation,
		std::unordered_map<int, glm::mat4>& cache)
	{
		if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
			return glm::mat4(1.0f);
		}

		auto cacheIt = cache.find(nodeIndex);
		if (cacheIt != cache.end()) {
			return cacheIt->second;
		}

		const Scene::NodeInfo& nodeInfo = model.nodes[nodeIndex];
		glm::mat4 localTransform = ResolveAnimatedNodeLocalTransform(model, nodeIndex, animation);
		glm::mat4 worldTransform = (nodeInfo.parent >= 0)
			? ComputeAnimatedNodeWorldTransform(model, nodeInfo.parent, animation, cache) * localTransform
			: localTransform;
		cache[nodeIndex] = worldTransform;
		return worldTransform;
	}
}

RenderSystem::RenderSystem(ComponentManager* componentManager, TransformSystem* transformSystem)
	: m_componentManager(componentManager)
	, m_transformSystem(transformSystem)
{
	m_renderQueue.reserve(256);
	m_submissionCache.visibleAllItems.items.reserve(1024);
	m_submissionCache.visibleOpaqueItems.items.reserve(1024);
	m_submissionCache.sortedOpaqueItems.items.reserve(1024);
	m_submissionCache.visibleTransparentItems.items.reserve(512);
	m_submissionCache.velocityItems.items.reserve(1024);
	m_submissionCache.shadowVisibleItems.items.reserve(1024);
	m_simdBackend = SimdKernels::DetectBestBackend();
}

RenderSystem::~RenderSystem()
{
	ClearBakedGBufferBatches();
	if (m_instanceVBO != 0) {
		glDeleteBuffers(1, &m_instanceVBO);
		m_instanceVBO = 0;
	}
}

void RenderSystem::SetRuntimeScene(SceneRuntimeData* runtimeScene)
{
	m_runtimeScene = runtimeScene;
	m_lastRenderItemRevision = 0;
	m_lastCameraCachePublication = 0;
	m_lastShadowCachePublication = 0;
}

void RenderSystem::BeginFrameDiagnostics()
{
	m_diagnostics = Diagnostics{};
}

void RenderSystem::PrepareFrameTransforms()
{
	if (!m_componentManager || !m_transformSystem) {
		return;
	}

	const uint64_t currentRevision = m_componentManager->GetTransformUpdateRevision();
	const uint64_t currentRenderableRevision = m_componentManager->GetRenderableRevision();
	const uint64_t currentPublication = m_transformSystem->GetWorldPublicationGeneration();
	if (m_lastPreparedTransformRevision != currentRevision ||
		m_lastPreparedRenderableRevision != currentRenderableRevision ||
		m_lastPreparedTransformPublication != currentPublication ||
		!m_transformBuffer ||
		!m_transformBuffer->IsValid() ||
		m_gpuTransformRecords.empty()) {
		const bool dirtyListAlreadyConsumed =
			m_componentManager->GetPendingTransformUpdateCount() == 0 &&
			m_lastPreparedTransformPublication != currentPublication;
		m_transformSystem->UpdateTransforms();
		const bool publicationChangedWithoutDirtyList =
			dirtyListAlreadyConsumed &&
			m_transformSystem->GetLastTransformsRecomputedCount() == 0;
		const bool forceFullTransformUpload = publicationChangedWithoutDirtyList;
		UpdateGpuTransformBuffer(forceFullTransformUpload);
		m_lastPreparedTransformRevision = currentRevision;
		m_lastPreparedRenderableRevision = currentRenderableRevision;
		m_lastPreparedTransformPublication = m_transformSystem ? m_transformSystem->GetWorldPublicationGeneration() : 0;
	}

	RebuildRenderItemsIfNeeded();
	UpdateChangedRenderItemBounds();
}

const RenderSystem::ShaderUniformCache& RenderSystem::GetShaderUniformCache(GLuint shader)
{
	auto it = m_shaderUniformCaches.find(shader);
	if (it != m_shaderUniformCaches.end()) {
		return it->second;
	}

	ShaderUniformCache uniforms{};
	uniforms.view = glGetUniformLocation(shader, "view");
	uniforms.projection = glGetUniformLocation(shader, "projection");
	uniforms.model = glGetUniformLocation(shader, "model");
	uniforms.normalMatrix = glGetUniformLocation(shader, "normalMatrix");
	uniforms.prevView = glGetUniformLocation(shader, "prevView");
	uniforms.prevProjection = glGetUniformLocation(shader, "prevProjection");
	uniforms.prevModel = glGetUniformLocation(shader, "prevModel");
	uniforms.transformID = glGetUniformLocation(shader, "uTransformID");
	uniforms.useVertexTransformID = glGetUniformLocation(shader, "uUseVertexTransformID");
	uniforms.useModelMatrixUniform = glGetUniformLocation(shader, "uUseModelMatrixUniform");
	uniforms.materialID = glGetUniformLocation(shader, "uMaterialID");
	uniforms.lightSpaceMatrix = glGetUniformLocation(shader, "lightSpaceMatrix");
	uniforms.uEnableSkinning = glGetUniformLocation(shader, "u_enableSkinning");
	uniforms.uBoneMatrices = glGetUniformLocation(shader, "u_boneMatrices");
	uniforms.bones = glGetUniformLocation(shader, "bones");

	uniforms.textureDiffuse = glGetUniformLocation(shader, "texture_diffuse");
	uniforms.textureNormal = glGetUniformLocation(shader, "texture_normal");
	uniforms.textureMetallicRoughness = glGetUniformLocation(shader, "texture_metallic_roughness");
	uniforms.textureEmissive = glGetUniformLocation(shader, "texture_emissive");
	uniforms.textureOcclusion = glGetUniformLocation(shader, "texture_occlusion");
	uniforms.textureSpecular = glGetUniformLocation(shader, "texture_specular");
	uniforms.textureSpecularColor = glGetUniformLocation(shader, "texture_specular_color");
	uniforms.textureTransmission = glGetUniformLocation(shader, "texture_transmission");

	uniforms.hasBaseColorTexture = glGetUniformLocation(shader, "hasBaseColorTexture");
	uniforms.hasNormalTexture = glGetUniformLocation(shader, "hasNormalTexture");
	uniforms.hasMetallicRoughnessTexture = glGetUniformLocation(shader, "hasMetallicRoughnessTexture");
	uniforms.hasEmissiveTexture = glGetUniformLocation(shader, "hasEmissiveTexture");
	uniforms.hasOcclusionTexture = glGetUniformLocation(shader, "hasOcclusionTexture");
	uniforms.hasSpecularTexture = glGetUniformLocation(shader, "hasSpecularTexture");
	uniforms.hasSpecularColorTexture = glGetUniformLocation(shader, "hasSpecularColorTexture");
	uniforms.hasTransmissionTexture = glGetUniformLocation(shader, "hasTransmissionTexture");

	uniforms.baseColorFactor = glGetUniformLocation(shader, "baseColorFactor");
	uniforms.metallicFactor = glGetUniformLocation(shader, "metallicFactor");
	uniforms.roughnessFactor = glGetUniformLocation(shader, "roughnessFactor");
	uniforms.emissiveFactor = glGetUniformLocation(shader, "emissiveFactor");
	uniforms.emissiveStrength = glGetUniformLocation(shader, "emissiveStrength");
	uniforms.occlusionStrength = glGetUniformLocation(shader, "occlusionStrength");
	uniforms.normalScale = glGetUniformLocation(shader, "normalScale");
	uniforms.alphaCutoff = glGetUniformLocation(shader, "alphaCutoff");
	uniforms.alphaMode = glGetUniformLocation(shader, "alphaMode");
	uniforms.specularFactor = glGetUniformLocation(shader, "specularFactor");
	uniforms.specularColorFactor = glGetUniformLocation(shader, "specularColorFactor");
	uniforms.clearcoatFactor = glGetUniformLocation(shader, "clearcoatFactor");
	uniforms.clearcoatRoughnessFactor = glGetUniformLocation(shader, "clearcoatRoughnessFactor");
	uniforms.transmissionFactor = glGetUniformLocation(shader, "transmissionFactor");
	uniforms.thicknessFactor = glGetUniformLocation(shader, "thicknessFactor");
	uniforms.attenuationDistance = glGetUniformLocation(shader, "attenuationDistance");
	uniforms.attenuationColor = glGetUniformLocation(shader, "attenuationColor");
	uniforms.ior = glGetUniformLocation(shader, "ior");

	uniforms.baseColorUVSet = glGetUniformLocation(shader, "baseColorUVSet");
	uniforms.normalUVSet = glGetUniformLocation(shader, "normalUVSet");
	uniforms.metallicRoughnessUVSet = glGetUniformLocation(shader, "metallicRoughnessUVSet");
	uniforms.emissiveUVSet = glGetUniformLocation(shader, "emissiveUVSet");
	uniforms.occlusionUVSet = glGetUniformLocation(shader, "occlusionUVSet");
	uniforms.specularUVSet = glGetUniformLocation(shader, "specularUVSet");
	uniforms.specularColorUVSet = glGetUniformLocation(shader, "specularColorUVSet");
	uniforms.transmissionUVSet = glGetUniformLocation(shader, "transmissionUVSet");

	auto [insertedIt, _] = m_shaderUniformCaches.emplace(shader, uniforms);
	return insertedIt->second;
}

void RenderSystem::RenderForward(const glm::mat4& view,
	const glm::mat4& projection,
	GLuint defaultShader)
{
	if (!m_componentManager || !m_transformSystem) return;

	PrepareFrameTransforms();
	PrepareCameraSubmissionCache();
	m_cachedBoneMatrices.clear();

	glUseProgram(defaultShader);
	ResetMaterialStateCache(defaultShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}

	// Upload view and projection matrices once
	const auto& uniforms = GetShaderUniformCache(defaultShader);
	if (uniforms.view != -1) glUniformMatrix4fv(uniforms.view, 1, GL_FALSE, glm::value_ptr(view));
	if (uniforms.projection != -1) glUniformMatrix4fv(uniforms.projection, 1, GL_FALSE, glm::value_ptr(projection));
	UploadMaterialSamplerUniforms(uniforms);
	int skinningState = -1;
		GLuint currentVAO = 0;
		bool blendEnabled = false;
		bool depthWriteEnabled = true;
		glDisable(GL_BLEND);
		glDepthMask(GL_TRUE);

	m_visibleCount = 0;
	m_totalCount = m_renderItems.size();
	m_visibleCount = m_submissionCache.visibleAllItems.items.size();

	for (uint32_t itemIndex : m_submissionCache.visibleAllItems.items) {
		if (itemIndex >= m_renderItems.size()) {
			continue;
		}

		const RenderItem& item = m_renderItems[itemIndex];
		if (!item.mesh) {
			continue;
		}

		const glm::mat4& entityWorldTransform = (m_runtimeScene && item.runtimeNodeIndex != INVALID_RUNTIME_NODE_INDEX)
			? m_runtimeScene->GetWorldTransformByIndex(item.runtimeNodeIndex)
			: m_transformSystem->GetWorldTransform(item.entity);
		const glm::mat4 worldTransform = ComposeRenderItemWorldTransform(entityWorldTransform, item);

		UploadTransformUniforms(item.entity, worldTransform, uniforms);
		if (item.skinned) {
			UploadBoneMatrices(item.entity, worldTransform, uniforms);
			if (uniforms.uEnableSkinning != -1 && skinningState != 1) {
				glUniform1i(uniforms.uEnableSkinning, 1);
				skinningState = 1;
			}
		}
		else if (uniforms.uEnableSkinning != -1 && skinningState != 0) {
			glUniform1i(uniforms.uEnableSkinning, 0);
			skinningState = 0;
		}

		ApplyCullingState(*item.mesh, item.cullingOverride, worldTransform);

		if (item.transparent != blendEnabled) {
			if (item.transparent) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
			else {
				glDisable(GL_BLEND);
			}
			blendEnabled = item.transparent;
		}
		if (item.transparent == depthWriteEnabled) {
			glDepthMask(item.transparent ? GL_FALSE : GL_TRUE);
			depthWriteEnabled = !item.transparent;
		}

		BindMaterialTextures(*item.mesh, uniforms);
		UploadMaterialUniforms(*item.mesh, uniforms);

		if (currentVAO != item.mesh->VAO) {
			glBindVertexArray(item.mesh->VAO);
			currentVAO = item.mesh->VAO;
		}
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(item.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
		++m_diagnostics.forwardDrawCalls;
	}

	if (currentVAO != 0) {
		glBindVertexArray(0);
	}
	if (blendEnabled) {
		glDisable(GL_BLEND);
	}
	if (!depthWriteEnabled) {
		glDepthMask(GL_TRUE);
	}
}

void RenderSystem::RenderGeometry(GLuint geometryShader)
{
	if (!m_componentManager || !m_transformSystem) {
		std::cerr << "[RenderSystem] ERROR: Missing component manager or transform system!" << std::endl;
		return;
	}

	PrepareFrameTransforms();
	PrepareCameraSubmissionCache();
	RenderGeometryPrepared(geometryShader);
}

void RenderSystem::RenderGeometryPrepared(GLuint geometryShader)
{
	m_cachedBoneMatrices.clear();
	glUseProgram(geometryShader);
	ResetMaterialStateCache(geometryShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(geometryShader);
	UploadMaterialSamplerUniforms(uniforms);
	if (uniforms.useVertexTransformID != -1) {
		glUniform1i(uniforms.useVertexTransformID, 0);
	}

	m_visibleCount = 0;
	m_totalCount = 0;
	m_totalCount = m_renderItems.size();
	m_visibleCount = m_submissionCache.visibleOpaqueItems.items.size();

	GLuint currentVAO = 0;
	int skinningState = -1;
	for (uint32_t itemIndex : m_submissionCache.sortedOpaqueItems.items) {
		if (itemIndex >= m_renderItems.size()) {
			continue;
		}
		const RenderItem& item = m_renderItems[itemIndex];
		if (!item.mesh) {
			continue;
		}

		const glm::mat4& entityWorldTransform = (m_runtimeScene && item.runtimeNodeIndex != INVALID_RUNTIME_NODE_INDEX)
			? m_runtimeScene->GetWorldTransformByIndex(item.runtimeNodeIndex)
			: m_transformSystem->GetWorldTransform(item.entity);
		const glm::mat4 worldTransform = ComposeRenderItemWorldTransform(entityWorldTransform, item);
		const MeshComponent& mesh = *item.mesh;

		UploadTransformUniforms(item.entity, worldTransform, uniforms);
		if (item.skinned) {
			UploadBoneMatrices(item.entity, worldTransform, uniforms);
			if (uniforms.uEnableSkinning != -1 && skinningState != 1) {
				glUniform1i(uniforms.uEnableSkinning, 1);
				skinningState = 1;
			}
		}
		else if (uniforms.uEnableSkinning != -1 && skinningState != 0) {
			glUniform1i(uniforms.uEnableSkinning, 0);
			skinningState = 0;
		}

		ApplyCullingState(mesh, item.cullingOverride, worldTransform);
		BindMaterialTextures(mesh, uniforms);
		UploadMaterialUniforms(mesh, uniforms);

		if (currentVAO != mesh.VAO) {
			glBindVertexArray(mesh.VAO);
			currentVAO = mesh.VAO;
		}
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
		++m_diagnostics.geometryDrawCalls;
	}

	if (currentVAO != 0) {
		glBindVertexArray(0);
	}
}

void RenderSystem::RenderGeometryBatchedGBuffer(GLuint geometryShader)
{
	if (!m_componentManager || !m_transformSystem) {
		std::cerr << "[RenderSystem] ERROR: Missing component manager or transform system!" << std::endl;
		return;
	}

	PrepareFrameTransforms();
	PrepareCameraSubmissionCache();

	if (!ShouldUseBakedGBufferPath()) {
		RenderGeometryPrepared(geometryShader);
		return;
	}

	RebuildBakedGBufferBatches();
	if (m_bakedGBufferBatches.empty()) {
		RenderGeometryPrepared(geometryShader);
		return;
	}
	UpdateBakedGBufferTransformBuffer();
	if (!m_bakedGBufferTransformBuffer || !m_bakedGBufferTransformBuffer->IsValid()) {
		RenderGeometryPrepared(geometryShader);
		return;
	}

	m_cachedBoneMatrices.clear();
	glUseProgram(geometryShader);
	ResetMaterialStateCache(geometryShader);
	m_bakedGBufferTransformBuffer->BindBase(kGlobalTransformBufferBinding);
	const auto& uniforms = GetShaderUniformCache(geometryShader);
	UploadMaterialSamplerUniforms(uniforms);

	const glm::mat4 identity(1.0f);
	const glm::mat3 identityNormal(1.0f);
	if (uniforms.model != -1) {
		glUniformMatrix4fv(uniforms.model, 1, GL_FALSE, glm::value_ptr(identity));
	}
	if (uniforms.normalMatrix != -1) {
		glUniformMatrix3fv(uniforms.normalMatrix, 1, GL_FALSE, glm::value_ptr(identityNormal));
	}
	if (uniforms.uEnableSkinning != -1) {
		glUniform1i(uniforms.uEnableSkinning, 0);
	}
	if (uniforms.useVertexTransformID != -1) {
		glUniform1i(uniforms.useVertexTransformID, 1);
	}

	m_totalCount = m_renderItems.size();
	m_visibleCount = m_submissionCache.visibleOpaqueItems.items.size();

	for (const BakedGBufferBatch& batch : m_bakedGBufferBatches) {
		if (!batch.materialMesh || batch.vao == 0 || batch.indexCount == 0) {
			continue;
		}

		ApplyBakedBatchCulling(*batch.materialMesh, batch.cullingOverride, batch.frontFace);
		BindMaterialTextures(*batch.materialMesh, uniforms);
		UploadMaterialUniforms(*batch.materialMesh, uniforms);

		glBindVertexArray(batch.vao);
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT, nullptr);
		++m_diagnostics.geometryDrawCalls;
	}

	glBindVertexArray(0);
	if (uniforms.useVertexTransformID != -1) {
		glUniform1i(uniforms.useVertexTransformID, 0);
	}
}

void RenderSystem::RenderShadowCascade(const glm::mat4& lightSpaceMatrix, GLuint shadowShader)
{
	if (!m_componentManager || !m_transformSystem) return;

	PrepareFrameTransforms();
	PrepareShadowSubmissionCache(lightSpaceMatrix);
	m_cachedBoneMatrices.clear();
	glUseProgram(shadowShader);
	ResetMaterialStateCache(shadowShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(shadowShader);
	UploadMaterialSamplerUniforms(uniforms);

	if (uniforms.lightSpaceMatrix != -1) {
		glUniformMatrix4fv(uniforms.lightSpaceMatrix, 1, GL_FALSE, glm::value_ptr(lightSpaceMatrix));
	}
	if (uniforms.useModelMatrixUniform != -1) {
		glUniform1i(uniforms.useModelMatrixUniform, 1);
	}

	GLuint currentVAO = 0;
	int skinningState = -1;
	for (uint32_t itemIndex : m_submissionCache.shadowVisibleItems.items) {
		if (itemIndex >= m_renderItems.size()) {
			continue;
		}

		const RenderItem& item = m_renderItems[itemIndex];
		const glm::mat4& entityWorldTransform = (m_runtimeScene && item.runtimeNodeIndex != INVALID_RUNTIME_NODE_INDEX)
			? m_runtimeScene->GetWorldTransformByIndex(item.runtimeNodeIndex)
			: m_transformSystem->GetWorldTransform(item.entity);
		const glm::mat4 worldTransform = ComposeRenderItemWorldTransform(entityWorldTransform, item);
		UploadTransformUniforms(item.entity, worldTransform, uniforms);
		if (item.skinned) {
			UploadBoneMatrices(item.entity, worldTransform, uniforms);
			if (uniforms.uEnableSkinning != -1 && skinningState != 1) {
				glUniform1i(uniforms.uEnableSkinning, 1);
				skinningState = 1;
			}
		}
		else if (uniforms.uEnableSkinning != -1 && skinningState != 0) {
			glUniform1i(uniforms.uEnableSkinning, 0);
			skinningState = 0;
		}

		ApplyCullingState(*item.mesh, item.cullingOverride, worldTransform);
		BindMaterialTextures(*item.mesh, uniforms);
		UploadMaterialUniforms(*item.mesh, uniforms);
		if (currentVAO != item.mesh->VAO) {
			glBindVertexArray(item.mesh->VAO);
			currentVAO = item.mesh->VAO;
		}
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(item.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
		++m_diagnostics.shadowDrawCalls;
	}
	if (currentVAO != 0) {
		glBindVertexArray(0);
	}
	if (uniforms.useModelMatrixUniform != -1) {
		glUniform1i(uniforms.useModelMatrixUniform, 0);
	}
}

void RenderSystem::RenderVelocity(const glm::mat4& view,
	const glm::mat4& projection,
	const glm::mat4& prevView,
	const glm::mat4& prevProjection,
	GLuint velocityShader)
{
	if (!m_componentManager || !m_transformSystem) return;

	PrepareFrameTransforms();
	PrepareCameraSubmissionCache();
	m_cachedBoneMatrices.clear();
	glUseProgram(velocityShader);
	ResetMaterialStateCache(velocityShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(velocityShader);
	UploadMaterialSamplerUniforms(uniforms);

	if (uniforms.view != -1) glUniformMatrix4fv(uniforms.view, 1, GL_FALSE, glm::value_ptr(view));
	if (uniforms.projection != -1) glUniformMatrix4fv(uniforms.projection, 1, GL_FALSE, glm::value_ptr(projection));
	if (uniforms.prevView != -1) glUniformMatrix4fv(uniforms.prevView, 1, GL_FALSE, glm::value_ptr(prevView));
	if (uniforms.prevProjection != -1) glUniformMatrix4fv(uniforms.prevProjection, 1, GL_FALSE, glm::value_ptr(prevProjection));
	int skinningState = -1;
	GLuint currentVAO = 0;

	for (uint32_t itemIndex : m_submissionCache.velocityItems.items) {
		if (itemIndex >= m_renderItems.size()) {
			continue;
		}

		const RenderItem& item = m_renderItems[itemIndex];
		const glm::mat4& entityWorldTransform = (m_runtimeScene && item.runtimeNodeIndex != INVALID_RUNTIME_NODE_INDEX)
			? m_runtimeScene->GetWorldTransformByIndex(item.runtimeNodeIndex)
			: m_transformSystem->GetWorldTransform(item.entity);
		const glm::mat4 worldTransform = ComposeRenderItemWorldTransform(entityWorldTransform, item);

		UploadTransformUniforms(item.entity, worldTransform, uniforms);
		if (item.skinned) {
			UploadBoneMatrices(item.entity, worldTransform, uniforms);
			if (uniforms.uEnableSkinning != -1 && skinningState != 1) {
				glUniform1i(uniforms.uEnableSkinning, 1);
				skinningState = 1;
			}
		}
		else if (uniforms.uEnableSkinning != -1 && skinningState != 0) {
			glUniform1i(uniforms.uEnableSkinning, 0);
			skinningState = 0;
		}

		ApplyCullingState(*item.mesh, item.cullingOverride, worldTransform);
		if (currentVAO != item.mesh->VAO) {
			glBindVertexArray(item.mesh->VAO);
			currentVAO = item.mesh->VAO;
		}
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(item.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
		++m_diagnostics.velocityDrawCalls;
	}

	if (currentVAO != 0) {
		glBindVertexArray(0);
	}
}

void RenderSystem::CollectRenderables(MDIBatch& batch)
{
	if (!m_componentManager || !m_transformSystem) return;

	PrepareFrameTransforms();
	RebuildRenderItemsIfNeeded();
	UpdateChangedRenderItemBounds();
	for (size_t itemIndex = 0; itemIndex < m_renderItems.size(); ++itemIndex) {
		const RenderItem& item = m_renderItems[itemIndex];
		if (!item.mesh) {
			continue;
		}

		MDI_RenderableObject obj{};
		obj.count = static_cast<GLuint>(item.mesh->indexCount);
		obj.firstIndex = 0;
		obj.baseVertex = 0;
		const glm::mat4& entityWorldTransform = (m_runtimeScene && item.runtimeNodeIndex != INVALID_RUNTIME_NODE_INDEX)
			? m_runtimeScene->GetWorldTransformByIndex(item.runtimeNodeIndex)
			: m_transformSystem->GetWorldTransform(item.entity);
		obj.modelMatrix = ComposeRenderItemWorldTransform(entityWorldTransform, item);
		if (const TransformComponent* transform = m_componentManager->GetTransform(item.entity)) {
			obj.transformID = transform->transformID;
		}
		obj.vao = item.mesh->VAO;
		obj.boundingSphere = glm::vec4(m_renderItemWorldCenterX[itemIndex],
			m_renderItemWorldCenterY[itemIndex],
			m_renderItemWorldCenterZ[itemIndex],
			m_renderItemWorldRadius[itemIndex]);
		batch.AddObject(obj);
	}
}

void RenderSystem::RenderTransparent(const glm::mat4& view,
	const glm::mat4& projection,
	GLuint transparentShader)
{
	if (!m_componentManager || !m_transformSystem) return;
	PrepareFrameTransforms();
	PrepareCameraSubmissionCache();
	m_cachedBoneMatrices.clear();

	glm::vec3 cameraPos = glm::vec3(glm::inverse(view)[3]);

	m_renderQueue.clear();
	for (uint32_t itemIndex : m_submissionCache.visibleTransparentItems.items) {
		if (itemIndex >= m_renderItems.size()) {
			continue;
		}

		const glm::vec3 centerWS(
			m_renderItemWorldCenterX[itemIndex],
			m_renderItemWorldCenterY[itemIndex],
			m_renderItemWorldCenterZ[itemIndex]);
		const glm::vec3 toCamera = centerWS - cameraPos;
		const float dist = glm::dot(toCamera, toCamera);
		const RenderItem& item = m_renderItems[itemIndex];
		RenderBatch batch;
		batch.entity = item.entity;
		batch.runtimeNodeIndex = item.runtimeNodeIndex;
		batch.mesh = item.mesh;
		batch.localTransform = item.localTransform;
		batch.distanceToCamera = dist;
		batch.sortKey = item.sortKey;
		batch.isTransparent = true;
		batch.cullingOverride = item.cullingOverride;
		batch.skinned = item.skinned;
		m_renderQueue.push_back(batch);
	}

	// Sort back to front
	const auto sortStart = std::chrono::high_resolution_clock::now();
	SortRenderQueue(cameraPos);
	const auto sortEnd = std::chrono::high_resolution_clock::now();
	m_diagnostics.transparentSortMs += std::chrono::duration<float, std::milli>(sortEnd - sortStart).count();

	// Render
	glUseProgram(transparentShader);
	ResetMaterialStateCache(transparentShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(transparentShader);
	UploadMaterialSamplerUniforms(uniforms);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);

	if (uniforms.view != -1) glUniformMatrix4fv(uniforms.view, 1, GL_FALSE, glm::value_ptr(view));
	if (uniforms.projection != -1) glUniformMatrix4fv(uniforms.projection, 1, GL_FALSE, glm::value_ptr(projection));

	GLuint currentVAO = 0;
	int skinningState = -1;
	for (const auto& batch : m_renderQueue) {
		if (!batch.mesh) {
			continue;
		}

		const glm::mat4& entityWorldTransform = (m_runtimeScene && batch.runtimeNodeIndex != INVALID_RUNTIME_NODE_INDEX)
			? m_runtimeScene->GetWorldTransformByIndex(batch.runtimeNodeIndex)
			: m_transformSystem->GetWorldTransform(batch.entity);
		const glm::mat4 worldTransform = entityWorldTransform * batch.localTransform;

		UploadTransformUniforms(batch.entity, worldTransform, uniforms);
		if (batch.skinned) {
			UploadBoneMatrices(batch.entity, worldTransform, uniforms);
			if (uniforms.uEnableSkinning != -1 && skinningState != 1) {
				glUniform1i(uniforms.uEnableSkinning, 1);
				skinningState = 1;
			}
		}
		else if (uniforms.uEnableSkinning != -1 && skinningState != 0) {
			glUniform1i(uniforms.uEnableSkinning, 0);
			skinningState = 0;
		}

		ApplyCullingState(*batch.mesh, batch.cullingOverride, worldTransform);
		BindMaterialTextures(*batch.mesh, uniforms);
		UploadMaterialUniforms(*batch.mesh, uniforms);

		if (currentVAO != batch.mesh->VAO) {
			glBindVertexArray(batch.mesh->VAO);
			currentVAO = batch.mesh->VAO;
		}
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
		++m_diagnostics.transparentDrawCalls;
	}

	if (currentVAO != 0) {
		glBindVertexArray(0);
	}

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

void RenderSystem::SetFrustumPlanes(const glm::mat4& viewProjection)
{
	// Extract frustum planes from view-projection matrix
	// Left plane
	m_frustumPlanes[0] = glm::vec4(
		viewProjection[0][3] + viewProjection[0][0],
		viewProjection[1][3] + viewProjection[1][0],
		viewProjection[2][3] + viewProjection[2][0],
		viewProjection[3][3] + viewProjection[3][0]
	);
	// Right plane
	m_frustumPlanes[1] = glm::vec4(
		viewProjection[0][3] - viewProjection[0][0],
		viewProjection[1][3] - viewProjection[1][0],
		viewProjection[2][3] - viewProjection[2][0],
		viewProjection[3][3] - viewProjection[3][0]
	);
	// Bottom plane
	m_frustumPlanes[2] = glm::vec4(
		viewProjection[0][3] + viewProjection[0][1],
		viewProjection[1][3] + viewProjection[1][1],
		viewProjection[2][3] + viewProjection[2][1],
		viewProjection[3][3] + viewProjection[3][1]
	);
	// Top plane
	m_frustumPlanes[3] = glm::vec4(
		viewProjection[0][3] - viewProjection[0][1],
		viewProjection[1][3] - viewProjection[1][1],
		viewProjection[2][3] - viewProjection[2][1],
		viewProjection[3][3] - viewProjection[3][1]
	);
	// Near plane
	m_frustumPlanes[4] = glm::vec4(
		viewProjection[0][3] + viewProjection[0][2],
		viewProjection[1][3] + viewProjection[1][2],
		viewProjection[2][3] + viewProjection[2][2],
		viewProjection[3][3] + viewProjection[3][2]
	);
	// Far plane
	m_frustumPlanes[5] = glm::vec4(
		viewProjection[0][3] - viewProjection[0][2],
		viewProjection[1][3] - viewProjection[1][2],
		viewProjection[2][3] - viewProjection[2][2],
		viewProjection[3][3] - viewProjection[3][2]
	);

	// Normalize planes
	for (int i = 0; i < 6; ++i) {
		float len = glm::length(glm::vec3(m_frustumPlanes[i]));
		if (len > 0.0f) {
			m_frustumPlanes[i] /= len;
		}
	}

	m_frustumValid = true;
	++m_frustumSerial;
}

bool RenderSystem::IsSphereVisible(const glm::vec3& center, float radius) const
{
	for (int i = 0; i < 6; ++i) {
		float distance = glm::dot(glm::vec3(m_frustumPlanes[i]), center) + m_frustumPlanes[i].w;
		if (distance < -radius) {
			return false; // Sphere is completely outside this plane
		}
	}
	return true;
}

void RenderSystem::BindMaterialTextures(const MeshComponent& mesh, const ShaderUniformCache& uniforms)
{
	auto bindTexture = [this](GLuint unit, const std::shared_ptr<Texture>& tex, GLuint fallback) {
		const GLuint desiredTexture = (tex && tex->IsValid()) ? tex->ID() : fallback;
		if (m_boundMaterialTextures[unit] != desiredTexture) {
			glActiveTexture(GL_TEXTURE0 + unit);
			glBindTexture(GL_TEXTURE_2D, desiredTexture);
			m_boundMaterialTextures[unit] = desiredTexture;
			++m_diagnostics.textureBindCount;
		}
		};

	bindTexture(TextureUnits::MATERIAL_BASE_COLOR, mesh.diffuseTexture, DefaultTextures::White());
	bindTexture(TextureUnits::MATERIAL_NORMAL, mesh.normalTexture, DefaultTextures::Normal());
	bindTexture(TextureUnits::MATERIAL_METALLIC_ROUGHNESS, mesh.roughnessTexture, DefaultTextures::MetallicRoughnessDefault());
	bindTexture(TextureUnits::MATERIAL_EMISSIVE, mesh.emissiveTexture, DefaultTextures::Black());
	bindTexture(TextureUnits::MATERIAL_OCCLUSION, mesh.occlusionTexture, DefaultTextures::AOWhite());
	bindTexture(TextureUnits::MATERIAL_SPECULAR, mesh.specularTexture, DefaultTextures::White());
	bindTexture(TextureUnits::MATERIAL_SPECULAR_COLOR, mesh.specularColorTexture, DefaultTextures::White());
	bindTexture(TextureUnits::MATERIAL_TRANSMISSION, mesh.transmissionTexture, DefaultTextures::Black());
}

void RenderSystem::UploadMaterialSamplerUniforms(const ShaderUniformCache& uniforms)
{
	if (uniforms.textureDiffuse >= 0) glUniform1i(uniforms.textureDiffuse, TextureUnits::MATERIAL_BASE_COLOR);
	if (uniforms.textureNormal >= 0) glUniform1i(uniforms.textureNormal, TextureUnits::MATERIAL_NORMAL);
	if (uniforms.textureMetallicRoughness >= 0) glUniform1i(uniforms.textureMetallicRoughness, TextureUnits::MATERIAL_METALLIC_ROUGHNESS);
	if (uniforms.textureEmissive >= 0) glUniform1i(uniforms.textureEmissive, TextureUnits::MATERIAL_EMISSIVE);
	if (uniforms.textureOcclusion >= 0) glUniform1i(uniforms.textureOcclusion, TextureUnits::MATERIAL_OCCLUSION);
	if (uniforms.textureSpecular >= 0) glUniform1i(uniforms.textureSpecular, TextureUnits::MATERIAL_SPECULAR);
	if (uniforms.textureSpecularColor >= 0) glUniform1i(uniforms.textureSpecularColor, TextureUnits::MATERIAL_SPECULAR_COLOR);
	if (uniforms.textureTransmission >= 0) glUniform1i(uniforms.textureTransmission, TextureUnits::MATERIAL_TRANSMISSION);
}

void RenderSystem::UploadMaterialUniforms(const MeshComponent& mesh, const ShaderUniformCache& uniforms)
{
	const MaterialDesc material = mesh.GetMaterialDesc();
	if (m_cachedMaterialID == material.stableMaterialID) {
		++m_diagnostics.materialCacheHitCount;
		return;
	}
	++m_diagnostics.materialUploadCount;

	if (uniforms.materialID >= 0) glUniform1ui(uniforms.materialID, material.stableMaterialID);
	if (uniforms.baseColorFactor >= 0) glUniform4fv(uniforms.baseColorFactor, 1, glm::value_ptr(material.baseColorFactor));
	if (uniforms.metallicFactor >= 0) glUniform1f(uniforms.metallicFactor, material.metallicFactor);
	if (uniforms.roughnessFactor >= 0) glUniform1f(uniforms.roughnessFactor, material.roughnessFactor);
	if (uniforms.emissiveFactor >= 0) glUniform3fv(uniforms.emissiveFactor, 1, glm::value_ptr(material.emissiveFactor));
	if (uniforms.emissiveStrength >= 0) glUniform1f(uniforms.emissiveStrength, material.emissiveStrength);
	if (uniforms.occlusionStrength >= 0) glUniform1f(uniforms.occlusionStrength, material.occlusionStrength);
	if (uniforms.normalScale >= 0) glUniform1f(uniforms.normalScale, material.normalScale);
	if (uniforms.alphaCutoff >= 0) glUniform1f(uniforms.alphaCutoff, material.alphaCutoff);
	if (uniforms.alphaMode >= 0) glUniform1i(uniforms.alphaMode, static_cast<int>(material.alphaMode));
	if (uniforms.specularFactor >= 0) glUniform1f(uniforms.specularFactor, material.specularFactor);
	if (uniforms.specularColorFactor >= 0) glUniform3fv(uniforms.specularColorFactor, 1, glm::value_ptr(material.specularColorFactor));
	if (uniforms.clearcoatFactor >= 0) glUniform1f(uniforms.clearcoatFactor, material.clearcoatFactor);
	if (uniforms.clearcoatRoughnessFactor >= 0) glUniform1f(uniforms.clearcoatRoughnessFactor, material.clearcoatRoughnessFactor);
	if (uniforms.transmissionFactor >= 0) glUniform1f(uniforms.transmissionFactor, material.transmissionFactor);
	if (uniforms.thicknessFactor >= 0) glUniform1f(uniforms.thicknessFactor, material.thicknessFactor);
	if (uniforms.attenuationDistance >= 0) glUniform1f(uniforms.attenuationDistance, material.attenuationDistance);
	if (uniforms.attenuationColor >= 0) glUniform3fv(uniforms.attenuationColor, 1, glm::value_ptr(material.attenuationColor));
	if (uniforms.ior >= 0) glUniform1f(uniforms.ior, material.ior);

	auto clampUVSet = [](int uvSet) { return (uvSet == 1) ? 1 : 0; };
	if (uniforms.baseColorUVSet >= 0) glUniform1i(uniforms.baseColorUVSet, clampUVSet(material.baseColorTexCoord));
	if (uniforms.normalUVSet >= 0) glUniform1i(uniforms.normalUVSet, clampUVSet(material.normalTexCoord));
	if (uniforms.metallicRoughnessUVSet >= 0) glUniform1i(uniforms.metallicRoughnessUVSet, clampUVSet(material.metallicRoughnessTexCoord));
	if (uniforms.emissiveUVSet >= 0) glUniform1i(uniforms.emissiveUVSet, clampUVSet(material.emissiveTexCoord));
	if (uniforms.occlusionUVSet >= 0) glUniform1i(uniforms.occlusionUVSet, clampUVSet(material.occlusionTexCoord));
	if (uniforms.specularUVSet >= 0) glUniform1i(uniforms.specularUVSet, clampUVSet(material.specularTexCoord));
	if (uniforms.specularColorUVSet >= 0) glUniform1i(uniforms.specularColorUVSet, clampUVSet(material.specularColorTexCoord));
	if (uniforms.transmissionUVSet >= 0) glUniform1i(uniforms.transmissionUVSet, clampUVSet(material.transmissionTexCoord));

	if (uniforms.hasBaseColorTexture >= 0) glUniform1i(uniforms.hasBaseColorTexture, (mesh.diffuseTexture && mesh.diffuseTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasNormalTexture >= 0) glUniform1i(uniforms.hasNormalTexture, (mesh.normalTexture && mesh.normalTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasMetallicRoughnessTexture >= 0) glUniform1i(uniforms.hasMetallicRoughnessTexture, (mesh.roughnessTexture && mesh.roughnessTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasEmissiveTexture >= 0) glUniform1i(uniforms.hasEmissiveTexture, (mesh.emissiveTexture && mesh.emissiveTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasOcclusionTexture >= 0) glUniform1i(uniforms.hasOcclusionTexture, (mesh.occlusionTexture && mesh.occlusionTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasSpecularTexture >= 0) glUniform1i(uniforms.hasSpecularTexture, (mesh.specularTexture && mesh.specularTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasSpecularColorTexture >= 0) glUniform1i(uniforms.hasSpecularColorTexture, (mesh.specularColorTexture && mesh.specularColorTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasTransmissionTexture >= 0) glUniform1i(uniforms.hasTransmissionTexture, (mesh.transmissionTexture && mesh.transmissionTexture->IsValid()) ? 1 : 0);

	m_cachedMaterialID = material.stableMaterialID;
}

void RenderSystem::UploadTransformUniforms(EntityID entity,
	const glm::mat4& modelTransform,
	const ShaderUniformCache& uniforms)
{
	if (uniforms.model != -1) {
		glUniformMatrix4fv(uniforms.model, 1, GL_FALSE, glm::value_ptr(modelTransform));
	}
	if (uniforms.normalMatrix != -1) {
		const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(modelTransform)));
		glUniformMatrix3fv(uniforms.normalMatrix, 1, GL_FALSE, glm::value_ptr(normalMatrix));
	}

	const TransformComponent* transform = m_componentManager ? m_componentManager->GetTransform(entity) : nullptr;
	glm::mat4 prevModelTransform = modelTransform;
	if (transform) {
		if (MatricesMatchExact(modelTransform, transform->worldTransform)) {
			prevModelTransform = transform->prevWorldTransform;
		}
		else {
			glm::mat4 localTransform = glm::mat4(1.0f);
			glm::mat4 worldInverse = glm::inverse(transform->worldTransform);
			bool validInverse = true;
			for (int c = 0; c < 4 && validInverse; ++c) {
				for (int r = 0; r < 4 && validInverse; ++r) {
					if (!std::isfinite(worldInverse[c][r])) {
						validInverse = false;
					}
				}
			}
			if (validInverse) {
				localTransform = worldInverse * modelTransform;
			}
			prevModelTransform = transform->prevWorldTransform * localTransform;
		}
	}

	if (uniforms.prevModel != -1) {
		glUniformMatrix4fv(uniforms.prevModel, 1, GL_FALSE, glm::value_ptr(prevModelTransform));
	}

	if (uniforms.transformID != -1) {
		const GLuint transformID = transform ? transform->transformID : INVALID_TRANSFORM_ID;
		glUniform1ui(uniforms.transformID, transformID);
	}
}

void RenderSystem::ApplyCullingState(const MeshComponent& mesh, CullingOverride override, const glm::mat4& modelTransform)
{
	GLenum frontFace = GL_CCW;
	const float determinant = glm::determinant(glm::mat3(modelTransform));
	if (std::isfinite(determinant) && determinant < 0.0f) {
		frontFace = GL_CW;
	}
	if (!m_cachedFrontFaceValid || m_cachedFrontFace != frontFace) {
		glFrontFace(frontFace);
		m_cachedFrontFace = frontFace;
		m_cachedFrontFaceValid = true;
	}

	// If force backface culling is enabled globally, always cull back faces
	if (m_forceBackfaceCulling) {
		if (!m_cachedCullEnabledValid || !m_cachedCullEnabled) {
			glEnable(GL_CULL_FACE);
			m_cachedCullEnabled = true;
			m_cachedCullEnabledValid = true;
		}
		if (!m_cachedCullFaceValid || m_cachedCullFace != GL_BACK) {
			glCullFace(GL_BACK);
			m_cachedCullFace = GL_BACK;
			m_cachedCullFaceValid = true;
		}
		return;
	}

	bool enableCulling = true;
	GLenum cullFace = GL_BACK;

	switch (override) {
	case CullingOverride::CULLING_FORCE_ENABLE:
		enableCulling = true;
		cullFace = GL_BACK;
		break;
	case CullingOverride::CULLING_FORCE_DISABLE:
		enableCulling = false;
		break;
	case CullingOverride::CULLING_FORCE_FRONT:
		enableCulling = true;
		cullFace = GL_FRONT;
		break;
	case CullingOverride::CULLING_INHERIT:
	default:
		auto meshCulling = mesh.GetEffectiveCullingMode();
		switch (meshCulling) {
		case MeshComponent::CULL_BACK:
			enableCulling = true;
			cullFace = GL_BACK;
			break;
		case MeshComponent::CULL_FRONT:
			enableCulling = true;
			cullFace = GL_FRONT;
			break;
		case MeshComponent::CULL_NONE:
			enableCulling = false;
			break;
		default:
			break;
		}
		break;
	}

	if (enableCulling) {
		if (!m_cachedCullEnabledValid || !m_cachedCullEnabled) {
			glEnable(GL_CULL_FACE);
			m_cachedCullEnabled = true;
			m_cachedCullEnabledValid = true;
		}
		if (!m_cachedCullFaceValid || m_cachedCullFace != cullFace) {
			glCullFace(cullFace);
			m_cachedCullFace = cullFace;
			m_cachedCullFaceValid = true;
		}
	}
	else {
		if (!m_cachedCullEnabledValid || m_cachedCullEnabled) {
			glDisable(GL_CULL_FACE);
			m_cachedCullEnabled = false;
			m_cachedCullEnabledValid = true;
		}
	}
}

void RenderSystem::ApplyBakedBatchCulling(const MeshComponent& mesh, CullingOverride override, GLenum frontFace)
{
	if (!m_cachedFrontFaceValid || m_cachedFrontFace != frontFace) {
		glFrontFace(frontFace);
		m_cachedFrontFace = frontFace;
		m_cachedFrontFaceValid = true;
	}

	if (m_forceBackfaceCulling) {
		if (!m_cachedCullEnabledValid || !m_cachedCullEnabled) {
			glEnable(GL_CULL_FACE);
			m_cachedCullEnabled = true;
			m_cachedCullEnabledValid = true;
		}
		if (!m_cachedCullFaceValid || m_cachedCullFace != GL_BACK) {
			glCullFace(GL_BACK);
			m_cachedCullFace = GL_BACK;
			m_cachedCullFaceValid = true;
		}
		return;
	}

	bool enableCulling = true;
	GLenum cullFace = GL_BACK;
	switch (override) {
	case CullingOverride::CULLING_FORCE_ENABLE:
		enableCulling = true;
		cullFace = GL_BACK;
		break;
	case CullingOverride::CULLING_FORCE_DISABLE:
		enableCulling = false;
		break;
	case CullingOverride::CULLING_FORCE_FRONT:
		enableCulling = true;
		cullFace = GL_FRONT;
		break;
	case CullingOverride::CULLING_INHERIT:
	default:
		switch (mesh.GetEffectiveCullingMode()) {
		case MeshComponent::CULL_BACK:
			enableCulling = true;
			cullFace = GL_BACK;
			break;
		case MeshComponent::CULL_FRONT:
			enableCulling = true;
			cullFace = GL_FRONT;
			break;
		case MeshComponent::CULL_NONE:
			enableCulling = false;
			break;
		default:
			break;
		}
		break;
	}

	if (enableCulling) {
		if (!m_cachedCullEnabledValid || !m_cachedCullEnabled) {
			glEnable(GL_CULL_FACE);
			m_cachedCullEnabled = true;
			m_cachedCullEnabledValid = true;
		}
		if (!m_cachedCullFaceValid || m_cachedCullFace != cullFace) {
			glCullFace(cullFace);
			m_cachedCullFace = cullFace;
			m_cachedCullFaceValid = true;
		}
	}
	else if (!m_cachedCullEnabledValid || m_cachedCullEnabled) {
		glDisable(GL_CULL_FACE);
		m_cachedCullEnabled = false;
		m_cachedCullEnabledValid = true;
	}
}

void RenderSystem::UploadBoneMatrices(EntityID entity, const glm::mat4& meshWorldTransform, const ShaderUniformCache& uniforms)
{
	auto* renderable = m_componentManager->GetRenderable(entity);
	if (!renderable || !renderable->isSkinned || !renderable->model) return;

	constexpr size_t kMaxShaderBones = 128;
	const size_t numBones = renderable->boneInverseBindMatrices.size();
	if (numBones == 0) return;

	auto cacheIt = m_cachedBoneMatrices.find(entity);
	if (cacheIt == m_cachedBoneMatrices.end()) {
		if (numBones > kMaxShaderBones && m_warnedBoneLimitEntities.insert(entity).second) {
			std::cout << "[RenderSystem] Skinned entity " << entity
				<< " has " << numBones << " bones; clamping to shader limit " << kMaxShaderBones << std::endl;
		}

		const size_t computeBoneCount = std::min(numBones, kMaxShaderBones);
		std::vector<glm::mat4> boneMatrices(computeBoneCount, glm::mat4(1.0f));
		const Scene& model = *renderable->model;
		const AnimationComponent* animation = m_componentManager->GetAnimation(entity);
		const bool useLegacyBoneNodeWorlds = animation && animation->controller && animation->controller->HasActiveAnimations();

		if (useLegacyBoneNodeWorlds) {
			glm::mat4 meshWorldInverse = glm::inverse(meshWorldTransform);
			bool meshInverseValid = true;
			for (int c = 0; c < 4 && meshInverseValid; ++c) {
				for (int r = 0; r < 4 && meshInverseValid; ++r) {
					if (!std::isfinite(meshWorldInverse[c][r])) {
						meshInverseValid = false;
					}
				}
			}
			if (!meshInverseValid) {
				meshWorldInverse = glm::mat4(1.0f);
			}

			for (size_t i = 0; i < computeBoneCount && i < renderable->boneNodes.size(); ++i) {
				if (!renderable->boneNodes[i]) {
					continue;
				}

				glm::mat4 boneWorld = renderable->boneNodes[i]->GetWorldPosition4x4();
				glm::mat4 boneMatrix = meshWorldInverse * boneWorld * renderable->boneInverseBindMatrices[i];
				bool valid = true;
				for (int c = 0; c < 4 && valid; ++c) {
					for (int r = 0; r < 4 && valid; ++r) {
						if (!std::isfinite(boneMatrix[c][r])) {
							valid = false;
						}
					}
				}
				boneMatrices[i] = valid ? boneMatrix : glm::mat4(1.0f);
			}
		}

		if (!useLegacyBoneNodeWorlds) {
			std::unordered_map<int, glm::mat4> nodeWorldCache;

			int meshNodeIndex = ResolveRenderableNodeIndex(*renderable);
			glm::mat4 meshModelTransform = glm::mat4(1.0f);
			if (meshNodeIndex >= 0) {
				meshModelTransform = ComputeAnimatedNodeWorldTransform(model, meshNodeIndex, animation, nodeWorldCache);
			}

			glm::mat4 meshModelInverse = glm::inverse(meshModelTransform);
			bool meshInverseValid = true;
			for (int c = 0; c < 4 && meshInverseValid; ++c) {
				for (int r = 0; r < 4 && meshInverseValid; ++r) {
					if (!std::isfinite(meshModelInverse[c][r])) {
						meshInverseValid = false;
					}
				}
			}
			if (!meshInverseValid) {
				meshModelInverse = glm::mat4(1.0f);
			}

			for (size_t i = 0; i < computeBoneCount; ++i) {
				int jointNodeIndex = (i < model.skin.joints.size()) ? model.skin.joints[i] : -1;
				if (jointNodeIndex < 0 && i < renderable->boneNodes.size() && renderable->boneNodes[i]) {
					jointNodeIndex = renderable->boneNodes[i]->nodeIndex;
				}

				glm::mat4 boneMatrix = glm::mat4(1.0f);
				if (jointNodeIndex >= 0) {
					glm::mat4 jointModelWorld = ComputeAnimatedNodeWorldTransform(model, jointNodeIndex, animation, nodeWorldCache);
					boneMatrix = meshModelInverse * jointModelWorld * renderable->boneInverseBindMatrices[i];
				}

				bool valid = true;
				for (int c = 0; c < 4 && valid; ++c) {
					for (int r = 0; r < 4 && valid; ++r) {
						if (!std::isfinite(boneMatrix[c][r])) {
							valid = false;
						}
					}
				}

				boneMatrices[i] = valid ? boneMatrix : glm::mat4(1.0f);
			}
		}

		m_cachedBoneMatrices.emplace(entity, std::move(boneMatrices));
		cacheIt = m_cachedBoneMatrices.find(entity);
	}

	GLint locBones = uniforms.uBoneMatrices != -1 ? uniforms.uBoneMatrices : uniforms.bones;
	if (locBones != -1 && cacheIt != m_cachedBoneMatrices.end() && !cacheIt->second.empty()) {
		glUniformMatrix4fv(locBones,
			static_cast<GLsizei>(cacheIt->second.size()),
			GL_FALSE,
			glm::value_ptr(cacheIt->second[0]));
	}
}

void RenderSystem::EnsureTransformBuffer()
{
	if (m_transformBuffer && m_transformBuffer->IsValid()) {
		return;
	}

	m_transformBuffer = std::make_unique<GLBuffer>(
		BufferType::ShaderStorage,
		BufferUsage::DynamicDraw
	);
	m_transformBuffer->SetLabel("RenderSystem_GlobalTransformSSBO");
}

void RenderSystem::UpdateGpuTransformBuffer(bool forceFullTransformUpload)
{
	if (!m_componentManager) {
		return;
	}

	const size_t maxTransformID = static_cast<size_t>(m_componentManager->GetMaxAllocatedTransformID());
	auto& transformPool = m_componentManager->GetTransformPool();
	const size_t requiredRecordCount = (maxTransformID == 0) ? 1 : (maxTransformID + 1);
	const bool renderableRevisionChanged =
		(m_lastPreparedRenderableRevision != m_componentManager->GetRenderableRevision());
	const bool forceFullUpload =
		forceFullTransformUpload ||
		!m_transformBuffer ||
		!m_transformBuffer->IsValid() ||
		m_gpuTransformRecords.size() != requiredRecordCount ||
		renderableRevisionChanged;

	if (forceFullUpload) {
		m_gpuTransformRecords.assign(requiredRecordCount, GpuTransformRecord{});
		for (auto& record : m_gpuTransformRecords) {
			record.world = glm::mat4(1.0f);
			record.prevWorld = glm::mat4(1.0f);
			record.metadata = glm::uvec4(kTransformFlagInvalid, 0u, 0u, 0u);
		}

		for (const auto& entry : transformPool) {
			const TransformComponent& transform = entry.component;
			if (transform.transformID == INVALID_TRANSFORM_ID) {
				continue;
			}

			GpuTransformRecord& record = m_gpuTransformRecords[transform.transformID];
			record.world = transform.worldTransform;
			record.prevWorld = transform.prevWorldTransform;
			record.metadata = glm::uvec4(0u);
			record.metadata.z = transform.transformGeneration;

			if (const RenderableComponent* renderable = m_componentManager->GetRenderable(entry.entity)) {
				record.metadata.x |= kTransformFlagRenderable;
				if (renderable->isSkinned) {
					record.metadata.x |= kTransformFlagSkinned;
				}
			}
		}
	}
	else {
		if (m_transformSystem && m_transformSystem->GetLastTransformsRecomputedCount() == 0) {
			return;
		}

		std::vector<TransformID> dirtyTransformIDs;
		const auto& changedEntities = m_transformSystem->GetLastChangedEntities();
		dirtyTransformIDs.reserve(changedEntities.size());

		for (EntityID entity : changedEntities) {
			const TransformComponent* transform = m_componentManager->GetTransform(entity);
			if (!transform || transform->transformID == INVALID_TRANSFORM_ID) {
				continue;
			}
			dirtyTransformIDs.push_back(transform->transformID);
		}

		if (dirtyTransformIDs.empty()) {
			return;
		}

		std::sort(dirtyTransformIDs.begin(), dirtyTransformIDs.end());
		dirtyTransformIDs.erase(std::unique(dirtyTransformIDs.begin(), dirtyTransformIDs.end()),
			dirtyTransformIDs.end());

		for (TransformID transformID : dirtyTransformIDs) {
			GpuTransformRecord& record = m_gpuTransformRecords[transformID];
			record.world = glm::mat4(1.0f);
			record.prevWorld = glm::mat4(1.0f);
			record.metadata = glm::uvec4(kTransformFlagInvalid, 0u, 0u, 0u);
		}

		for (EntityID entity : changedEntities) {
			const TransformComponent* transform = m_componentManager->GetTransform(entity);
			if (!transform || transform->transformID == INVALID_TRANSFORM_ID) {
				continue;
			}

			GpuTransformRecord& record = m_gpuTransformRecords[transform->transformID];
			record.world = transform->worldTransform;
			record.prevWorld = transform->prevWorldTransform;
			record.metadata = glm::uvec4(0u);
			record.metadata.z = transform->transformGeneration;

			if (const RenderableComponent* renderable = m_componentManager->GetRenderable(entity)) {
				record.metadata.x |= kTransformFlagRenderable;
				if (renderable->isSkinned) {
					record.metadata.x |= kTransformFlagSkinned;
				}
			}
		}
	}

	EnsureTransformBuffer();
	if (!m_transformBuffer) {
		return;
	}

	if (forceFullUpload) {
		if (!m_transformBuffer->SetData(m_gpuTransformRecords)) {
			m_transformBuffer = std::make_unique<GLBuffer>(
				BufferType::ShaderStorage,
				BufferUsage::DynamicDraw
			);
			m_transformBuffer->SetLabel("RenderSystem_GlobalTransformSSBO");
			m_transformBuffer->SetData(m_gpuTransformRecords);
		}
		++m_transformUploadCount;
		m_submissionCache.bytesUploaded = m_gpuTransformRecords.size() * sizeof(GpuTransformRecord);
		++m_diagnostics.transformFullUploads;
		m_diagnostics.transformUploadBytes += m_submissionCache.bytesUploaded;
		return;
	}

	std::vector<TransformID> dirtyTransformIDs;
	const auto& changedEntities = m_transformSystem->GetLastChangedEntities();
	dirtyTransformIDs.reserve(changedEntities.size());
	for (EntityID entity : changedEntities) {
		const TransformComponent* transform = m_componentManager->GetTransform(entity);
		if (!transform || transform->transformID == INVALID_TRANSFORM_ID) {
			continue;
		}
		dirtyTransformIDs.push_back(transform->transformID);
	}

	if (dirtyTransformIDs.empty()) {
		return;
	}

	std::sort(dirtyTransformIDs.begin(), dirtyTransformIDs.end());
	dirtyTransformIDs.erase(std::unique(dirtyTransformIDs.begin(), dirtyTransformIDs.end()),
		dirtyTransformIDs.end());

	size_t uploadedBytes = 0;
	size_t segmentStart = static_cast<size_t>(dirtyTransformIDs.front());
	size_t segmentLen = 1;
	for (size_t i = 1; i < dirtyTransformIDs.size(); ++i) {
		const size_t transformID = static_cast<size_t>(dirtyTransformIDs[i]);
		if (transformID == segmentStart + segmentLen) {
			++segmentLen;
			continue;
		}

		m_transformBuffer->SubData(segmentStart * sizeof(GpuTransformRecord),
			segmentLen * sizeof(GpuTransformRecord),
			m_gpuTransformRecords.data() + segmentStart);
		uploadedBytes += segmentLen * sizeof(GpuTransformRecord);
		segmentStart = transformID;
		segmentLen = 1;
	}

	m_transformBuffer->SubData(segmentStart * sizeof(GpuTransformRecord),
		segmentLen * sizeof(GpuTransformRecord),
		m_gpuTransformRecords.data() + segmentStart);
	uploadedBytes += segmentLen * sizeof(GpuTransformRecord);

	++m_transformUploadCount;
	m_submissionCache.bytesUploaded = uploadedBytes;
	++m_diagnostics.transformPartialUploads;
	m_diagnostics.transformUploadBytes += uploadedBytes;
}

void RenderSystem::RebuildRenderItemsIfNeeded()
{
	if (!m_componentManager) {
		return;
	}

	if (m_runtimeScene) {
		m_runtimeScene->EnsureCompiled();
	}

	const uint64_t renderableRevision = m_componentManager->GetRenderableRevision();
	const bool runtimeSizeChanged = m_runtimeScene && (m_runtimeNodeToRenderItems.size() != m_runtimeScene->GetNodeCount());
	if (m_lastRenderItemRevision == renderableRevision && !runtimeSizeChanged && !m_renderItems.empty()) {
		return;
	}

	m_renderItems.clear();
	m_runtimeNodeToRenderItems.clear();
	if (m_runtimeScene) {
		m_runtimeNodeToRenderItems.resize(m_runtimeScene->GetNodeCount());
	}

	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		const EntityID entity = entry.entity;
		const RenderableComponent& renderable = entry.component;
		if (!renderable.model) {
			continue;
		}

		const uint32_t runtimeIndex = m_runtimeScene ? m_runtimeScene->FindRuntimeIndex(entity) : INVALID_RUNTIME_NODE_INDEX;
		uint32_t meshIndex = 0;
		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			RenderItem item;
			item.entity = entity;
			item.runtimeNodeIndex = runtimeIndex;
			item.mesh = &mesh;
			item.model = renderable.model;
			item.localTransform = ResolveRenderableMeshLocalTransform(renderable, mesh);
			item.meshIndex = meshIndex++;
			const MaterialDesc material = mesh.GetMaterialDesc();
			item.materialKey = material.stableMaterialID;
			item.sortKey = (static_cast<uint64_t>(material.stableMaterialID) << 32u) | static_cast<uint64_t>(mesh.VAO);
			item.cullingOverride = renderable.cullingOverride;
			item.transparent = mesh.RequiresAlphaBlending();
			item.skinned = renderable.isSkinned;
			const uint32_t newItemIndex = static_cast<uint32_t>(m_renderItems.size());
			m_renderItems.push_back(item);
			if (runtimeIndex != INVALID_RUNTIME_NODE_INDEX && runtimeIndex < m_runtimeNodeToRenderItems.size()) {
				m_runtimeNodeToRenderItems[runtimeIndex].push_back(newItemIndex);
			}
		});
	}

	const size_t itemCount = m_renderItems.size();
	m_renderItemRuntimeIndices.assign(itemCount, INVALID_RUNTIME_NODE_INDEX);
	m_renderItemLocalCenterX.assign(itemCount, 0.0f);
	m_renderItemLocalCenterY.assign(itemCount, 0.0f);
	m_renderItemLocalCenterZ.assign(itemCount, 0.0f);
	m_renderItemLocalRadius.assign(itemCount, 1.0f);
	m_renderItemWorldCenterX.assign(itemCount, 0.0f);
	m_renderItemWorldCenterY.assign(itemCount, 0.0f);
	m_renderItemWorldCenterZ.assign(itemCount, 0.0f);
	m_renderItemWorldRadius.assign(itemCount, 1.0f);
	m_renderItemVisibleMask.assign(itemCount, 1u);
	m_changedRenderItemScratch.clear();
	m_changedRenderItemScratch.reserve(itemCount);

	for (size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
		const RenderItem& item = m_renderItems[itemIndex];
		m_renderItemRuntimeIndices[itemIndex] = item.runtimeNodeIndex;
		if (item.mesh && item.mesh->boundingVolumeValid) {
			const glm::vec3 localCenter = ComputeTransformedLocalCenter(*item.mesh, item.localTransform);
			m_renderItemLocalCenterX[itemIndex] = localCenter.x;
			m_renderItemLocalCenterY[itemIndex] = localCenter.y;
			m_renderItemLocalCenterZ[itemIndex] = localCenter.z;
			m_renderItemLocalRadius[itemIndex] = ComputeTransformedLocalRadius(*item.mesh, item.localTransform);
		}
	}

	UpdateAllRenderItemBounds();
	m_lastRenderItemRevision = renderableRevision;
	m_lastCameraCachePublication = 0;
	m_lastShadowCachePublication = 0;
}

void RenderSystem::UpdateChangedRenderItemBounds()
{
	if (!m_runtimeScene || !m_transformSystem) {
		UpdateAllRenderItemBounds();
		return;
	}

	const uint64_t publication = m_transformSystem->GetWorldPublicationGeneration();
	if (publication == 0 || publication == m_lastBoundsUpdatePublication) {
		return;
	}

	m_changedRenderItemScratch.clear();
	const auto& changedRuntimeIndices = m_transformSystem->GetLastChangedRuntimeIndices();
	if (changedRuntimeIndices.empty()) {
		UpdateAllRenderItemBounds();
		m_lastBoundsUpdatePublication = publication;
		return;
	}

	for (uint32_t runtimeIndex : changedRuntimeIndices) {
		if (runtimeIndex >= m_runtimeNodeToRenderItems.size()) {
			continue;
		}
		for (uint32_t itemIndex : m_runtimeNodeToRenderItems[runtimeIndex]) {
			m_changedRenderItemScratch.push_back(itemIndex);
		}
	}

	if (m_changedRenderItemScratch.empty()) {
		m_lastBoundsUpdatePublication = publication;
		return;
	}

	std::sort(m_changedRenderItemScratch.begin(), m_changedRenderItemScratch.end());
	m_changedRenderItemScratch.erase(std::unique(m_changedRenderItemScratch.begin(), m_changedRenderItemScratch.end()),
		m_changedRenderItemScratch.end());

	SimdKernels::UpdateWorldBounds(m_simdBackend,
		m_runtimeScene->GetWorldTransforms(),
		m_renderItemRuntimeIndices,
		m_renderItemLocalCenterX.data(),
		m_renderItemLocalCenterY.data(),
		m_renderItemLocalCenterZ.data(),
		m_renderItemLocalRadius.data(),
		m_renderItems.size(),
		m_changedRenderItemScratch.data(),
		m_changedRenderItemScratch.size(),
		m_renderItemWorldCenterX.data(),
		m_renderItemWorldCenterY.data(),
		m_renderItemWorldCenterZ.data(),
		m_renderItemWorldRadius.data());

	m_lastBoundsUpdatePublication = publication;
	m_lastCameraCachePublication = 0;
	m_lastShadowCachePublication = 0;
}

void RenderSystem::UpdateAllRenderItemBounds()
{
	if (m_renderItems.empty()) {
		return;
	}

	if (!m_runtimeScene) {
		for (size_t itemIndex = 0; itemIndex < m_renderItems.size(); ++itemIndex) {
			const RenderItem& item = m_renderItems[itemIndex];
			const glm::mat4 world = ComposeRenderItemWorldTransform(m_transformSystem->GetWorldTransform(item.entity), item);
			const glm::vec3 center = item.mesh && item.mesh->boundingVolumeValid
				? ComputeMeshCenterWS(world, *item.mesh)
				: glm::vec3(world[3]);
			const float radius = item.mesh && item.mesh->boundingVolumeValid
				? ComputeMeshRadiusWS(world, *item.mesh)
				: 1.0f;
			m_renderItemWorldCenterX[itemIndex] = center.x;
			m_renderItemWorldCenterY[itemIndex] = center.y;
			m_renderItemWorldCenterZ[itemIndex] = center.z;
			m_renderItemWorldRadius[itemIndex] = radius;
		}
	}
	else {
		SimdKernels::UpdateWorldBounds(m_simdBackend,
			m_runtimeScene->GetWorldTransforms(),
			m_renderItemRuntimeIndices,
			m_renderItemLocalCenterX.data(),
			m_renderItemLocalCenterY.data(),
			m_renderItemLocalCenterZ.data(),
			m_renderItemLocalRadius.data(),
			m_renderItems.size(),
			nullptr,
			m_renderItems.size(),
			m_renderItemWorldCenterX.data(),
			m_renderItemWorldCenterY.data(),
			m_renderItemWorldCenterZ.data(),
			m_renderItemWorldRadius.data());
	}

	m_lastBoundsUpdatePublication = m_transformSystem ? m_transformSystem->GetWorldPublicationGeneration() : 0;
}

void RenderSystem::PrepareCameraSubmissionCache()
{
	const auto cacheBuildStart = std::chrono::high_resolution_clock::now();
	bool cacheRebuilt = false;

	RebuildRenderItemsIfNeeded();
	if (!m_runtimeScene || !m_transformSystem) {
		UpdateAllRenderItemBounds();
	}

	const uint64_t publication = m_transformSystem ? m_transformSystem->GetWorldPublicationGeneration() : 0;
	if (m_lastCameraCachePublication == publication && m_submissionCache.frustumSerial == m_frustumSerial) {
		m_diagnostics.renderItemCount = m_renderItems.size();
		m_diagnostics.visibleAllCount = m_submissionCache.visibleAllItems.items.size();
		m_diagnostics.visibleOpaqueCount = m_submissionCache.visibleOpaqueItems.items.size();
		m_diagnostics.visibleTransparentCount = m_submissionCache.visibleTransparentItems.items.size();
		m_diagnostics.frustumCulledCount = m_renderItems.size() >= m_submissionCache.visibleAllItems.items.size()
			? (m_renderItems.size() - m_submissionCache.visibleAllItems.items.size())
			: 0;
		return;
	}
	cacheRebuilt = true;

	m_submissionCache.visibleAllItems.Clear();
	m_submissionCache.visibleOpaqueItems.Clear();
	m_submissionCache.sortedOpaqueItems.Clear();
	m_submissionCache.visibleTransparentItems.Clear();
	m_submissionCache.velocityItems.Clear();

	if (m_renderItems.empty()) {
		m_lastCameraCachePublication = publication;
		m_submissionCache.frustumSerial = m_frustumSerial;
		return;
	}

	if (m_frustumValid) {
		SimdFrustumPlanes frustum{};
		for (size_t plane = 0; plane < 6; ++plane) {
			frustum.nx[plane] = m_frustumPlanes[plane].x;
			frustum.ny[plane] = m_frustumPlanes[plane].y;
			frustum.nz[plane] = m_frustumPlanes[plane].z;
			frustum.d[plane] = m_frustumPlanes[plane].w;
		}

		SimdKernels::CullSpheres(m_simdBackend,
			frustum,
			m_renderItemWorldCenterX.data(),
			m_renderItemWorldCenterY.data(),
			m_renderItemWorldCenterZ.data(),
			m_renderItemWorldRadius.data(),
			m_renderItems.size(),
			m_renderItemVisibleMask.data());
	}
	else {
		std::fill(m_renderItemVisibleMask.begin(), m_renderItemVisibleMask.end(), 1u);
	}

	for (uint32_t itemIndex = 0; itemIndex < m_renderItems.size(); ++itemIndex) {
		if (m_frustumValid && m_renderItemVisibleMask[itemIndex] == 0u) {
			continue;
		}

		const RenderItem& item = m_renderItems[itemIndex];
		m_submissionCache.visibleAllItems.items.push_back(itemIndex);
		m_submissionCache.velocityItems.items.push_back(itemIndex);
		if (item.transparent) {
			m_submissionCache.visibleTransparentItems.items.push_back(itemIndex);
		}
		else {
			m_submissionCache.visibleOpaqueItems.items.push_back(itemIndex);
		}
	}

	m_submissionCache.sortedOpaqueItems.items = m_submissionCache.visibleOpaqueItems.items;
	std::sort(m_submissionCache.sortedOpaqueItems.items.begin(), m_submissionCache.sortedOpaqueItems.items.end(),
		[this](uint32_t a, uint32_t b) {
			const RenderItem& itemA = m_renderItems[a];
			const RenderItem& itemB = m_renderItems[b];
			if (itemA.sortKey != itemB.sortKey) {
				return itemA.sortKey < itemB.sortKey;
			}
			return itemA.entity < itemB.entity;
		});

	m_submissionCache.cameraBuildGeneration = publication;
	m_submissionCache.frustumSerial = m_frustumSerial;
	m_submissionCache.drawCount = m_submissionCache.visibleAllItems.items.size();
	m_lastCameraCachePublication = publication;

	m_diagnostics.renderItemCount = m_renderItems.size();
	m_diagnostics.visibleAllCount = m_submissionCache.visibleAllItems.items.size();
	m_diagnostics.visibleOpaqueCount = m_submissionCache.visibleOpaqueItems.items.size();
	m_diagnostics.visibleTransparentCount = m_submissionCache.visibleTransparentItems.items.size();
	m_diagnostics.frustumCulledCount = m_renderItems.size() >= m_submissionCache.visibleAllItems.items.size()
		? (m_renderItems.size() - m_submissionCache.visibleAllItems.items.size())
		: 0;
	if (cacheRebuilt) {
		const auto cacheBuildEnd = std::chrono::high_resolution_clock::now();
		m_diagnostics.cameraCacheBuildMs += std::chrono::duration<float, std::milli>(cacheBuildEnd - cacheBuildStart).count();
	}
}

void RenderSystem::PrepareShadowSubmissionCache(const glm::mat4& lightSpaceMatrix)
{
	const auto cacheBuildStart = std::chrono::high_resolution_clock::now();
	bool cacheRebuilt = false;

	RebuildRenderItemsIfNeeded();
	const uint64_t publication = m_transformSystem ? m_transformSystem->GetWorldPublicationGeneration() : 0;
	if (m_lastShadowCachePublication == publication && MatricesMatchExact(m_lastShadowLightSpace, lightSpaceMatrix)) {
		m_diagnostics.shadowVisibleCount = m_submissionCache.shadowVisibleItems.items.size();
		return;
	}
	cacheRebuilt = true;

	m_submissionCache.shadowVisibleItems.Clear();
	for (uint32_t itemIndex = 0; itemIndex < m_renderItems.size(); ++itemIndex) {
		const glm::vec3 center(
			m_renderItemWorldCenterX[itemIndex],
			m_renderItemWorldCenterY[itemIndex],
			m_renderItemWorldCenterZ[itemIndex]);
		const float radius = m_renderItemWorldRadius[itemIndex];
		const glm::vec4 clip = lightSpaceMatrix * glm::vec4(center, 1.0f);
		bool visible = true;
		if (clip.w != 0.0f) {
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			const float margin = 0.2f * (1.0f + std::abs(ndc.z) * 0.5f) + radius * 0.01f;
			visible = ndc.x >= -1.0f - margin && ndc.x <= 1.0f + margin &&
				ndc.y >= -1.0f - margin && ndc.y <= 1.0f + margin &&
				ndc.z >= -1.0f - margin && ndc.z <= 1.0f + margin;
		}

		if (visible) {
			m_submissionCache.shadowVisibleItems.items.push_back(itemIndex);
		}
	}

	m_submissionCache.shadowBuildGeneration = publication;
	m_lastShadowCachePublication = publication;
	m_lastShadowLightSpace = lightSpaceMatrix;
	m_diagnostics.shadowVisibleCount = m_submissionCache.shadowVisibleItems.items.size();
	if (cacheRebuilt) {
		const auto cacheBuildEnd = std::chrono::high_resolution_clock::now();
		m_diagnostics.shadowCacheBuildMs += std::chrono::duration<float, std::milli>(cacheBuildEnd - cacheBuildStart).count();
	}
}

void RenderSystem::SortRenderQueue(const glm::vec3& cameraPos)
{
	// Sort back to front for transparent objects
	std::sort(m_renderQueue.begin(), m_renderQueue.end(),
		[](const RenderBatch& a, const RenderBatch& b) {
			if (a.distanceToCamera != b.distanceToCamera) {
				return a.distanceToCamera > b.distanceToCamera;
			}
			return a.sortKey < b.sortKey;
		});
}

void RenderSystem::ResetMaterialStateCache(GLuint shader)
{
	m_cachedMaterialShader = shader;
	m_cachedMaterialID = std::numeric_limits<uint32_t>::max();
	m_boundMaterialTextures.fill(std::numeric_limits<GLuint>::max());
	m_cachedFrontFaceValid = false;
	m_cachedCullFaceValid = false;
	m_cachedCullEnabledValid = false;
}

void RenderSystem::BuildInstanceGroups()
{
	if (!m_componentManager || !m_transformSystem) return;

	m_instanceGroups.clear();

	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		EntityID entityID = entry.entity;
		auto& renderable = entry.component;

		if (!renderable.model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entityID);

		// Group by VAO + shader combination
		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			// Skip transparent meshes (they need special sorting)
			if (mesh.RequiresAlphaBlending()) {
				return;
			}

			uint64_t key = ComputeMeshKey(mesh.VAO, renderable.shaderID);

			auto& group = m_instanceGroups[key];
			if (group.transforms.empty()) {
				group.vao = mesh.VAO;
				group.shaderID = renderable.shaderID;
				group.indexCount = mesh.indexCount;
			}

			group.transforms.push_back(worldTransform);
			group.entities.push_back(entityID);
		});
	}
}

void RenderSystem::RenderInstancedGroup(const InstanceGroup& group, GLuint shader)
{
	if (group.transforms.empty()) return;

	// Update instance buffer
	EnsureInstanceBuffer(group.transforms.size());

	glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0,
		group.transforms.size() * sizeof(glm::mat4),
		group.transforms.data());

	// Bind VAO
	glBindVertexArray(group.vao);

	// Set up instanced matrix attribute (uses locations 6-9 for mat4)
	for (int i = 0; i < 4; ++i) {
		GLuint loc = 6 + i;
		glEnableVertexAttribArray(loc);
		glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE,
			sizeof(glm::mat4),
			(void*)(sizeof(glm::vec4) * i));
		glVertexAttribDivisor(loc, 1); // One per instance
	}

	// Draw instanced
	glDrawElementsInstanced(GL_TRIANGLES,
		static_cast<GLsizei>(group.indexCount),
		GL_UNSIGNED_INT,
		nullptr,
		static_cast<GLsizei>(group.transforms.size()));

	// Reset vertex attrib divisors
	for (int i = 0; i < 4; ++i) {
		glVertexAttribDivisor(6 + i, 0);
		glDisableVertexAttribArray(6 + i);
	}

	glBindVertexArray(0);
}

uint64_t RenderSystem::ComputeMeshKey(GLuint vao, GLuint shaderID) const
{
	return (static_cast<uint64_t>(vao) << 32) | static_cast<uint64_t>(shaderID);
}

void RenderSystem::EnsureInstanceBuffer(size_t requiredSize)
{
	size_t requiredBytes = requiredSize * sizeof(glm::mat4);

	if (m_instanceVBO == 0) {
		glGenBuffers(1, &m_instanceVBO);
		m_instanceBufferCapacity = 0;
	}

	if (requiredBytes > m_instanceBufferCapacity) {
		// Grow buffer with some headroom
		size_t newCapacity = std::max(requiredBytes * 2, size_t(256 * sizeof(glm::mat4)));

		glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
		glBufferData(GL_ARRAY_BUFFER, newCapacity, nullptr, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		m_instanceBufferCapacity = newCapacity;
	}
}

bool RenderSystem::ShouldUseBakedGBufferPath() const
{
	if (!m_runtimeScene) {
		return false;
	}

	const size_t total = m_renderItems.size();
	const size_t visibleOpaque = m_submissionCache.visibleOpaqueItems.items.size();
	if (total < 512 || visibleOpaque < 256) {
		return false;
	}

	return visibleOpaque * 100 >= total * 65;
}

uint64_t RenderSystem::ComputeBakedBatchKey(const MeshComponent& mesh, CullingOverride override, GLenum frontFace) const
{
	const MaterialDesc material = mesh.GetMaterialDesc();
	auto textureID = [](const std::shared_ptr<Texture>& texture, GLuint fallback) {
		return (texture && texture->IsValid()) ? texture->ID() : fallback;
	};
	auto mix = [](uint64_t seed, uint64_t value) {
		return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
	};

	uint64_t key = material.stableMaterialID;
	key = mix(key, static_cast<uint64_t>(override));
	key = mix(key, static_cast<uint64_t>(mesh.GetEffectiveCullingMode()));
	key = mix(key, static_cast<uint64_t>(frontFace));
	key = mix(key, textureID(mesh.diffuseTexture, DefaultTextures::White()));
	key = mix(key, textureID(mesh.normalTexture, DefaultTextures::Normal()));
	key = mix(key, textureID(mesh.roughnessTexture, DefaultTextures::MetallicRoughnessDefault()));
	key = mix(key, textureID(mesh.emissiveTexture, DefaultTextures::Black()));
	key = mix(key, textureID(mesh.occlusionTexture, DefaultTextures::AOWhite()));
	key = mix(key, textureID(mesh.specularTexture, DefaultTextures::White()));
	key = mix(key, textureID(mesh.specularColorTexture, DefaultTextures::White()));
	key = mix(key, textureID(mesh.transmissionTexture, DefaultTextures::Black()));
	return key;
}

void RenderSystem::ClearBakedGBufferBatches()
{
	for (BakedGBufferBatch& batch : m_bakedGBufferBatches) {
		if (batch.vao != 0) {
			glDeleteVertexArrays(1, &batch.vao);
			batch.vao = 0;
		}
	}
	m_bakedGBufferBatches.clear();
}

void RenderSystem::UpdateBakedGBufferTransformBuffer()
{
	if (!m_runtimeScene || !m_transformSystem) {
		return;
	}

	const size_t nodeCount = m_runtimeScene->GetNodeCount();
	if (nodeCount == 0) {
		return;
	}

	const uint64_t publication = m_transformSystem->GetWorldPublicationGeneration();
	if (m_bakedGBufferTransformBuffer &&
		m_bakedGBufferTransformBuffer->IsValid() &&
		m_bakedGBufferTransformPublication == publication &&
		m_bakedGBufferTransformRecords.size() == nodeCount) {
		return;
	}

	m_bakedGBufferTransformRecords.resize(nodeCount);
	for (uint32_t runtimeIndex = 0; runtimeIndex < nodeCount; ++runtimeIndex) {
		GpuTransformRecord& record = m_bakedGBufferTransformRecords[runtimeIndex];
		record.world = m_runtimeScene->GetWorldTransformByIndex(runtimeIndex);
		record.prevWorld = m_runtimeScene->GetPreviousWorldTransformByIndex(runtimeIndex);
		record.metadata = glm::uvec4(0u);
		if (const RuntimeTransformNode* node = m_runtimeScene->GetRuntimeNodeByIndex(runtimeIndex)) {
			record.metadata.z = node->worldGeneration;
		}
	}

	const size_t requiredBytes = m_bakedGBufferTransformRecords.size() * sizeof(GpuTransformRecord);
	if (!m_bakedGBufferTransformBuffer || !m_bakedGBufferTransformBuffer->IsValid()) {
		m_bakedGBufferTransformBuffer = std::make_unique<GLBuffer>(
			BufferType::ShaderStorage,
			requiredBytes,
			m_bakedGBufferTransformRecords.data(),
			BufferUsage::DynamicDraw);
		m_bakedGBufferTransformBuffer->SetLabel("RenderSystem_BakedGBufferRuntimeTransformSSBO");
	}
	else if (m_bakedGBufferTransformBuffer->GetSize() == requiredBytes) {
		m_bakedGBufferTransformBuffer->SubData(0, requiredBytes, m_bakedGBufferTransformRecords.data());
	}
	else {
		m_bakedGBufferTransformBuffer->SetData(m_bakedGBufferTransformRecords);
	}

	m_bakedGBufferTransformPublication = publication;
}

void RenderSystem::RebuildBakedGBufferBatches()
{
	const uint64_t renderableRevision = m_componentManager ? m_componentManager->GetRenderableRevision() : 0;
	if (!m_bakedGBufferBatches.empty() &&
		m_bakedGBufferRenderableRevision == renderableRevision) {
		return;
	}

	ClearBakedGBufferBatches();

	struct BuildBatch {
		std::vector<BakedGBufferVertex> vertices;
		std::vector<uint32_t> indices;
		const MeshComponent* materialMesh = nullptr;
		CullingOverride cullingOverride = CullingOverride::CULLING_INHERIT;
		GLenum frontFace = GL_CCW;
	};

	std::unordered_map<uint64_t, BuildBatch> buildBatches;
	buildBatches.reserve(m_renderItems.size() / 4);

	for (const RenderItem& item : m_renderItems) {
		if (!item.mesh || item.transparent || item.skinned) {
			continue;
		}

		const MeshComponent& mesh = *item.mesh;
		if (mesh.rawVertices.empty() || mesh.rawIndices.empty() || mesh.morphTargetCount > 0 ||
			item.runtimeNodeIndex == INVALID_RUNTIME_NODE_INDEX) {
			continue;
		}

		const TransformComponent* transform = m_componentManager ? m_componentManager->GetTransform(item.entity) : nullptr;
		if (!transform || transform->transformID == INVALID_TRANSFORM_ID) {
			continue;
		}

		const glm::mat4& localTransform = item.localTransform;
		const float determinant = glm::determinant(glm::mat3(localTransform));
		const GLenum frontFace = (std::isfinite(determinant) && determinant < 0.0f) ? GL_CW : GL_CCW;
		const uint64_t key = ComputeBakedBatchKey(mesh, item.cullingOverride, frontFace);
		BuildBatch& batch = buildBatches[key];
		if (!batch.materialMesh) {
			batch.materialMesh = &mesh;
			batch.cullingOverride = item.cullingOverride;
			batch.frontFace = frontFace;
		}

		const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(localTransform)));
		const uint32_t baseVertex = static_cast<uint32_t>(batch.vertices.size());

		batch.vertices.reserve(batch.vertices.size() + mesh.rawVertices.size());
		for (const Vertex& source : mesh.rawVertices) {
			BakedGBufferVertex baked{};
			baked.vertex = source;
			baked.vertex.position = glm::vec3(localTransform * glm::vec4(source.position, 1.0f));
			baked.vertex.normal = glm::normalize(normalMatrix * source.normal);
			baked.vertex.tangent = glm::vec4(glm::normalize(normalMatrix * glm::vec3(source.tangent)), source.tangent.w);
			baked.runtimeTransformIndex = item.runtimeNodeIndex;
			baked.stableTransformID = transform->transformID;
			batch.vertices.push_back(baked);
		}

		batch.indices.reserve(batch.indices.size() + mesh.rawIndices.size());
		for (uint32_t index : mesh.rawIndices) {
			batch.indices.push_back(baseVertex + index);
		}
	}

	m_bakedGBufferBatches.reserve(buildBatches.size());
	for (auto& [_, buildBatch] : buildBatches) {
		if (buildBatch.vertices.empty() || buildBatch.indices.empty() || !buildBatch.materialMesh) {
			continue;
		}

		BakedGBufferBatch batch{};
		batch.indexCount = buildBatch.indices.size();
		batch.materialMesh = buildBatch.materialMesh;
		batch.cullingOverride = buildBatch.cullingOverride;
		batch.frontFace = buildBatch.frontFace;

		glGenVertexArrays(1, &batch.vao);
		glBindVertexArray(batch.vao);

		batch.vertexBuffer = std::make_unique<GLBuffer>(
			BufferType::Vertex,
			buildBatch.vertices.size() * sizeof(BakedGBufferVertex),
			buildBatch.vertices.data(),
			BufferUsage::StaticDraw);
		batch.indexBuffer = std::make_unique<GLBuffer>(
			BufferType::Index,
			buildBatch.indices.size() * sizeof(uint32_t),
			buildBatch.indices.data(),
			BufferUsage::StaticDraw);

		const GLsizei stride = sizeof(BakedGBufferVertex);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, vertex) + offsetof(Vertex, position)));
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, vertex) + offsetof(Vertex, normal)));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, vertex) + offsetof(Vertex, texCoord)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(10, 2, GL_FLOAT, GL_FALSE, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, vertex) + offsetof(Vertex, texCoord1)));
		glEnableVertexAttribArray(10);
		glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, vertex) + offsetof(Vertex, tangent)));
		glEnableVertexAttribArray(3);
		glVertexAttribIPointer(4, 4, GL_INT, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, vertex) + offsetof(Vertex, boneIDs)));
		glEnableVertexAttribArray(4);
		glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, vertex) + offsetof(Vertex, boneWeights)));
		glEnableVertexAttribArray(5);
		glVertexAttribIPointer(11, 1, GL_UNSIGNED_INT, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, runtimeTransformIndex)));
		glEnableVertexAttribArray(11);
		glVertexAttribIPointer(12, 1, GL_UNSIGNED_INT, stride,
			reinterpret_cast<void*>(offsetof(BakedGBufferVertex, stableTransformID)));
		glEnableVertexAttribArray(12);
		glBindVertexArray(0);
		m_bakedGBufferBatches.push_back(std::move(batch));
	}

	m_bakedGBufferRenderableRevision = renderableRevision;
	m_bakedGBufferTransformPublication = 0;
}
