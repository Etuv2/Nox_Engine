#include "RenderSystem.h"
#include "Scene.h"
#include "SceneNode.h"
#include "MeshComponent.h"
#include "DefaultTextures.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <iostream>
#include <cmath>

namespace {
	constexpr GLuint kGlobalTransformBufferBinding = 6;
	constexpr uint32_t kTransformFlagRenderable = 1u << 0;
	constexpr uint32_t kTransformFlagSkinned = 1u << 1;
}

RenderSystem::RenderSystem(ComponentManager* componentManager, TransformSystem* transformSystem)
	: m_componentManager(componentManager)
	, m_transformSystem(transformSystem)
{
	m_renderQueue.reserve(256);
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
	uniforms.lightSpaceMatrix = glGetUniformLocation(shader, "lightSpaceMatrix");
	uniforms.uEnableSkinning = glGetUniformLocation(shader, "u_enableSkinning");
	uniforms.uBoneMatrices = glGetUniformLocation(shader, "u_boneMatrices");
	uniforms.bones = glGetUniformLocation(shader, "bones");

	uniforms.textureDiffuse = glGetUniformLocation(shader, "texture_diffuse");
	uniforms.textureNormal = glGetUniformLocation(shader, "texture_normal");
	uniforms.textureMetallicRoughness = glGetUniformLocation(shader, "texture_metallic_roughness");
	uniforms.textureEmissive = glGetUniformLocation(shader, "texture_emissive");
	uniforms.textureOcclusion = glGetUniformLocation(shader, "texture_occlusion");

	uniforms.hasBaseColorTexture = glGetUniformLocation(shader, "hasBaseColorTexture");
	uniforms.hasNormalTexture = glGetUniformLocation(shader, "hasNormalTexture");
	uniforms.hasMetallicRoughnessTexture = glGetUniformLocation(shader, "hasMetallicRoughnessTexture");
	uniforms.hasEmissiveTexture = glGetUniformLocation(shader, "hasEmissiveTexture");
	uniforms.hasOcclusionTexture = glGetUniformLocation(shader, "hasOcclusionTexture");

	uniforms.baseColorFactor = glGetUniformLocation(shader, "baseColorFactor");
	uniforms.metallicFactor = glGetUniformLocation(shader, "metallicFactor");
	uniforms.roughnessFactor = glGetUniformLocation(shader, "roughnessFactor");
	uniforms.emissiveFactor = glGetUniformLocation(shader, "emissiveFactor");
	uniforms.occlusionStrength = glGetUniformLocation(shader, "occlusionStrength");
	uniforms.normalScale = glGetUniformLocation(shader, "normalScale");

	auto [insertedIt, _] = m_shaderUniformCaches.emplace(shader, uniforms);
	return insertedIt->second;
}

void RenderSystem::RenderForward(const glm::mat4& view,
	const glm::mat4& projection,
	GLuint defaultShader)
{
	if (!m_componentManager || !m_transformSystem) return;

	// Update all transforms first
	m_transformSystem->UpdateTransforms();
	UpdateGpuTransformBuffer();

	glUseProgram(defaultShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}

	// Upload view and projection matrices once
		const auto& uniforms = GetShaderUniformCache(defaultShader);
	if (uniforms.view != -1) glUniformMatrix4fv(uniforms.view, 1, GL_FALSE, glm::value_ptr(view));
	if (uniforms.projection != -1) glUniformMatrix4fv(uniforms.projection, 1, GL_FALSE, glm::value_ptr(projection));

	m_visibleCount = 0;
	m_totalCount = 0;

	// Iterate through all renderable components
	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		EntityID entityID = entry.entity;
		auto& renderable = entry.component;
		m_totalCount++;

		// Skip entities without models
		if (!renderable.model) continue;

		// Get world transform
		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entityID);

		// Frustum culling
		if (m_frustumValid) {
			glm::vec3 center = glm::vec3(worldTransform[3]);
			if (!IsSphereVisible(center, renderable.boundingRadius)) {
				continue; // Culled
			}
		}

		m_visibleCount++;

		// Upload model matrix
		UploadTransformUniforms(entityID, worldTransform, uniforms);

		// Handle skinning
		if (renderable.isSkinned) {
			UploadBoneMatrices(entityID, uniforms);
			if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 1);
		}
		else {
			if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 0);
		}

		// Draw each mesh in the model
		for (auto& mesh : renderable.model->meshes) {
			ApplyCullingState(mesh, renderable.cullingOverride);

			// Handle alpha blending
			if (mesh.RequiresAlphaBlending()) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glDepthMask(GL_FALSE);
			}
			else {
				glDisable(GL_BLEND);
				glDepthMask(GL_TRUE);
			}

			BindMaterialTextures(mesh, uniforms);
			UploadMaterialUniforms(mesh, uniforms);

			glBindVertexArray(mesh.VAO);
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
			glBindVertexArray(0);

			// Reset blend state
			if (mesh.RequiresAlphaBlending()) {
				glDisable(GL_BLEND);
				glDepthMask(GL_TRUE);
			}
		}
	}
}

