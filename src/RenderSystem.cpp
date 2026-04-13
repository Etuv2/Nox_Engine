#include "RenderSystem.h"
#include "Scene.h"
#include "SceneNode.h"
#include "MeshComponent.h"
#include "DefaultTextures.h"
#include "TextureUnits.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <iostream>
#include <cmath>

namespace {
	constexpr GLuint kGlobalTransformBufferBinding = 6;
	constexpr uint32_t kTransformFlagRenderable = 1u << 0;
	constexpr uint32_t kTransformFlagSkinned = 1u << 1;

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
	uniforms.specularFactor = glGetUniformLocation(shader, "specularFactor");
	uniforms.specularColorFactor = glGetUniformLocation(shader, "specularColorFactor");
	uniforms.clearcoatFactor = glGetUniformLocation(shader, "clearcoatFactor");
	uniforms.clearcoatRoughnessFactor = glGetUniformLocation(shader, "clearcoatRoughnessFactor");
	uniforms.transmissionFactor = glGetUniformLocation(shader, "transmissionFactor");
	uniforms.thicknessFactor = glGetUniformLocation(shader, "thicknessFactor");
	uniforms.attenuationDistance = glGetUniformLocation(shader, "attenuationDistance");
	uniforms.attenuationColor = glGetUniformLocation(shader, "attenuationColor");
	uniforms.ior = glGetUniformLocation(shader, "ior");

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
	ResetMaterialStateCache(defaultShader);
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
		// Draw each mesh in the model
		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			UploadTransformUniforms(entityID, worldTransform, uniforms);

			if (renderable.isSkinned) {
				UploadBoneMatrices(entityID, worldTransform, uniforms);
				if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 1);
			}
			else if (uniforms.uEnableSkinning != -1) {
				glUniform1i(uniforms.uEnableSkinning, 0);
			}

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
		});
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
	ResetMaterialStateCache(geometryShader);
	EnsureTransformBuffer();
	if (m_transformBuffer) {
		m_transformBuffer->BindBase(kGlobalTransformBufferBinding);
	}
	const auto& uniforms = GetShaderUniformCache(geometryShader);

	m_visibleCount = 0;
	m_totalCount = 0;

	auto& renderablePool = m_componentManager->GetRenderablePool();
	size_t poolSize = renderablePool.Size();
	struct OpaqueDraw {
		EntityID entity = INVALID_ENTITY;
		const MeshComponent* mesh = nullptr;
		glm::mat4 worldTransform{ 1.0f };
		uint64_t sortKey = 0;
	};
	std::vector<OpaqueDraw> opaqueDraws;
	opaqueDraws.reserve(poolSize * 2);

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

		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			// Skip transparent meshes in deferred pass
			if (mesh.RequiresAlphaBlending()) {
				return;
			}

			OpaqueDraw draw;
			draw.entity = entityID;
			draw.mesh = &mesh;
			draw.worldTransform = worldTransform;
			draw.sortKey = (static_cast<uint64_t>(mesh.material.stableMaterialID) << 32u) | static_cast<uint64_t>(mesh.VAO);
			opaqueDraws.push_back(draw);
		});
	}

	std::sort(opaqueDraws.begin(), opaqueDraws.end(),
		[](const OpaqueDraw& a, const OpaqueDraw& b) {
			if (a.sortKey != b.sortKey) {
				return a.sortKey < b.sortKey;
			}
			return a.entity < b.entity;
		});

	GLuint currentVAO = 0;
	for (const OpaqueDraw& draw : opaqueDraws) {
		auto* renderable = m_componentManager->GetRenderable(draw.entity);
		if (!renderable || !draw.mesh) {
			continue;
		}

		UploadTransformUniforms(draw.entity, draw.worldTransform, uniforms);
		if (renderable->isSkinned) {
			UploadBoneMatrices(draw.entity, draw.worldTransform, uniforms);
			if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 1);
		}
		else if (uniforms.uEnableSkinning != -1) {
			glUniform1i(uniforms.uEnableSkinning, 0);
		}

		ApplyCullingState(*draw.mesh, renderable->cullingOverride);
		BindMaterialTextures(*draw.mesh, uniforms);
		UploadMaterialUniforms(*draw.mesh, uniforms);

		if (currentVAO != draw.mesh->VAO) {
			glBindVertexArray(draw.mesh->VAO);
			currentVAO = draw.mesh->VAO;
		}
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(draw.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
	}

	if (currentVAO != 0) {
		glBindVertexArray(0);
	}
}

