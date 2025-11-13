#include "SceneNode.h"
#include "Scene.h"
#include "RigidBody.h"
#include "SceneGraph.h" // Include for BVH dirty tracking
#include "DefaultTextures.h"
#include "AudioNode.h"
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GL/glew.h>
#include <iostream>
#include <algorithm>

// Static member initialization
ComponentManager* SceneNode::s_globalComponentManager = nullptr;
TransformSystem* SceneNode::s_globalTransformSystem = nullptr;
SceneGraph* SceneNode::s_globalSceneGraph = nullptr;

// Static method implementations
void SceneNode::SetGlobalComponentManager(ComponentManager* manager) {
	s_globalComponentManager = manager;
}

void SceneNode::SetGlobalTransformSystem(TransformSystem* transformSystem) {
	s_globalTransformSystem = transformSystem;
}

void SceneNode::SetGlobalSceneGraph(SceneGraph* sceneGraph) {
	s_globalSceneGraph = sceneGraph;
}

SceneNode::SceneNode()
	: transform(1.0f),
	animatedTransform(1.0f),
	m_shader(0),
	boundingRadius(1.0f),
	m_isAnimationPlaying(false),
	m_isAnimationPaused(false),
	m_animationTime(0.0f),
	m_currentAnimationIndex(-1),
	isSkinned(false),
	nodeIndex(-1),
	m_nodeType(NODE),
	m_cullingOverride(CULLING_INHERIT),
	m_updatingFromPhysics(false),
	m_transformCacheDirty(false),
	m_entityID(0),
	m_componentManager(s_globalComponentManager),
	m_transformSystem(s_globalTransformSystem)
{
}

SceneNode::SceneNode(ComponentManager* manager, TransformSystem* transformSystem, EntityID entityID)
	: transform(1.0f),
	animatedTransform(1.0f),
	m_shader(0),
	boundingRadius(1.0f),
	m_isAnimationPlaying(false),
	m_isAnimationPaused(false),
	m_animationTime(0.0f),
	m_currentAnimationIndex(-1),
	isSkinned(false),
	nodeIndex(-1),
	m_nodeType(NODE),
	m_cullingOverride(CULLING_INHERIT),
	m_updatingFromPhysics(false),
	m_transformCacheDirty(false),
	m_entityID(entityID),
	m_componentManager(manager),
	m_transformSystem(transformSystem)
{
}

void SceneNode::SetModel(const std::shared_ptr<Scene>& model) {
	m_model = model;

	// Auto-initialize animation controller if the model has animations
	if (model && !model->animations.empty() && !m_animationController) {
		m_animationController = std::make_shared<AnimationController>();
		std::cout << "[SceneNode] Auto-initialized animation controller for model with "
			<< model->animations.size() << " animations" << std::endl;
	}
}

std::shared_ptr<Scene> SceneNode::GetModel() const {
	return m_model;
}

void SceneNode::SetShader(unsigned int shader) {
	m_shader = shader;
}

void SceneNode::AddChild(const std::shared_ptr<SceneNode>& child) {
	child->parentNode = shared_from_this();
	children.push_back(child);

	// Mark transform cache as dirty for the new child
	child->InvalidateTransformCache();
}

std::shared_ptr<SceneNode> SceneNode::GetChild(int index) {
	if (index < 0 || index >= (int)children.size()) {
		return nullptr;
	}
	return children[index];
}

std::string SceneNode::GetName() {
	if (!m_model) {
		switch (m_nodeType) {
		case NODE:  return "Node";
		case MODEL: return "Mesh";
		case AUDIO: return "AudioPlayer";
		case LIGHT: return "LightSource";
		case CAMERA:return "Camera";
		default:  return "Unknown";
		}
	}
	return m_model->GetName();
}

//Add unified world position calculation method
glm::vec3 SceneNode::GetWorldPosition() const
{
	// Get parent's world transform if we have a parent
	glm::mat4 parentWorld(1.0f);
	if (auto parent = parentNode.lock()) {
		parentWorld = parent->GetWorldPosition4x4();
	}

	// Get our global transform using the proper hierarchy method
	glm::mat4 worldTransform = GetGlobalTransform(parentWorld);

	return glm::vec3(worldTransform[3]);
}

//Add method to get parent's world transform as matrix
glm::mat4 SceneNode::GetWorldPosition4x4() const
{
	// Get parent's world transform if we have a parent  
	glm::mat4 parentWorld(1.0f);
	if (auto parent = parentNode.lock()) {
		parentWorld = parent->GetWorldPosition4x4();
	}

	// Return our global transform using the proper hierarchy method
	return GetGlobalTransform(parentWorld);
}

std::pair<glm::vec3, glm::vec3> SceneNode::GetBoundingBox() {
	if (!m_model) {
		return { glm::vec3(0), glm::vec3(0) };
	}

	auto [minB, maxB] = m_model->GetBoundingBox();

	// Apply the complete local transform (including animation)
	glm::mat4 localTransform;
	if (m_isAnimationPlaying && !m_isAnimationPaused) {
		localTransform = transform * animatedTransform;
	}
	else {
		if (animatedTransform != glm::mat4(1.0f)) {
			localTransform = transform * animatedTransform;
		}
		else {
			localTransform = transform;
		}
	}

	// Transform all 8 corners of the bounding box to get accurate AABB
	glm::vec3 corners[8] = {
		glm::vec3(minB.x, minB.y, minB.z),
		glm::vec3(maxB.x, minB.y, minB.z),
		glm::vec3(minB.x, maxB.y, minB.z),
		glm::vec3(maxB.x, maxB.y, minB.z),
		glm::vec3(minB.x, minB.y, maxB.z),
		glm::vec3(maxB.x, minB.y, maxB.z),
		glm::vec3(minB.x, maxB.y, maxB.z),
		glm::vec3(maxB.x, maxB.y, maxB.z)
	};

	glm::vec3 transformedMin = glm::vec3(std::numeric_limits<float>::max());
	glm::vec3 transformedMax = glm::vec3(std::numeric_limits<float>::lowest());

	for (int i = 0; i < 8; ++i) {
		glm::vec3 transformedCorner = glm::vec3(localTransform * glm::vec4(corners[i], 1.0f));
		transformedMin = glm::min(transformedMin, transformedCorner);
		transformedMax = glm::max(transformedMax, transformedCorner);
	}

	return { transformedMin, transformedMax };
}