void RenderSystem::RenderGeometry(GLuint geometryShader)
{
	if (!m_componentManager || !m_transformSystem) {
		std::cerr << "[RenderSystem] ERROR: Missing component manager or transform system!" << std::endl;
		return;
	}

	m_transformSystem->UpdateTransforms();
	UpdateGpuTransformBuffer();
	glUseProgram(geometryShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(geometryShader);

	m_visibleCount = 0;
	m_totalCount = 0;

	auto& renderablePool = m_componentManager->GetRenderablePool();
	size_t poolSize = renderablePool.Size();

	if constexpr (VerboseLogging) {
		if (m_runtimeVerboseLogging) {
			std::cout << "[RenderSystem] RenderGeometry - Renderable pool size: " << poolSize << std::endl;
		}
	}

	for (auto& entry : renderablePool) {
		EntityID entityID = entry.entity;
		auto& renderable = entry.component;
		m_totalCount++;

		if (!renderable.model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entityID);

		// Frustum culling
		if (m_frustumValid) {
			glm::vec3 center = glm::vec3(worldTransform[3]);
			if (!IsSphereVisible(center, renderable.boundingRadius)) {
				continue;
			}
		}

		m_visibleCount++;

		UploadTransformUniforms(entityID, worldTransform, uniforms);

		// Handle skinning
		if (renderable.isSkinned) {
			UploadBoneMatrices(entityID, uniforms);
			if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 1);
		}
		else {
			if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 0);
		}

		for (auto& mesh : renderable.model->meshes) {
			// Skip transparent meshes in deferred pass
			if (mesh.RequiresAlphaBlending()) continue;

			ApplyCullingState(mesh, renderable.cullingOverride);
			BindMaterialTextures(mesh, uniforms);
			UploadMaterialUniforms(mesh, uniforms);

			glBindVertexArray(mesh.VAO);
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
			glBindVertexArray(0);
		}
	}
}

void RenderSystem::RenderShadowCascade(const glm::mat4& lightSpaceMatrix, GLuint shadowShader)
{
	if (!m_componentManager || !m_transformSystem) return;

	m_transformSystem->UpdateTransforms();
	UpdateGpuTransformBuffer();
	glUseProgram(shadowShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(shadowShader);

	if (uniforms.lightSpaceMatrix != -1) {
		glUniformMatrix4fv(uniforms.lightSpaceMatrix, 1, GL_FALSE, glm::value_ptr(lightSpaceMatrix));
	}

	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		EntityID entityID = entry.entity;
		auto& renderable = entry.component;

		if (!renderable.model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entityID);

		UploadTransformUniforms(entityID, worldTransform, uniforms);

		// Draw model (positions only for shadow pass)
		renderable.model->Draw();
	}
}