void RenderSystem::RenderShadowCascade(const glm::mat4& lightSpaceMatrix, GLuint shadowShader)
{
	if (!m_componentManager || !m_transformSystem) return;

	m_transformSystem->UpdateTransforms();
	UpdateGpuTransformBuffer();
	glUseProgram(shadowShader);
	ResetMaterialStateCache(shadowShader);
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
		GLuint currentVAO = 0;
		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			UploadTransformUniforms(entityID, worldTransform, uniforms);
			if (renderable.isSkinned) {
				UploadBoneMatrices(entityID, worldTransform, uniforms);
				if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 1);
			}
			else if (uniforms.uEnableSkinning != -1) {
				glUniform1i(uniforms.uEnableSkinning, 0);
			}

			ApplyCullingState(mesh, renderable.cullingOverride);
			if (currentVAO != mesh.VAO) {
				glBindVertexArray(mesh.VAO);
				currentVAO = mesh.VAO;
			}
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
		});
		if (currentVAO != 0) {
			glBindVertexArray(0);
		}
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
	ResetMaterialStateCache(velocityShader);
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

		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			UploadTransformUniforms(entityID, worldTransform, uniforms);
			if (renderable.isSkinned) {
				UploadBoneMatrices(entityID, worldTransform, uniforms);
				if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 1);
			}
			else if (uniforms.uEnableSkinning != -1) {
				glUniform1i(uniforms.uEnableSkinning, 0);
			}

			ApplyCullingState(mesh, renderable.cullingOverride);

			glBindVertexArray(mesh.VAO);
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
			glBindVertexArray(0);
		});
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

		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			MDI_RenderableObject obj{};
			obj.count = static_cast<GLuint>(mesh.indexCount);
			obj.firstIndex = 0;
			obj.baseVertex = 0;
			obj.modelMatrix = worldTransform;
			if (const TransformComponent* transform = m_componentManager->GetTransform(entityID)) {
				obj.transformID = transform->transformID;
			}
			obj.vao = mesh.VAO;

			if (mesh.boundingVolumeValid) {
				obj.boundingSphere = glm::vec4(mesh.boundingCenter, mesh.boundingRadius);
			}
			else {
				obj.boundingSphere = glm::vec4(0.0f, 0.0f, 0.0f, renderable.boundingRadius);
			}

			batch.AddObject(obj);
		});
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

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entry.entity);
		ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
			if (!mesh.RequiresAlphaBlending()) {
				return;
			}

			glm::vec3 centerWS = glm::vec3(worldTransform * glm::vec4(mesh.boundingCenter, 1.0f));
			if (m_frustumValid && !IsSphereVisible(centerWS, mesh.boundingRadius)) {
				return;
			}
			float dist = glm::length(centerWS - cameraPos);
			uint64_t sortKey =
				(static_cast<uint64_t>(mesh.material.stableMaterialID) << 32u) |
				static_cast<uint64_t>(mesh.VAO);
			m_renderQueue.push_back({ entry.entity, &mesh, dist, sortKey, true });
		});
	}

	// Sort back to front
	SortRenderQueue(cameraPos);

	// Render
	glUseProgram(transparentShader);
	ResetMaterialStateCache(transparentShader);
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

	GLuint currentVAO = 0;
	for (const auto& batch : m_renderQueue) {
		auto* renderable = m_componentManager->GetRenderable(batch.entity);
		if (!renderable || !renderable->model || !batch.mesh) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(batch.entity);

		UploadTransformUniforms(batch.entity, worldTransform, uniforms);
		if (renderable->isSkinned) {
			UploadBoneMatrices(batch.entity, worldTransform, uniforms);
			if (uniforms.uEnableSkinning != -1) glUniform1i(uniforms.uEnableSkinning, 1);
		}
		else if (uniforms.uEnableSkinning != -1) {
			glUniform1i(uniforms.uEnableSkinning, 0);
		}

		ApplyCullingState(*batch.mesh, renderable->cullingOverride);
		BindMaterialTextures(*batch.mesh, uniforms);
		UploadMaterialUniforms(*batch.mesh, uniforms);

		if (currentVAO != batch.mesh->VAO) {
			glBindVertexArray(batch.mesh->VAO);
			currentVAO = batch.mesh->VAO;
		}
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
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
	auto bindTexture = [this](GLint loc, GLuint unit, const std::shared_ptr<Texture>& tex, GLuint fallback) {
		const GLuint desiredTexture = (tex && tex->IsValid()) ? tex->ID() : fallback;
		if (m_boundMaterialTextures[unit] != desiredTexture) {
			glActiveTexture(GL_TEXTURE0 + unit);
			glBindTexture(GL_TEXTURE_2D, desiredTexture);
			m_boundMaterialTextures[unit] = desiredTexture;
		}
		if (loc >= 0) glUniform1i(loc, unit);
		};

	bindTexture(uniforms.textureDiffuse, TextureUnits::MATERIAL_BASE_COLOR, mesh.diffuseTexture, DefaultTextures::White());
	bindTexture(uniforms.textureNormal, TextureUnits::MATERIAL_NORMAL, mesh.normalTexture, DefaultTextures::Normal());
	bindTexture(uniforms.textureMetallicRoughness, TextureUnits::MATERIAL_METALLIC_ROUGHNESS, mesh.roughnessTexture, DefaultTextures::MetallicRoughnessDefault());
	bindTexture(uniforms.textureEmissive, TextureUnits::MATERIAL_EMISSIVE, mesh.emissiveTexture, DefaultTextures::Black());
	bindTexture(uniforms.textureOcclusion, TextureUnits::MATERIAL_OCCLUSION, mesh.occlusionTexture, DefaultTextures::AOWhite());
	bindTexture(uniforms.textureSpecular, TextureUnits::MATERIAL_SPECULAR, mesh.specularTexture, DefaultTextures::White());
	bindTexture(uniforms.textureSpecularColor, TextureUnits::MATERIAL_SPECULAR_COLOR, mesh.specularColorTexture, DefaultTextures::White());
	bindTexture(uniforms.textureTransmission, TextureUnits::MATERIAL_TRANSMISSION, mesh.transmissionTexture, DefaultTextures::Black());

	if (uniforms.hasBaseColorTexture >= 0) glUniform1i(uniforms.hasBaseColorTexture, (mesh.diffuseTexture && mesh.diffuseTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasNormalTexture >= 0) glUniform1i(uniforms.hasNormalTexture, (mesh.normalTexture && mesh.normalTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasMetallicRoughnessTexture >= 0) glUniform1i(uniforms.hasMetallicRoughnessTexture, (mesh.roughnessTexture && mesh.roughnessTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasEmissiveTexture >= 0) glUniform1i(uniforms.hasEmissiveTexture, (mesh.emissiveTexture && mesh.emissiveTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasOcclusionTexture >= 0) glUniform1i(uniforms.hasOcclusionTexture, (mesh.occlusionTexture && mesh.occlusionTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasSpecularTexture >= 0) glUniform1i(uniforms.hasSpecularTexture, (mesh.specularTexture && mesh.specularTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasSpecularColorTexture >= 0) glUniform1i(uniforms.hasSpecularColorTexture, (mesh.specularColorTexture && mesh.specularColorTexture->IsValid()) ? 1 : 0);
	if (uniforms.hasTransmissionTexture >= 0) glUniform1i(uniforms.hasTransmissionTexture, (mesh.transmissionTexture && mesh.transmissionTexture->IsValid()) ? 1 : 0);
}

void RenderSystem::UploadMaterialUniforms(const MeshComponent& mesh, const ShaderUniformCache& uniforms)
{
	const MaterialDesc& material = mesh.material;
	if (m_cachedMaterialID == material.stableMaterialID) {
		return;
	}

	if (uniforms.materialID >= 0) glUniform1ui(uniforms.materialID, material.stableMaterialID);
	if (uniforms.baseColorFactor >= 0) glUniform4fv(uniforms.baseColorFactor, 1, glm::value_ptr(material.baseColorFactor));
	if (uniforms.metallicFactor >= 0) glUniform1f(uniforms.metallicFactor, material.metallicFactor);
	if (uniforms.roughnessFactor >= 0) glUniform1f(uniforms.roughnessFactor, material.roughnessFactor);
	if (uniforms.emissiveFactor >= 0) glUniform3fv(uniforms.emissiveFactor, 1, glm::value_ptr(material.emissiveFactor));
	if (uniforms.emissiveStrength >= 0) glUniform1f(uniforms.emissiveStrength, material.emissiveStrength);
	if (uniforms.occlusionStrength >= 0) glUniform1f(uniforms.occlusionStrength, material.occlusionStrength);
	if (uniforms.normalScale >= 0) glUniform1f(uniforms.normalScale, material.normalScale);
	if (uniforms.alphaCutoff >= 0) glUniform1f(uniforms.alphaCutoff, material.alphaCutoff);
	if (uniforms.specularFactor >= 0) glUniform1f(uniforms.specularFactor, material.specularFactor.x);
	if (uniforms.specularColorFactor >= 0) glUniform3fv(uniforms.specularColorFactor, 1, glm::value_ptr(material.specularColorFactor));
	if (uniforms.clearcoatFactor >= 0) glUniform1f(uniforms.clearcoatFactor, material.clearcoatFactor);
	if (uniforms.clearcoatRoughnessFactor >= 0) glUniform1f(uniforms.clearcoatRoughnessFactor, material.clearcoatRoughnessFactor);
	if (uniforms.transmissionFactor >= 0) glUniform1f(uniforms.transmissionFactor, material.transmissionFactor);
	if (uniforms.thicknessFactor >= 0) glUniform1f(uniforms.thicknessFactor, material.thicknessFactor);
	if (uniforms.attenuationDistance >= 0) glUniform1f(uniforms.attenuationDistance, material.attenuationDistance);
	if (uniforms.attenuationColor >= 0) glUniform3fv(uniforms.attenuationColor, 1, glm::value_ptr(material.attenuationColor));
	if (uniforms.ior >= 0) glUniform1f(uniforms.ior, material.ior);
	m_cachedMaterialID = material.stableMaterialID;
}

void RenderSystem::UploadTransformUniforms(EntityID entity,
	const glm::mat4& modelTransform,
	const ShaderUniformCache& uniforms)
{
	if (uniforms.model != -1) {
		glUniformMatrix4fv(uniforms.model, 1, GL_FALSE, glm::value_ptr(modelTransform));
	}

	const TransformComponent* transform = m_componentManager ? m_componentManager->GetTransform(entity) : nullptr;
	glm::mat4 prevModelTransform = modelTransform;
	if (transform) {
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

	if (uniforms.prevModel != -1) {
		glUniformMatrix4fv(uniforms.prevModel, 1, GL_FALSE, glm::value_ptr(prevModelTransform));
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

void RenderSystem::UploadBoneMatrices(EntityID entity, const glm::mat4& meshWorldTransform, const ShaderUniformCache& uniforms)
{
	auto* renderable = m_componentManager->GetRenderable(entity);
	if (!renderable || !renderable->isSkinned) return;

	// Compute bone matrices using the actual bone hierarchy
	std::vector<glm::mat4> boneMatrices;
	size_t numBones = renderable->boneInverseBindMatrices.size();
	if (numBones == 0) return;

	boneMatrices.resize(numBones, glm::mat4(1.0f));

	// Get the skinned mesh's world transform (for proper coordinate space)
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