void SceneNode::Draw(
	const glm::mat4& parentTransform,
	const glm::mat4& view,
	const glm::mat4& projection,
	unsigned int defaultShaderProgram)
{
	//Calculate the global transform properly using the helper method
	glm::mat4 global = GetGlobalTransform(parentTransform);

	auto bindTextureWithFallback = [](GLint uniformLoc, GLuint unit, const std::shared_ptr<Texture>& tex, GLuint fallback) {
		glActiveTexture(GL_TEXTURE0 + unit);
		if (tex && tex->IsValid()) {
			tex->Bind(GL_TEXTURE0 + unit);
		}
		else {
			glBindTexture(GL_TEXTURE_2D, fallback);
		}
		if (uniformLoc >= 0) {
			glUniform1i(uniformLoc, unit);
		}
		//Check for OpenGL errors after texture binding
		GLenum error = glGetError();
		if (error != GL_NO_ERROR) {
			std::cerr << "[SceneNode]OpenGL error " << error << " binding texture to unit " << unit << std::endl;
		}
		};

	unsigned int useShader = (m_shader != 0 ? m_shader : defaultShaderProgram);
	glUseProgram(useShader);

	// Upload transform matrices
	if (GLint locModel = glGetUniformLocation(useShader, "model"); locModel != -1) {
		glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(global));
	}
	if (GLint locView = glGetUniformLocation(useShader, "view"); locView != -1) {
		glUniformMatrix4fv(locView, 1, GL_FALSE, glm::value_ptr(view));
	}
	if (GLint locProj = glGetUniformLocation(useShader, "projection"); locProj != -1) {
		glUniformMatrix4fv(locProj, 1, GL_FALSE, glm::value_ptr(projection));
	}

	// Handle skinning with corrected bone transforms
	if (isSkinned) {
		std::vector<glm::mat4> boneMats = GetBoneTransforms();

		// Try enhanced shader uniform names first, then fall back to legacy names
		if (!boneMats.empty()) {
			GLint locBones = glGetUniformLocation(useShader, "u_boneMatrices");
			if (locBones == -1) {
				locBones = glGetUniformLocation(useShader, "bones"); // Legacy fallback
			}

			if (locBones != -1) {
				size_t numBones = std::min(boneMats.size(), (size_t)128);
				glUniformMatrix4fv(locBones, (GLsizei)numBones, GL_FALSE, glm::value_ptr(boneMats[0]));
			}
		}

		GLint locUseSkin = glGetUniformLocation(useShader, "u_enableSkinning");
		if (locUseSkin == -1) {
			locUseSkin = glGetUniformLocation(useShader, "useSkinning"); // Legacy fallback
		}

		if (locUseSkin != -1) {
			glUniform1i(locUseSkin, 1);
		}
	}
	else {
		GLint locUseSkin = glGetUniformLocation(useShader, "u_enableSkinning");
		if (locUseSkin == -1) {
			locUseSkin = glGetUniformLocation(useShader, "useSkinning"); // Legacy fallback
		}

		if (locUseSkin != -1) {
			glUniform1i(locUseSkin, 0);
		}
	}

	// Draw the model's meshes
	if (m_model) {
		for (auto& mesh : m_model->meshes) {
			// Apply per-node culling override with mesh culling
			ApplyCullingState(mesh, m_cullingOverride);

			// Handle alpha modes according to glTF 2.0 specification
			if (mesh.RequiresAlphaBlending()) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glDepthMask(GL_FALSE); // Disable depth writes for transparent objects
			}
			else {
				glDisable(GL_BLEND);
				glDepthMask(GL_TRUE); // Enable depth writes for opaque objects
			}

			// Configure alpha testing
			if (mesh.RequiresAlphaTesting()) {
				if (GLint locAlphaCutoff = glGetUniformLocation(useShader, "alphaCutoff"); locAlphaCutoff >= 0) {
					glUniform1f(locAlphaCutoff, mesh.alphaCutoff);
				}
				if (GLint locUseAlphaTest = glGetUniformLocation(useShader, "useAlphaTest"); locUseAlphaTest >= 0) {
					glUniform1i(locUseAlphaTest, 1);
				}
			}
			else {
				if (GLint locUseAlphaTest = glGetUniformLocation(useShader, "useAlphaTest"); locUseAlphaTest >= 0) {
					glUniform1i(locUseAlphaTest, 0);
				}
			}

			// Bind textures following glTF 2.0 standard naming and units
			GLint uBase = glGetUniformLocation(useShader, "texture_diffuse");
			GLint uNorm = glGetUniformLocation(useShader, "texture_normal");
			GLint uMR = glGetUniformLocation(useShader, "texture_metallic_roughness");
			GLint uEmis = glGetUniformLocation(useShader, "texture_emissive");
			GLint uAO = glGetUniformLocation(useShader, "texture_occlusion");
			GLint uSpec = glGetUniformLocation(useShader, "texture_specular");

			bindTextureWithFallback(uBase, TextureUnits::MATERIAL_BASE_COLOR, mesh.diffuseTexture, DefaultTextures::White());
			bindTextureWithFallback(uNorm, TextureUnits::MATERIAL_NORMAL, mesh.normalTexture, DefaultTextures::Normal());
			bindTextureWithFallback(uMR, TextureUnits::MATERIAL_METALLIC_ROUGHNESS, mesh.roughnessTexture, DefaultTextures::MetallicRoughnessDefault());
			bindTextureWithFallback(uEmis, TextureUnits::MATERIAL_EMISSIVE, mesh.emissiveTexture, DefaultTextures::Black());
			bindTextureWithFallback(uAO, TextureUnits::MATERIAL_OCCLUSION, mesh.occlusionTexture, DefaultTextures::AOWhite());
			bindTextureWithFallback(uSpec, TextureUnits::MATERIAL_SPECULAR, mesh.specularTexture, DefaultTextures::White());

			// Upload texture presence flags for proper material handling
			if (GLint locHasBaseColor = glGetUniformLocation(useShader, "hasBaseColorTexture"); locHasBaseColor >= 0) {
				glUniform1i(locHasBaseColor, (mesh.diffuseTexture && mesh.diffuseTexture->IsValid()) ? 1 : 0);
			}
			if (GLint locHasNormal = glGetUniformLocation(useShader, "hasNormalTexture"); locHasNormal >= 0) {
				glUniform1i(locHasNormal, (mesh.normalTexture && mesh.normalTexture->IsValid()) ? 1 : 0);
			}
			if (GLint locHasMR = glGetUniformLocation(useShader, "hasMetallicRoughnessTexture"); locHasMR >= 0) {
				glUniform1i(locHasMR, (mesh.roughnessTexture && mesh.roughnessTexture->IsValid()) ? 1 : 0);
			}
			if (GLint locHasEmissive = glGetUniformLocation(useShader, "hasEmissiveTexture"); locHasEmissive >= 0) {
				glUniform1i(locHasEmissive, (mesh.emissiveTexture && mesh.emissiveTexture->IsValid()) ? 1 : 0);
			}
			if (GLint locHasOcclusion = glGetUniformLocation(useShader, "hasOcclusionTexture"); locHasOcclusion >= 0) {
				glUniform1i(locHasOcclusion, (mesh.occlusionTexture && mesh.occlusionTexture->IsValid()) ? 1 : 0);
			}
			if (GLint locHasSpecular = glGetUniformLocation(useShader, "hasSpecularTexture"); locHasSpecular >= 0) {
				glUniform1i(locHasSpecular, (mesh.specularTexture && mesh.specularTexture->IsValid()) ? 1 : 0);
			}

			// Upload glTF 2.0 standard material factors
			if (GLint locBaseColor = glGetUniformLocation(useShader, "baseColorFactor"); locBaseColor >= 0) {
				glUniform4fv(locBaseColor, 1, glm::value_ptr(mesh.baseColorFactor));
			}
			if (GLint locMetallic = glGetUniformLocation(useShader, "metallicFactor"); locMetallic >= 0) {
				glUniform1f(locMetallic, mesh.metallicFactor);
			}
			if (GLint locRoughness = glGetUniformLocation(useShader, "roughnessFactor"); locRoughness >= 0) {
				glUniform1f(locRoughness, mesh.roughnessFactor);
			}
			if (GLint locEmissive = glGetUniformLocation(useShader, "emissiveFactor"); locEmissive >= 0) {
				glUniform3fv(locEmissive, 1, glm::value_ptr(mesh.emissiveFactor));
			}

			// Upload additional material properties
			if (GLint locNormalScale = glGetUniformLocation(useShader, "normalScale"); locNormalScale >= 0) {
				glUniform1f(locNormalScale, mesh.normalScale);
			}
			if (GLint locOcclusionStrength = glGetUniformLocation(useShader, "occlusionStrength"); locOcclusionStrength >= 0) {
				glUniform1f(locOcclusionStrength, mesh.occlusionStrength);
			}

			// Upload KHR_materials_specular extension factors
			if (GLint locSpecularFactor = glGetUniformLocation(useShader, "specularFactor"); locSpecularFactor >= 0) {
				glUniform1f(locSpecularFactor, mesh.specularFactor.x); // Use x component as uniform scalar
			}
			if (GLint locSpecularColor = glGetUniformLocation(useShader, "specularColorFactor"); locSpecularColor >= 0) {
				glUniform3fv(locSpecularColor, 1, glm::value_ptr(mesh.specularColorFactor));
			}

			// Draw
			glBindVertexArray(mesh.VAO);
			glDrawElements(GL_TRIANGLES, (GLsizei)mesh.indexCount, GL_UNSIGNED_INT, 0);
			glBindVertexArray(0);

			// Reset states
			if (mesh.RequiresAlphaBlending()) {
				glDisable(GL_BLEND);
				glDepthMask(GL_TRUE); // Re-enable depth writes
			}
		}
	}

	// Recursively draw children with the correct global transform
	for (auto& child : children) {
		child->Draw(global, view, projection, defaultShaderProgram);
	}
}