void RenderSystem::RenderVelocity(const glm::mat4& view,
	const glm::mat4& projection,
	const glm::mat4& prevView,
	const glm::mat4& prevProjection,
	GLuint velocityShader)
{
	if (!m_componentManager || !m_transformSystem) return;

	m_transformSystem->UpdateTransforms();
	UpdateGpuTransformBuffer();
	glUseProgram(velocityShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(velocityShader);

	if (uniforms.view != -1) glUniformMatrix4fv(uniforms.view, 1, GL_FALSE, glm::value_ptr(view));
	if (uniforms.projection != -1) glUniformMatrix4fv(uniforms.projection, 1, GL_FALSE, glm::value_ptr(projection));
	if (uniforms.prevView != -1) glUniformMatrix4fv(uniforms.prevView, 1, GL_FALSE, glm::value_ptr(prevView));
	if (uniforms.prevProjection != -1) glUniformMatrix4fv(uniforms.prevProjection, 1, GL_FALSE, glm::value_ptr(prevProjection));

	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		EntityID entityID = entry.entity;
		auto& renderable = entry.component;

		if (!renderable.model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entityID);

		UploadTransformUniforms(entityID, worldTransform, uniforms);

		for (auto& mesh : renderable.model->meshes) {
			ApplyCullingState(mesh, renderable.cullingOverride);

			glBindVertexArray(mesh.VAO);
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
			glBindVertexArray(0);
		}
	}
}

void RenderSystem::CollectRenderables(MDIBatch& batch)
{
	if (!m_componentManager || !m_transformSystem) return;

	m_transformSystem->UpdateTransforms();
	UpdateGpuTransformBuffer();

	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		EntityID entityID = entry.entity;
		auto& renderable = entry.component;

		if (!renderable.model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entityID);

		for (const auto& mesh : renderable.model->meshes) {
			MDI_RenderableObject obj{};
			obj.count = static_cast<GLuint>(mesh.indexCount);
			obj.firstIndex = 0;
			obj.baseVertex = 0;
			obj.modelMatrix = worldTransform;
			obj.vao = mesh.VAO;

			if (mesh.boundingVolumeValid) {
				obj.boundingSphere = glm::vec4(mesh.boundingCenter, mesh.boundingRadius);
			}
			else {
				obj.boundingSphere = glm::vec4(0.0f, 0.0f, 0.0f, renderable.boundingRadius);
			}

			batch.AddObject(obj);
		}
	}
}

