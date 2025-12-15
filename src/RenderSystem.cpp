#include "RenderSystem.h"
#include "Scene.h"
#include "MeshComponent.h"
#include "DefaultTextures.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <iostream>

RenderSystem::RenderSystem(ComponentManager* componentManager, TransformSystem* transformSystem)
	: m_componentManager(componentManager)
	, m_transformSystem(transformSystem)
{
	m_renderQueue.reserve(256);
}

void RenderSystem::RenderForward(const glm::mat4& view,
	const glm::mat4& projection,
	GLuint defaultShader)
{
	if (!m_componentManager || !m_transformSystem) return;

	// Update all transforms first
	m_transformSystem->UpdateTransforms();

	glUseProgram(defaultShader);

	// Upload view and projection matrices once
	GLint locView = glGetUniformLocation(defaultShader, "view");
	GLint locProj = glGetUniformLocation(defaultShader, "projection");
	if (locView != -1) glUniformMatrix4fv(locView, 1, GL_FALSE, glm::value_ptr(view));
	if (locProj != -1) glUniformMatrix4fv(locProj, 1, GL_FALSE, glm::value_ptr(projection));

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
		GLint locModel = glGetUniformLocation(defaultShader, "model");
		if (locModel != -1) {
			glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
		}

		// Handle skinning
		if (renderable.isSkinned) {
			UploadBoneMatrices(entityID, defaultShader);
			GLint locUseSkin = glGetUniformLocation(defaultShader, "u_enableSkinning");
			if (locUseSkin != -1) glUniform1i(locUseSkin, 1);
		}
		else {
			GLint locUseSkin = glGetUniformLocation(defaultShader, "u_enableSkinning");
			if (locUseSkin != -1) glUniform1i(locUseSkin, 0);
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

			BindMaterialTextures(mesh, defaultShader);
			UploadMaterialUniforms(mesh, defaultShader);

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
	glUseProgram(geometryShader);

	m_visibleCount = 0;
	m_totalCount = 0;

	auto& renderablePool = m_componentManager->GetRenderablePool();
	size_t poolSize = renderablePool.Size();

	std::cout << "[RenderSystem] RenderGeometry - Renderable pool size: " << poolSize << std::endl;

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

		GLint locModel = glGetUniformLocation(geometryShader, "model");
		if (locModel != -1) {
			glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
		}

		// Handle skinning
		if (renderable.isSkinned) {
			UploadBoneMatrices(entityID, geometryShader);
			GLint locUseSkin = glGetUniformLocation(geometryShader, "u_enableSkinning");
			if (locUseSkin != -1) glUniform1i(locUseSkin, 1);
		}
		else {
			GLint locUseSkin = glGetUniformLocation(geometryShader, "u_enableSkinning");
			if (locUseSkin != -1) glUniform1i(locUseSkin, 0);
		}

		for (auto& mesh : renderable.model->meshes) {
			// Skip transparent meshes in deferred pass
			if (mesh.RequiresAlphaBlending()) continue;

			ApplyCullingState(mesh, renderable.cullingOverride);
			BindMaterialTextures(mesh, geometryShader);
			UploadMaterialUniforms(mesh, geometryShader);

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
	glUseProgram(shadowShader);

	GLint locLS = glGetUniformLocation(shadowShader, "lightSpaceMatrix");
	if (locLS != -1) {
		glUniformMatrix4fv(locLS, 1, GL_FALSE, glm::value_ptr(lightSpaceMatrix));
	}

	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		EntityID entityID = entry.entity;
		auto& renderable = entry.component;

		if (!renderable.model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entityID);

		GLint locModel = glGetUniformLocation(shadowShader, "model");
		if (locModel != -1) {
			glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
		}

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
	glUseProgram(velocityShader);

	// Upload current and previous view/projection
	GLint locView = glGetUniformLocation(velocityShader, "view");
	GLint locProj = glGetUniformLocation(velocityShader, "projection");
	GLint locPrevView = glGetUniformLocation(velocityShader, "prevView");
	GLint locPrevProj = glGetUniformLocation(velocityShader, "prevProjection");

	if (locView != -1) glUniformMatrix4fv(locView, 1, GL_FALSE, glm::value_ptr(view));
	if (locProj != -1) glUniformMatrix4fv(locProj, 1, GL_FALSE, glm::value_ptr(projection));
	if (locPrevView != -1) glUniformMatrix4fv(locPrevView, 1, GL_FALSE, glm::value_ptr(prevView));
	if (locPrevProj != -1) glUniformMatrix4fv(locPrevProj, 1, GL_FALSE, glm::value_ptr(prevProjection));

	auto& renderablePool = m_componentManager->GetRenderablePool();
	for (auto& entry : renderablePool) {
		EntityID entityID = entry.entity;
		auto& renderable = entry.component;

		if (!renderable.model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(entityID);

		GLint locModel = glGetUniformLocation(velocityShader, "model");
		GLint locPrevModel = glGetUniformLocation(velocityShader, "prevModel");

		if (locModel != -1) {
			glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
		}
		// For now, use same transform for previous (TODO: store previous frame transforms)
		if (locPrevModel != -1) {
			glUniformMatrix4fv(locPrevModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
		}

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
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);

	GLint locView = glGetUniformLocation(transparentShader, "view");
	GLint locProj = glGetUniformLocation(transparentShader, "projection");
	if (locView != -1) glUniformMatrix4fv(locView, 1, GL_FALSE, glm::value_ptr(view));
	if (locProj != -1) glUniformMatrix4fv(locProj, 1, GL_FALSE, glm::value_ptr(projection));

	for (const auto& batch : m_renderQueue) {
		auto* renderable = m_componentManager->GetRenderable(batch.entity);
		if (!renderable || !renderable->model) continue;

		const glm::mat4& worldTransform = m_transformSystem->GetWorldTransform(batch.entity);

		GLint locModel = glGetUniformLocation(transparentShader, "model");
		if (locModel != -1) {
			glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(worldTransform));
		}

		for (auto& mesh : renderable->model->meshes) {
			if (!mesh.RequiresAlphaBlending()) continue;

			ApplyCullingState(mesh, renderable->cullingOverride);
			BindMaterialTextures(mesh, transparentShader);
			UploadMaterialUniforms(mesh, transparentShader);

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

void RenderSystem::BindMaterialTextures(const MeshComponent& mesh, GLuint shader)
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

	GLint uBase = glGetUniformLocation(shader, "texture_diffuse");
	GLint uNorm = glGetUniformLocation(shader, "texture_normal");
	GLint uMR = glGetUniformLocation(shader, "texture_metallic_roughness");
	GLint uEmis = glGetUniformLocation(shader, "texture_emissive");
	GLint uAO = glGetUniformLocation(shader, "texture_occlusion");

	bindTexture(uBase, 0, mesh.diffuseTexture, DefaultTextures::White());
	bindTexture(uNorm, 1, mesh.normalTexture, DefaultTextures::Normal());
	bindTexture(uMR, 2, mesh.roughnessTexture, DefaultTextures::MetallicRoughnessDefault());
	bindTexture(uEmis, 3, mesh.emissiveTexture, DefaultTextures::Black());
	bindTexture(uAO, 4, mesh.occlusionTexture, DefaultTextures::AOWhite());

	// Texture presence flags
	GLint locHasBase = glGetUniformLocation(shader, "hasBaseColorTexture");
	GLint locHasNorm = glGetUniformLocation(shader, "hasNormalTexture");
	GLint locHasMR = glGetUniformLocation(shader, "hasMetallicRoughnessTexture");
	GLint locHasEmis = glGetUniformLocation(shader, "hasEmissiveTexture");
	GLint locHasOcc = glGetUniformLocation(shader, "hasOcclusionTexture");

	if (locHasBase >= 0) glUniform1i(locHasBase, (mesh.diffuseTexture && mesh.diffuseTexture->IsValid()) ? 1 : 0);
	if (locHasNorm >= 0) glUniform1i(locHasNorm, (mesh.normalTexture && mesh.normalTexture->IsValid()) ? 1 : 0);
	if (locHasMR >= 0) glUniform1i(locHasMR, (mesh.roughnessTexture && mesh.roughnessTexture->IsValid()) ? 1 : 0);
	if (locHasEmis >= 0) glUniform1i(locHasEmis, (mesh.emissiveTexture && mesh.emissiveTexture->IsValid()) ? 1 : 0);
	if (locHasOcc >= 0) glUniform1i(locHasOcc, (mesh.occlusionTexture && mesh.occlusionTexture->IsValid()) ? 1 : 0);
}

void RenderSystem::UploadMaterialUniforms(const MeshComponent& mesh, GLuint shader)
{
	GLint locBaseColor = glGetUniformLocation(shader, "baseColorFactor");
	GLint locMetallic = glGetUniformLocation(shader, "metallicFactor");
	GLint locRoughness = glGetUniformLocation(shader, "roughnessFactor");
	GLint locEmissive = glGetUniformLocation(shader, "emissiveFactor");
	GLint locOccStr = glGetUniformLocation(shader, "occlusionStrength");
	GLint locNormScale = glGetUniformLocation(shader, "normalScale");

	if (locBaseColor >= 0) glUniform4fv(locBaseColor, 1, glm::value_ptr(mesh.baseColorFactor));
	if (locMetallic >= 0) glUniform1f(locMetallic, mesh.metallicFactor);
	if (locRoughness >= 0) glUniform1f(locRoughness, mesh.roughnessFactor);
	if (locEmissive >= 0) glUniform3fv(locEmissive, 1, glm::value_ptr(mesh.emissiveFactor));
	if (locOccStr >= 0) glUniform1f(locOccStr, mesh.occlusionStrength);
	if (locNormScale >= 0) glUniform1f(locNormScale, mesh.normalScale);
}

void RenderSystem::ApplyCullingState(const MeshComponent& mesh, CullingOverride override)
{
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

void RenderSystem::UploadBoneMatrices(EntityID entity, GLuint shader)
{
	// TODO: Get bone matrices from animation component when available
	// For now, this is a placeholder
	auto* renderable = m_componentManager->GetRenderable(entity);
	if (!renderable || renderable->boneInverseBindMatrices.empty()) return;

	GLint locBones = glGetUniformLocation(shader, "u_boneMatrices");
	if (locBones == -1) {
		locBones = glGetUniformLocation(shader, "bones");
	}

	if (locBones != -1) {
		// Use identity matrices as placeholder
		std::vector<glm::mat4> boneMats(renderable->boneInverseBindMatrices.size(), glm::mat4(1.0f));
		size_t numBones = std::min(boneMats.size(), size_t(128));
		glUniformMatrix4fv(locBones, static_cast<GLsizei>(numBones), GL_FALSE, glm::value_ptr(boneMats[0]));
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