void SceneNode::DrawCascade(
	const glm::mat4& parentTransform,
	const glm::mat4& lightSpace,
	unsigned int shadowShader)
{
	// Use proper transform hierarchy calculation
	glm::mat4 global = GetGlobalTransform(parentTransform);
	glUseProgram(shadowShader);

	static GLint locModel = glGetUniformLocation(shadowShader, "model");
	if (locModel != -1) {
		glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(global));
	}
	static GLint locLS = glGetUniformLocation(shadowShader, "lightSpaceMatrix");
	if (locLS != -1) {
		glUniformMatrix4fv(locLS, 1, GL_FALSE, glm::value_ptr(lightSpace));
	}

	if (m_model) {
		// Usually in shadow pass we only need positions
		m_model->Draw();
	}

	// Recursively draw children with the correct global transform
	for (auto& c : children) {
		c->DrawCascade(global, lightSpace, shadowShader);
	}
}

void SceneNode::DrawGeometry(const glm::mat4& parentTransform, unsigned int geometryShader) {
	glm::mat4 global = GetGlobalTransform(parentTransform);
	glUseProgram(geometryShader);

	auto bindTextureIfValid = [](GLint uniformLoc, GLuint unit, const std::shared_ptr<Texture>& tex) -> bool {
		if (tex && tex->IsValid()) {
			glActiveTexture(GL_TEXTURE0 + unit);
			tex->Bind(GL_TEXTURE0 + unit);
			if (uniformLoc >= 0) {
				glUniform1i(uniformLoc, unit);
			}
			return true;
		}
		return false;
		};

	if (GLint locModel = glGetUniformLocation(geometryShader, "model"); locModel >= 0) glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(global));

	// Enhanced skeletal animation support with correct uniform names
	if (isSkinned) {
		std::vector<glm::mat4> boneMats = GetBoneTransforms();

		// Upload bone matrices (use correct uniform name for enhanced G-buffer shader)
		if (!boneMats.empty()) {
			if (GLint locBones = glGetUniformLocation(geometryShader, "u_boneMatrices"); locBones >= 0) {
				// Ensure we don't exceed the shader's bone limit (128)
				size_t numBones = std::min(boneMats.size(), (size_t)128);
				glUniformMatrix4fv(locBones, (GLsizei)numBones, GL_FALSE, glm::value_ptr(boneMats[0]));
				std::cout << "[SceneNode] Uploaded " << numBones << " bone matrices for skinned mesh" << std::endl;
			}
		}

		// Enable skinning (use correct uniform name)
		if (GLint locUseSkin = glGetUniformLocation(geometryShader, "u_enableSkinning"); locUseSkin >= 0) {
			glUniform1i(locUseSkin, 1);
		}
	}
	else {
		// Disable skinning for non-skinned meshes
		if (GLint locUseSkin = glGetUniformLocation(geometryShader, "u_enableSkinning"); locUseSkin >= 0) {
			glUniform1i(locUseSkin, 0);
		}
	}

	if (m_model) {
		for (auto& mesh : m_model->meshes) {
			// NEW: Skip transparent meshes (alpha blending) in deferred G-buffer pass
			if (mesh.RequiresAlphaBlending()) {
				continue; // they will be rendered later in forward transparent pass
			}
			ApplyCullingState(mesh, m_cullingOverride);
			if (mesh.RequiresAlphaBlending()) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); }
			else { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
			if (mesh.RequiresAlphaTesting()) { if (GLint locAlphaCutoff = glGetUniformLocation(geometryShader, "alphaCutoff"); locAlphaCutoff >= 0) glUniform1f(locAlphaCutoff, mesh.alphaCutoff); if (GLint locUseAlphaTest = glGetUniformLocation(geometryShader, "useAlphaTest"); locUseAlphaTest >= 0) glUniform1i(locUseAlphaTest, 1); }
			else { if (GLint locUseAlphaTest = glGetUniformLocation(geometryShader, "useAlphaTest"); locUseAlphaTest >= 0) glUniform1i(locUseAlphaTest, 0); }

			if (GLint locHasBaseColorTex = glGetUniformLocation(geometryShader, "hasBaseColorTexture"); locHasBaseColorTex >= 0) glUniform1i(locHasBaseColorTex, (mesh.diffuseTexture && mesh.diffuseTexture->IsValid()) ? 1 : 0);
			if (GLint locHasNormalTex = glGetUniformLocation(geometryShader, "hasNormalTexture"); locHasNormalTex >= 0) glUniform1i(locHasNormalTex, (mesh.normalTexture && mesh.normalTexture->IsValid()) ? 1 : 0);
			if (GLint locHasMR = glGetUniformLocation(geometryShader, "hasMetallicRoughnessTexture"); locHasMR >= 0) glUniform1i(locHasMR, (mesh.roughnessTexture && mesh.roughnessTexture->IsValid()) ? 1 : 0);
			if (GLint locHasEmis = glGetUniformLocation(geometryShader, "hasEmissiveTexture"); locHasEmis >= 0) glUniform1i(locHasEmis, (mesh.emissiveTexture && mesh.emissiveTexture->IsValid()) ? 1 : 0);
			if (GLint locHasOcc = glGetUniformLocation(geometryShader, "hasOcclusionTexture"); locHasOcc >= 0) glUniform1i(locHasOcc, (mesh.occlusionTexture && mesh.occlusionTexture->IsValid()) ? 1 : 0);
			if (GLint locHasSpec = glGetUniformLocation(geometryShader, "hasSpecularTexture"); locHasSpec >= 0) glUniform1i(locHasSpec, (mesh.specularTexture && mesh.specularTexture->IsValid()) ? 1 : 0);

			GLint uBase = glGetUniformLocation(geometryShader, "texture_diffuse");
			GLint uNorm = glGetUniformLocation(geometryShader, "texture_normal");
			GLint uMR = glGetUniformLocation(geometryShader, "texture_metallic_roughness");
			GLint uEmis = glGetUniformLocation(geometryShader, "texture_emissive");
			GLint uAO = glGetUniformLocation(geometryShader, "texture_occlusion");
			GLint uSpec = glGetUniformLocation(geometryShader, "texture_specular");
			bindTextureIfValid(uBase, TextureUnits::MATERIAL_BASE_COLOR, mesh.diffuseTexture);
			bindTextureIfValid(uNorm, TextureUnits::MATERIAL_NORMAL, mesh.normalTexture);
			bindTextureIfValid(uMR, TextureUnits::MATERIAL_METALLIC_ROUGHNESS, mesh.roughnessTexture);
			bindTextureIfValid(uEmis, TextureUnits::MATERIAL_EMISSIVE, mesh.emissiveTexture);
			bindTextureIfValid(uAO, TextureUnits::MATERIAL_OCCLUSION, mesh.occlusionTexture);
			bindTextureIfValid(uSpec, TextureUnits::MATERIAL_SPECULAR, mesh.specularTexture);

			if (GLint locBaseColor = glGetUniformLocation(geometryShader, "baseColorFactor"); locBaseColor >= 0) glUniform4fv(locBaseColor, 1, glm::value_ptr(mesh.baseColorFactor));
			if (GLint locMetallic = glGetUniformLocation(geometryShader, "metallicFactor"); locMetallic >= 0) glUniform1f(locMetallic, mesh.metallicFactor);
			if (GLint locRoughness = glGetUniformLocation(geometryShader, "roughnessFactor"); locRoughness >= 0) glUniform1f(locRoughness, mesh.roughnessFactor);
			if (GLint locEmissive = glGetUniformLocation(geometryShader, "emissiveFactor"); locEmissive >= 0) glUniform3fv(locEmissive, 1, glm::value_ptr(mesh.emissiveFactor));
			if (GLint locOccStr = glGetUniformLocation(geometryShader, "occlusionStrength"); locOccStr >= 0) glUniform1f(locOccStr, mesh.occlusionStrength);
			if (GLint locSpecFactor = glGetUniformLocation(geometryShader, "specularFactor"); locSpecFactor >= 0) glUniform1f(locSpecFactor, mesh.specularFactor.x); // assume uniform scalar in x
			if (GLint locSpecColor = glGetUniformLocation(geometryShader, "specularColorFactor"); locSpecColor >= 0) glUniform3fv(locSpecColor, 1, glm::value_ptr(mesh.specularColorFactor));

			glBindVertexArray(mesh.VAO);
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
			glBindVertexArray(0);
			if (mesh.RequiresAlphaBlending()) { glDisable(GL_BLEND); glDepthMask(GL_TRUE); }
		}
	}
	for (auto& child : children) child->DrawGeometry(global, geometryShader);
}