void RenderSystem::RenderTransparent(const glm::mat4& view,
	const glm::mat4& projection,
	GLuint transparentShader)
{
	if (!m_componentManager || !m_transformSystem) return;
	m_transformSystem->UpdateTransforms();
	UpdateGpuTransformBuffer();

	glm::vec3 cameraPos = glm::vec3(glm::inverse(view)[3]);

	// Collect transparent objects
	m_renderQueue.clear();

	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		auto& renderable = entry.component;
		if (!renderable.model) continue;

		// Check if any mesh is transparent
		bool hasTransparent = false;
		for (const auto& mesh : renderable.model->meshes) {
			if (mesh.RequiresAlphaBlending()) {
				hasTransparent = true;
				break;
			}
		}

		if (hasTransparent) {
			const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entry.entity);
			glm::vec3 objPos = glm::vec3(worldTransform[3]);
			float dist = glm::length(objPos - cameraPos);

			m_renderQueue.push_back({ entry.entity, dist, true });
		}
	}

	// Sort back to front
	SortRenderQueue(cameraPos);

	// Render
	glUseProgram(transparentShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(transparentShader);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);

		if (uniforms.view != -1) glUniformMatrix4fv(uniforms.view, 1, GL_FALSE, glm::value_ptr(view));
	if (uniforms.projection != -1) glUniformMatrix4fv(uniforms.projection, 1, GL_FALSE, glm::value_ptr(projection));

	for (const auto& batch : m_renderQueue) {
		auto* renderable = m_componentManager->GetRenderable(batch.entity);
		if (!renderable || !renderable->model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(batch.entity);

		UploadTransformUniforms(batch.entity, worldTransform, uniforms);

		for (auto& mesh : renderable->model->meshes) {
			if (!mesh.RequiresAlphaBlending()) continue;

			ApplyCullingState(mesh, renderable->cullingOverride);
			BindMaterialTextures(mesh, uniforms);
			UploadMaterialUniforms(mesh, uniforms);

			glBindVertexArray(mesh.VAO);
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
			glBindVertexArray(0);
		}
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
	auto bindTexture = [](GLint loc, GLuint unit, const std::shared_ptr<Texture>& tex, GLuint fallback) {
		glActiveTexture(GL_TEXTURE0 + unit);
		if (tex && tex->IsValid()) {
			tex->Bind(GL_TEXTURE0 + unit);
		}
		else {
			glBindTexture(GL_TEXTURE_2D, fallback);
		}
		if (loc >= 0) glUniform1i(loc, unit);
		};

		bindTexture(uniforms.textureDiffuse, 0, mesh.diffuseTexture, DefaultTextures::White());
	bindTexture(uniforms.textureNormal, 1, mesh.normalTexture, DefaultTextures::Normal());
	bindTexture(uniforms.textureMetallicRoughness, 2, mesh.roughnessTexture, DefaultTextures::MetallicRoughnessDefault());
	bindTexture(uniforms.textureEmissive, 3, mesh.emissiveTexture, DefaultTextures::Black());
	bindTexture(uniforms.textureOcclusion, 4, mesh.occlusionTexture, DefaultTextures::AOWhite());

	if (uniforms.hasBaseColorTexture >= 0) glUniform1i(uniforms.hasBaseColorTexture, (mesh.diffuseTexture && mesh.diffuseTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasNormalTexture >= 0) glUniform1i(uniforms.hasNormalTexture, (mesh.normalTexture && mesh.normalTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasMetallicRoughnessTexture >= 0) glUniform1i(uniforms.hasMetallicRoughnessTexture, (mesh.roughnessTexture && mesh.roughnessTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasEmissiveTexture >= 0) glUniform1i(uniforms.hasEmissiveTexture, (mesh.emissiveTexture && mesh.emissiveTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasOcclusionTexture >= 0) glUniform1i(uniforms.hasOcclusionTexture, (mesh.occlusionTexture && mesh.occlusionTexture->IsValid()) ? 1 : 0);
}

void RenderSystem::UploadMaterialUniforms(const MeshComponent& mesh, const ShaderUniformCache& uniforms)
{
	if (uniforms.baseColorFactor >= 0) glUniform4fv(uniforms.baseColorFactor, 1, glm::value_ptr(mesh.baseColorFactor));
	if (uniforms.metallicFactor >= 0) glUniform1f(uniforms.metallicFactor, mesh.metallicFactor);
	if (uniforms.roughnessFactor >= 0) glUniform1f(uniforms.roughnessFactor, mesh.roughnessFactor);
	if (uniforms.emissiveFactor >= 0) glUniform3fv(uniforms.emissiveFactor, 1, glm::value_ptr(mesh.emissiveFactor));
	if (uniforms.occlusionStrength >= 0) glUniform1f(uniforms.occlusionStrength, mesh.occlusionStrength);
	if (uniforms.normalScale >= 0) glUniform1f(uniforms.normalScale, mesh.normalScale);
}

void RenderSystem::UploadTransformUniforms(EntityID entity,
	const glm::mat4& worldTransform,
	const ShaderUniformCache& uniforms)
{
	if (uniforms.model != -1) {
		glUniformMatrix4fv(uniforms.model, 1, GL_FALSE, glm::value_ptr(worldTransform));
	}

	const TransformComponent* transform = m_componentManager ? m_componentManager->GetTransform(entity) : nullptr;
	const glm::mat4& prevWorldTransform = transform ? transform->prevWorldTransform : worldTransform;

	if (uniforms.prevModel != -1) {
		glUniformMatrix4fv(uniforms.prevModel, 1, GL_FALSE, glm::value_ptr(prevWorldTransform));
	}

	if (uniforms.transformID != -1) {
		const GLuint transformID = transform ? transform->transformID : INVALID_TRANSFORM_ID;
		glUniform1ui(uniforms.transformID, transformID);
	}
}

void RenderSystem::ApplyCullingState(const MeshComponent& mesh, CullingOverride override)
{
	// If force backface culling is enabled globally, always cull back faces
	if (m_forceBackfaceCulling) {
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
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
		glEnable(GL_CULL_FACE);
		glCullFace(cullFace);
	}
	else {
		glDisable(GL_CULL_FACE);
	}
}

void RenderSystem::UploadBoneMatrices(EntityID entity, const ShaderUniformCache& uniforms)
{
	auto* renderable = m_componentManager->GetRenderable(entity);
	if (!renderable || !renderable->isSkinned) return;

	// Compute bone matrices using the actual bone hierarchy
	std::vector<glm::mat4> boneMatrices;
	size_t numBones = renderable->boneInverseBindMatrices.size();
	if (numBones == 0) return;

	boneMatrices.resize(numBones, glm::mat4(1.0f));

	// Get the skinned mesh's world transform (for proper coordinate space)
	glm::mat4 meshWorldTransform = m_transformSystem->GetWorldTransform(entity);
	glm::mat4 meshWorldInverse = glm::inverse(meshWorldTransform);

	// Validate meshWorldInverse - if the mesh transform is degenerate, use identity
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

	// Compute final bone matrices:
	// boneMatrix[i] = inverse(meshWorld) * boneWorld[i] * inverseBindMatrix[i]
	for (size_t i = 0; i < numBones && i < renderable->boneNodes.size(); ++i) {
		if (renderable->boneNodes[i]) {
			// Get world transform of bone node (includes animation)
			glm::mat4 boneWorld = renderable->boneNodes[i]->GetWorldPosition4x4();

			// Compute final bone matrix
			glm::mat4 boneMatrix = meshWorldInverse * boneWorld * renderable->boneInverseBindMatrices[i];

			// Validate the bone matrix - if any component is NaN or Inf, use identity
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

	// Upload to shader
	GLint locBones = uniforms.uBoneMatrices != -1 ? uniforms.uBoneMatrices : uniforms.bones;

	if (locBones != -1) {
		size_t uploadCount = std::min(boneMatrices.size(), size_t(128));
		glUniformMatrix4fv(locBones, static_cast<GLsizei>(uploadCount), GL_FALSE, glm::value_ptr(boneMatrices[0]));
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

void RenderSystem::UpdateGpuTransformBuffer()
{
	if (!m_componentManager) {
		return;
	}

	size_t maxTransformID = 0;
	auto& transformPool = m_componentManager->GetTransformPool();
	for (const auto& entry : transformPool) {
		maxTransformID = std::max(maxTransformID, static_cast<size_t>(entry.component.transformID));
	}

	if (maxTransformID == 0) {
		m_gpuTransformRecords.assign(1, GpuTransformRecord{});
	}
	else {
		m_gpuTransformRecords.assign(maxTransformID + 1, GpuTransformRecord{});
	}

	for (auto& record : m_gpuTransformRecords) {
		record.world = glm::mat4(1.0f);
		record.prevWorld = glm::mat4(1.0f);
		record.metadata = glm::uvec4(0u);
	}

	for (const auto& entry : transformPool) {
		const TransformComponent& transform = entry.component;
		if (transform.transformID == INVALID_TRANSFORM_ID) {
			continue;
		}

		GpuTransformRecord& record = m_gpuTransformRecords[transform.transformID];
		record.world = transform.worldTransform;
		record.prevWorld = transform.prevWorldTransform;
		record.metadata.z = transform.transformGeneration;

		if (const RenderableComponent* renderable = m_componentManager->GetRenderable(entry.entity)) {
			record.metadata.x |= kTransformFlagRenderable;
			if (renderable->isSkinned) {
				record.metadata.x |= kTransformFlagSkinned;
			}
		}
	}

	EnsureTransformBuffer();
	if (!m_transformBuffer) {
		return;
	}

	if (!m_transformBuffer->SetData(m_gpuTransformRecords)) {
		m_transformBuffer = std::make_unique<GLBuffer>(
			BufferType::ShaderStorage,
			BufferUsage::DynamicDraw
		);
		m_transformBuffer->SetLabel("RenderSystem_GlobalTransformSSBO");
		m_transformBuffer->SetData(m_gpuTransformRecords);
	}
}

void RenderSystem::SortRenderQueue(const glm::vec3& cameraPos)
{
	// Sort back to front for transparent objects
	std::sort(m_renderQueue.begin(), m_renderQueue.end(),
		[](const RenderBatch& a, const RenderBatch& b) {
			return a.distanceToCamera > b.distanceToCamera;
		});
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
		for (const auto& mesh : renderable.model->meshes) {
			// Skip transparent meshes (they need special sorting)
			if (mesh.RequiresAlphaBlending()) continue;

			uint64_t key = ComputeMeshKey(mesh.VAO, renderable.shaderID);

			auto& group = m_instanceGroups[key];
			if (group.transforms.empty()) {
				group.vao = mesh.VAO;
				group.shaderID = renderable.shaderID;
				group.indexCount = mesh.indexCount;
			}

			group.transforms.push_back(worldTransform);
			group.entities.push_back(entityID);
		}
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