// Motion vector pass for TAA
void SceneNode::DrawVelocity(const glm::mat4& parentTransform,
	const glm::mat4& prevParentTransform,
	unsigned int velocityShader) {
	// Use proper transform hierarchy calculation
	glm::mat4 global = GetGlobalTransform(parentTransform);
	glm::mat4 prevGlobal = GetGlobalTransform(prevParentTransform);

	glUseProgram(velocityShader);

	// Upload current and previous model matrices
	if (GLint locModel = glGetUniformLocation(velocityShader, "model"); locModel >= 0)
		glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(global));

	if (GLint locPrevModel = glGetUniformLocation(velocityShader, "prevModel"); locPrevModel >= 0)
		glUniformMatrix4fv(locPrevModel, 1, GL_FALSE, glm::value_ptr(prevGlobal));

	// Handle skinning if required (for animated objects)
	if (isSkinned) {
		std::vector<glm::mat4> boneMats = GetBoneTransforms();
		if (!boneMats.empty()) {
			if (GLint locBones = glGetUniformLocation(velocityShader, "boneMatrices"); locBones >= 0)
				glUniformMatrix4fv(locBones, (GLsizei)boneMats.size(), GL_FALSE, glm::value_ptr(boneMats[0]));

			// For previous frame bone matrices, we'll use the same for now
			// In a full implementation, you'd store previous frame bone transforms
			if (GLint locPrevBones = glGetUniformLocation(velocityShader, "prevBoneMatrices"); locPrevBones >= 0)
				glUniformMatrix4fv(locPrevBones, (GLsizei)boneMats.size(), GL_FALSE, glm::value_ptr(boneMats[0]));
		}
		if (GLint locHasBones = glGetUniformLocation(velocityShader, "hasBones"); locHasBones >= 0)
			glUniform1i(locHasBones, 1);
	}
	else {
		if (GLint locHasBones = glGetUniformLocation(velocityShader, "hasBones"); locHasBones >= 0)
			glUniform1i(locHasBones, 0);
	}

	// Draw all meshes for this node (velocity pass only needs positions)
	if (m_model) {
		for (auto& mesh : m_model->meshes) {
			// Apply culling state for velocity pass
			ApplyCullingState(mesh, m_cullingOverride);

			// Render the mesh (velocity pass only needs vertex positions)
			glBindVertexArray(mesh.VAO);
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
			glBindVertexArray(0);
		}
	}

	// Recursively draw children with the correct global transforms
	for (auto& child : children) {
		child->DrawVelocity(global, prevGlobal, velocityShader);
	}
}

void SceneNode::CollectRenderableObjects(
	const glm::mat4& parentTransform,
	MDIBatch& batch)
{
	//Use cached global transform if not dirty
	glm::mat4 global = GetGlobalTransform(parentTransform);

	if (m_model) {
		for (const auto& mesh : m_model->meshes) {
			//Use precomputed bounding volume instead of scanning rawVertices
			glm::vec3 center = mesh.boundingCenter;
			float radius = mesh.boundingRadius;

			// If bounding volume hasn't been computed, use default or skip
			if (!mesh.boundingVolumeValid) {
				// Fallback to boundingRadius field if available
				center = glm::vec3(0.0f);
				radius = boundingRadius; // Use node's bounding radius
			}

			MDI_RenderableObject obj{};
			obj.count = static_cast<GLuint>(mesh.indexCount);
			// Without a combined index buffer, each mesh uses its own EBO
			obj.firstIndex = 0;
			obj.baseVertex = 0;
			obj.modelMatrix = global;
			obj.vao = mesh.VAO;

			// Use cached bounding sphere for fast culling
			obj.boundingSphere = glm::vec4(center, radius);

			batch.AddObject(obj);
		}
	}

	for (auto& child : children) {
		child->CollectRenderableObjects(global, batch);
	}
}

void SceneNode::ApplyCullingState(const MeshComponent& mesh, SceneNode::CullingOverride nodeOverride) {
	// Determine final culling mode
	bool enableCulling = true;
	GLenum cullFace = GL_BACK;

	switch (nodeOverride) {
	case SceneNode::CULLING_FORCE_ENABLE:
		enableCulling = true;
		cullFace = GL_BACK;
		break;
	case SceneNode::CULLING_FORCE_DISABLE:
		enableCulling = false;
		break;
	case SceneNode::CULLING_FORCE_FRONT:
		enableCulling = true;
		cullFace = GL_FRONT;
		break;
	case SceneNode::CULLING_INHERIT:
	default:
		// Use mesh's effective culling mode
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
			enableCulling = true;
			cullFace = GL_BACK;
			break;
		}
		break;
	}

	// Apply the culling state
	if (enableCulling) {
		glEnable(GL_CULL_FACE);
		glCullFace(cullFace);
	}
	else {
		glDisable(GL_CULL_FACE);
	}
}

void SceneNode::SetPosition(glm::vec3 pos) {
	transform[3] = glm::vec4(pos, 1.0f);

	// Mark scene BVH as dirty when geometry moves
	if (s_globalTransformSystem) {
		// Signal that geometry has moved - BVH needs rebuild
		InvalidateTransformCache();
	}
}

void SceneNode::SetRotation(glm::vec3 axis, float angle) {
	glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), angle, axis);
	transform = rotation * transform;

	// Mark scene BVH as dirty when geometry rotates
	if (s_globalTransformSystem) {
		InvalidateTransformCache();
	}
}

void SceneNode::SetScale(glm::vec3 scale) {
	transform = glm::scale(glm::mat4(1.0f), scale) * transform;

	// Mark scene BVH as dirty when geometry scales
	if (s_globalTransformSystem) {
		InvalidateTransformCache();
	}
}

void SceneNode::SetTransform(const glm::mat4& transform)
{
	this->transform = transform;

	// If this node has a physics body, sync it immediately
	// But only if we're not already updating from physics to prevent infinite recursion
	if (m_rigidbody && !m_updatingFromPhysics) {
		SyncPhysicsFromTransform();
	}

	// Mark that we need to update child transforms and BVH
	InvalidateTransformCache();
}

void SceneNode::SetLocalTRS(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale) {
	// Validate input parameters to prevent NaN/Infinity corruption
	auto isValidVec3 = [](const glm::vec3& v) {
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		};

	auto isValidQuat = [](const glm::quat& q) {
		return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
		};

	// Sanitize inputs
	glm::vec3 safeTranslation = translation;
	glm::quat safeRotation = rotation;
	glm::vec3 safeScale = scale;

	if (!isValidVec3(translation)) {
		std::cerr << "[SceneNode] Warning: Invalid translation detected, using current position" << std::endl;
		safeTranslation = GetPosition(); // Use current position as fallback
		if (!isValidVec3(safeTranslation)) {
			safeTranslation = glm::vec3(0.0f); // Ultimate fallback
		}
	}

	if (!isValidQuat(rotation) || glm::length(rotation) < 1e-6f) {
		std::cerr << "[SceneNode] Warning: Invalid rotation detected, using identity" << std::endl;
		safeRotation = glm::quat(1, 0, 0, 0); // Identity quaternion
	}
	else {
		safeRotation = glm::normalize(rotation); // Ensure normalized
	}

	if (!isValidVec3(scale)) {
		std::cerr << "[SceneNode] Warning: Invalid scale detected, using current scale" << std::endl;
		safeScale = GetScale(); // Use current scale as fallback
		if (!isValidVec3(safeScale)) {
			safeScale = glm::vec3(1.0f); // Ultimate fallback
		}
	}
	else {
		// Prevent zero or negative scale components
		const float MIN_SCALE = 1e-6f;
		safeScale = glm::max(safeScale, glm::vec3(MIN_SCALE));
	}

	// Clamp translation to reasonable bounds
	const float MAX_COORD = 100000.0f;
	safeTranslation = glm::clamp(safeTranslation, glm::vec3(-MAX_COORD), glm::vec3(MAX_COORD));

	// Clamp scale to reasonable bounds
	const float MAX_SCALE = 1000.0f;
	safeScale = glm::min(safeScale, glm::vec3(MAX_SCALE));

	glm::mat4 T = glm::translate(glm::mat4(1.0f), safeTranslation);
	glm::mat4 R = glm::mat4_cast(safeRotation);
	glm::mat4 S = glm::scale(glm::mat4(1.0f), safeScale);
	glm::mat4 newTransform = T * R * S;

	// Final validation of the resulting transform matrix
	bool validTransform = true;
	for (int i = 0; i < 4 && validTransform; ++i) {
		for (int j = 0; j < 4 && validTransform; ++j) {
			if (!std::isfinite(newTransform[i][j])) {
				validTransform = false;
			}
		}
	}

	if (validTransform) {
		transform = newTransform;
		InvalidateTransformCache();
		if (m_rigidbody && !m_updatingFromPhysics) {
			SyncPhysicsFromTransform();
		}
	}
	else {
		std::cerr << "[SceneNode] Error: Computed transform matrix is invalid, keeping previous transform" << std::endl;
		// Don't update the transform if it's invalid
	}
}

void SceneNode::SyncPhysicsFromTransform() {
	if (!m_rigidbody || m_updatingFromPhysics) return;

	// Set flag to prevent infinite recursion
	m_updatingFromPhysics = true;

	// Decompose the transform to get position and rotation
	glm::vec3 scale, translation, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(transform, scale, rotation, translation, skew, perspective);

	// Update physics body
	m_rigidbody->setPosition(translation);
	m_rigidbody->setOrientation(rotation);

	// Reset physics velocities when manually manipulated
	// This prevents the object from continuing previous motion
	m_rigidbody->setVelocity(glm::vec3(0.0f));
	m_rigidbody->setAngularVelocity(glm::vec3(0.0f));

	// Store previous state for interpolation
	m_rigidbody->storePreviousState();

	// Clear flag
	m_updatingFromPhysics = false;
}

void SceneNode::InvalidateTransformCache() {
	// Mark that cached transforms need to be recalculated
	// This will be used in GetGlobalTransform to recalculate when needed
	m_transformCacheDirty = true;

	// Mark BVH as dirty when any transform changes (for ray tracing)
	if (s_globalSceneGraph) {
		s_globalSceneGraph->MarkBVHDirty();
	}

	// Recursively invalidate children
	for (auto& child : children) {
		child->InvalidateTransformCache();
	}
}

glm::vec3 SceneNode::GetPosition() const
{
	return glm::vec3(transform[3]);
}

glm::vec3 SceneNode::GetRotation() const
{
	glm::vec3 eulerAngles = glm::eulerAngles(glm::quat_cast(transform));
	return eulerAngles;
}

glm::vec3 SceneNode::GetScale() const
{
	glm::vec3 translation, scale, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(transform, scale, rotation, translation, skew, perspective);
	return scale;
}

glm::mat4 SceneNode::GetTransform() const
{
	return transform;
}

void SceneNode::PlayAnimation(int animationIndex) {
	if (!m_model) return;
	if (animationIndex < 0 || animationIndex >= (int)m_model->animations.size()) {
		return;
	}
	m_currentAnimationIndex = animationIndex;
	m_isAnimationPlaying = true;
	m_isAnimationPaused = false;
}

void SceneNode::PauseAnimation() {
	if (m_isAnimationPlaying) {
		m_isAnimationPaused = true;
	}
}

void SceneNode::StopAnimation() {
	m_isAnimationPlaying = false;
	m_isAnimationPaused = false;
	m_animationTime = 0.0f;
	m_currentAnimationIndex = -1;
	animatedTransform = glm::mat4(1.0f);
}

void SceneNode::UpdateAnimation(float deltaTime) {
	// Update animation controller if we have one
	if (m_animationController) {
		m_animationController->Update(deltaTime);

		// Apply animations to this node
		if (m_model) {
			m_animationController->ApplyAnimationsToNode(shared_from_this(), m_model);

			// Update bone matrices for skinning if this is a skinned mesh
			if (isSkinned) {
				m_animationController->UpdateBoneMatrices(shared_from_this(), m_model);
			}
		}
	}

	// Legacy animation system for backward compatibility
	if (m_isAnimationPlaying && !m_isAnimationPaused && m_model && !m_model->animations.empty()) {
		if (m_currentAnimationIndex >= 0 && m_currentAnimationIndex < (int)m_model->animations.size()) {
			m_animationTime += deltaTime;

			const auto& animation = m_model->animations[m_currentAnimationIndex];

			// Handle looping
			if (animation.isLooping) {
				if (m_animationTime > animation.endTime) {
					m_animationTime = animation.startTime +
						std::fmod(m_animationTime - animation.startTime, animation.duration);
				}
			}
			else {
				// Clamp to animation bounds for non-looping animations
				if (m_animationTime > animation.endTime) {
					m_animationTime = animation.endTime;
					m_isAnimationPlaying = false; // Animation finished
					std::cout << "[SceneNode] Legacy animation finished: " << animation.name << std::endl;
				}
			}

			// Apply the animation transform
			if (m_animationTime >= animation.startTime && m_animationTime <= animation.endTime) {
				if (isSkinned) {
					// For skinned meshes, get bone transform
					animatedTransform = animation.GetBoneTransformForNode(nodeIndex, m_animationTime, *m_model);
				}
				else {
					// For regular nodes, get node transform
					animatedTransform = animation.GetNodeTransform(m_animationTime);
				}
			}
		}
	}

	// Recursively update children
	for (auto& child : children) {
		if (child) {
			child->UpdateAnimation(deltaTime);
		}
	}
}

void SceneNode::SetAnimationController(std::shared_ptr<AnimationController> controller) {
	m_animationController = controller;
}

std::shared_ptr<AnimationController> SceneNode::GetAnimationController() const {
	return m_animationController;
}

void SceneNode::PlayAnimationByName(const std::string& animationName, bool loop) {
	if (!m_animationController || !m_model) return;

	// Find the animation by name
	for (const auto& anim : m_model->animations) {
		if (anim.name == animationName) {
			auto animPtr = std::make_shared<Animation>(anim);
			m_animationController->PlayAnimation(animPtr, 0.0f, loop);
			std::cout << "[SceneNode] Playing animation by name: " << animationName << std::endl;
			return;
		}
	}

	std::cerr << "[SceneNode] Animation not found: " << animationName << std::endl;
}

void SceneNode::BlendToAnimation(const std::string& animationName, float blendTime, bool loop) {
	if (!m_animationController || !m_model) return;

	// Find the animation by name
	for (const auto& anim : m_model->animations) {
		if (anim.name == animationName) {
			auto animPtr = std::make_shared<Animation>(anim);
			m_animationController->BlendToAnimation(animPtr, blendTime, loop);
			std::cout << "[SceneNode] Blending to animation: " << animationName << std::endl;
			return;
		}
	}

	std::cerr << "[SceneNode] Animation not found for blending: " << animationName << std::endl;
}

void SceneNode::SetMorphWeights(const std::vector<float>& weights) {
	m_morphWeights = weights;

	// Apply morph weights to animation controller if available
	if (m_animationController) {
		m_animationController->ApplyMorphTargets(shared_from_this(), weights);
	}
}

const std::vector<float>& SceneNode::GetMorphWeights() const {
	return m_morphWeights;
}

void SceneNode::Shutdown() {
	// If this is a model node, detach the rigid body
	if (m_nodeType == MODEL) {
		if (m_rigidbody) {
			m_rigidbody->DetachNode();
			m_rigidbody.reset();
		}
	}

	// If this is an audio node, stop it
	if (m_nodeType == AUDIO) {
		AudioNode* audioNode = dynamic_cast<AudioNode*>(this);
		if (audioNode) {
			audioNode->stop();
		}
	}
	// Recursively shutdown children
	for (auto& child : children) {
		child->Shutdown();
	}
}

glm::mat4 SceneNode::GetGlobalTransform(const glm::mat4& parentTransform) const {
	// Combine base transform with animation transform
	glm::mat4 localTransform = transform;

	// Apply animation transform if available
	// Check if we have any animation applied (either from new controller or legacy system)
	if (glm::any(glm::notEqual(animatedTransform[0], glm::vec4(1, 0, 0, 0))) ||
		glm::any(glm::notEqual(animatedTransform[1], glm::vec4(0, 1, 0, 0))) ||
		glm::any(glm::notEqual(animatedTransform[2], glm::vec4(0, 0, 1, 0))) ||
		glm::any(glm::notEqual(animatedTransform[3], glm::vec4(0, 0, 0, 1)))) {
		//Apply animation transform first, then base transform
		// This ensures proper hierarchical transformation order
		localTransform = transform * animatedTransform;
	}

	return parentTransform * localTransform;
}

void SceneNode::UpdateAudioNodes(const glm::vec3& listenerPos, float listenerAngle) {
	// Default implementation - does nothing for non-audio nodes
	// This will be overridden in AudioNode subclass if it exists

	// Recursively update children
	for (auto& child : children) {
		child->UpdateAudioNodes(listenerPos, listenerAngle);
	}
}

void SceneNode::BuildSkeleton(const Scene& model) {
	// Build skeleton for skinned meshes
	if (!isSkinned || model.skin.joints.empty()) {
		return;
	}

	// Clear existing bone nodes
	boneNodes.clear();
	boneInverseBindMatrices.clear();

	// Create bone nodes for each joint
	for (size_t i = 0; i < model.skin.joints.size(); ++i) {
		int jointIndex = model.skin.joints[i];

		if (jointIndex >= 0 && jointIndex < (int)model.nodes.size()) {
			// Find or create a node for this joint
			auto boneNode = FindOrCreateNode(jointIndex, model);
			if (boneNode) {
				boneNodes.push_back(boneNode);

				// Add inverse bind matrix if available
				if (i < model.skin.inverseBindMatrices.size()) {
					boneInverseBindMatrices.push_back(model.skin.inverseBindMatrices[i]);
				}
				else {
					boneInverseBindMatrices.push_back(glm::mat4(1.0f));
				}
			}
		}
	}

	std::cout << "[SceneNode] Built skeleton with " << boneNodes.size() << " bones" << std::endl;
}

glm::quat SceneNode::GetOrientation() const {
	// Extract rotation from transform matrix
	glm::vec3 scale, translation, skew;
	glm::vec4 perspective;
	glm::quat rotation;
	glm::decompose(transform, scale, rotation, translation, skew, perspective);
	return rotation;
}

std::shared_ptr<SceneNode> SceneNode::FindNodeByIndex(int nodeIdx) {
	// Check if this node matches
	if (nodeIndex == nodeIdx) {
		return shared_from_this();
	}

	// Recursively search children
	for (auto& child : children) {
		auto found = child->FindNodeByIndex(nodeIdx);
		if (found) {
			return found;
		}
	}

	return nullptr;
}

std::vector<glm::mat4> SceneNode::GetBoneTransforms() const {
	std::vector<glm::mat4> boneMatrices;

	if (!isSkinned || !m_model) {
		return boneMatrices;
	}

	// For now, use the bone nodes if available
	size_t numBones = std::max(boneInverseBindMatrices.size(), boneNodes.size());

	if (numBones == 0) {
		// Default to identity matrices if no bone information is available
		return boneMatrices;
	}

	boneMatrices.resize(numBones, glm::mat4(1.0f));

	// Calculate bone matrices: globalTransform * inverseBindMatrix
	for (size_t i = 0; i < numBones; ++i) {
		glm::mat4 boneTransform = glm::mat4(1.0f);

		// Get the bone node's global transform if available
		if (i < boneNodes.size() && boneNodes[i]) {
			//Use the unified hierarchy system for bone transforms
			boneTransform = boneNodes[i]->GetWorldPosition4x4();
		}

		// Apply inverse bind matrix if available
		if (i < boneInverseBindMatrices.size()) {
			boneMatrices[i] = boneTransform * boneInverseBindMatrices[i];
		}
		else {
			boneMatrices[i] = boneTransform;
		}
	}

	return boneMatrices;
}

std::shared_ptr<SceneNode> SceneNode::FindOrCreateNode(int nodeIdx, const Scene& model) {
	// First try to find existing node
	auto existing = FindNodeByIndex(nodeIdx);
	if (existing) {
		return existing;
	}

	// Check if nodeIdx is valid
	if (nodeIdx < 0 || nodeIdx >= (int)model.nodes.size()) {
		std::cerr << "[SceneNode] Invalid node index: " << nodeIdx << std::endl;
		return nullptr;
	}

	// Create new node
	auto newNode = std::make_shared<SceneNode>();
	newNode->nodeIndex = nodeIdx;

	// Set up the node based on model data
	const auto& modelNode = model.nodes[nodeIdx];

	// Set transform from model (Scene::NodeInfo has localTransform)
	newNode->SetTransform(modelNode.localTransform);

	// Set name if available
	if (!modelNode.name.empty()) {
		// Store name - for now we'll just log it since SceneNode doesn't have a name field
		std::cout << "[SceneNode] Created node '" << modelNode.name << "' with index: " << nodeIdx << std::endl;
	}
	else {
		std::cout << "[SceneNode] Created unnamed node with index: " << nodeIdx << std::endl;
	}

	// Add as child to this node (assuming this is the parent)
	AddChild(newNode);

	return newNode;
}