#include "LightManager.h"
#include "LightNode.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "DirectionalLight.h"
#include "SpotLight.h"
#include "PointLight.h"
#include "TextureUnits.h"
#include "Camera.h"
#include "ShadowMapper.h"
#include "FrameBuffer.h"
#include "MDIBatch.h"
#include <algorithm>
#include <iostream>
#include <chrono>
#include <limits>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// --- LightProxy implementations ---
void LightManager::LightProxy::UpdateProxy()
{
	proxyCenter = glm::vec3(0.0f);
	proxyRadius = 1.0f;
	if (lightNode && lightNode->GetLight()) {
		auto light = lightNode->GetLight();
		proxyCenter = light->GetPosition();
		switch (light->GetLightType()) {
		case BaseLight::LightType::DIRECTIONAL: proxyRadius = 1.0f; break;
		case BaseLight::LightType::POINT:       proxyRadius = glm::min(2.0f, light->GetRange() * 0.1f); break;
		case BaseLight::LightType::SPOT:        proxyRadius = glm::min(2.0f, light->GetRange() * 0.15f); break;
		default: proxyRadius = 1.0f; break;
		}
		proxyRadius = glm::max(0.2f, proxyRadius);
	}
}

bool LightManager::LightProxy::RayIntersects(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float& distance) const
{
	glm::vec3 toCenter = proxyCenter - rayOrigin;
	float projLength = glm::dot(toCenter, rayDir);
	if (projLength < 0.0f) return false;
	glm::vec3 closestPoint = rayOrigin + rayDir * projLength;
	float d = glm::length(closestPoint - proxyCenter);
	if (d <= proxyRadius) {
		distance = projLength - sqrtf(proxyRadius * proxyRadius - d * d);
		return distance >= 0.0f;
	}
	return false;
}

LightManager::LightManager()
{
	// Initialize default shadow configuration
	shadowConfig.maxDirectionalLights = 2;
	shadowConfig.maxSpotLights = 4;
	shadowConfig.maxPointLights = 4;
	shadowConfig.baseResolution = 1024;
	shadowConfig.enablePCSS = true;
	shadowConfig.dynamicResolution = true;

	// Initialize performance stats
	memset(&stats, 0, sizeof(stats));
	m_frameCounter = 0;
	m_roundRobinSpot = 0;
	m_roundRobinPoint = 0;

	std::cout << "[LightManager] Created with shadow configuration:" << std::endl;
	std::cout << "  - Max directional lights: " << shadowConfig.maxDirectionalLights << std::endl;
	std::cout << "  - Max spot lights: " << shadowConfig.maxSpotLights << std::endl;
	std::cout << "  - Max point lights: " << shadowConfig.maxPointLights << std::endl;
	std::cout << "  - Base resolution: " << shadowConfig.baseResolution << std::endl;
}

LightManager::~LightManager()
{
	std::cout << "[LightManager] Cleaning up GPU resources..." << std::endl;

	// Cleanup GPU resources
	if (m_lightDataSSBO) {
		glDeleteBuffers(1, &m_lightDataSSBO);
		m_lightDataSSBO = 0;
	}
	if (m_shadowMatricesSSBO) {
		glDeleteBuffers(1, &m_shadowMatricesSSBO);
		m_shadowMatricesSSBO = 0;
	}
	if (m_tileDataSSBO) {
		glDeleteBuffers(1, &m_tileDataSSBO);
		m_tileDataSSBO = 0;
	}
	if (m_shadowArrayTexture) {
		glDeleteTextures(1, &m_shadowArrayTexture);
		m_shadowArrayTexture = 0;
	}
	// m_shadowFBO is now a unique_ptr and will be cleaned up automatically

	std::cout << "[LightManager] Cleanup completed" << std::endl;
}

void LightManager::RegisterLight(std::shared_ptr<BaseLight> light, const std::string& name)
{
	if (!light) {
		std::cerr << "[LightManager] Cannot register null light!" << std::endl;
		return;
	}

	std::string lightName = name.empty() ? GenerateLightName(light->GetLightType()) : name;
	m_lights[lightName] = light;
	UpdateActiveLights();

	std::cout << "[LightManager] Registered " << lightName << " (Type: "
		<< static_cast<int>(light->GetLightType()) << ", Enabled: " << light->IsEnabled()
		<< ", Casts Shadows: " << light->CastsShadows() << ")" << std::endl;
}

void LightManager::RegisterLightNode(std::shared_ptr<LightNode> lightNode, const std::string& name)
{
	if (lightNode && lightNode->GetLight()) {
		m_lightNodes.push_back(lightNode);
		RegisterLight(lightNode->GetLight(), name);

		std::cout << "[LightManager] Registered light node with light" << std::endl;
	}
	else {
		std::cerr << "[LightManager] Cannot register invalid light node!" << std::endl;
	}
}

void LightManager::UnregisterLight(const std::string& name)
{
	auto it = m_lights.find(name);
	if (it != m_lights.end()) {
		// Clear shadow mapping
		m_lightShadowInfo.erase(it->second.get());
		m_lights.erase(it);
		UpdateActiveLights();
		std::cout << "[LightManager] Unregistered " << name << std::endl;
	}
}

void LightManager::UnregisterLight(std::shared_ptr<BaseLight> light)
{
	for (auto it = m_lights.begin(); it != m_lights.end(); ++it) {
		if (it->second == light) {
			std::cout << "[LightManager] Unregistered " << it->first << std::endl;
			m_lightShadowInfo.erase(light.get());
			m_lights.erase(it);
			UpdateActiveLights();
			break;
		}
	}
}

std::shared_ptr<BaseLight> LightManager::GetLight(const std::string& name) const
{
	auto it = m_lights.find(name);
	return (it != m_lights.end()) ? it->second : nullptr;
}

std::vector<std::shared_ptr<BaseLight>> LightManager::GetAllLights() const
{
	std::vector<std::shared_ptr<BaseLight>> lights;
	lights.reserve(m_lights.size());
	for (const auto& pair : m_lights) {
		lights.push_back(pair.second);
	}
	return lights;
}

std::vector<std::shared_ptr<BaseLight>> LightManager::GetLightsByType(BaseLight::LightType type) const
{
	std::vector<std::shared_ptr<BaseLight>> lights;
	for (const auto& pair : m_lights) {
		if (pair.second->GetLightType() == type) {
			lights.push_back(pair.second);
		}
	}
	return lights;
}

std::vector<std::shared_ptr<BaseLight>> LightManager::GetEnabledLights() const
{
	return m_activeLights;
}

std::vector<std::shared_ptr<BaseLight>> LightManager::GetShadowCastingLights() const
{
	std::vector<std::shared_ptr<BaseLight>> shadowLights;
	for (const auto& light : m_activeLights) {
		if (light->CastsShadows()) {
			shadowLights.push_back(light);
		}
	}
	return shadowLights;
}

void LightManager::InitializeShadowSystem(int maxShadowCastingLights, int baseResolution)
{
	if (m_shadowSystemInitialized) {
		std::cout << "[LightManager] Shadow system already initialized" << std::endl;
		return;
	}

	shadowConfig.baseResolution = baseResolution;
	// Fast path by default: disable PCSS and rely on hardware PCF
	shadowConfig.enablePCSS = false;

	std::cout << "[LightManager] Initializing unified shadow system:" << std::endl;
	std::cout << "  - Base resolution: " << baseResolution << "x" << baseResolution << std::endl;

	// Calculate total shadow map layers needed with proper allocation
	int totalLayers = shadowConfig.maxDirectionalLights * 4 + // 4 cascades each
		shadowConfig.maxSpotLights * 1 +        // 1 shadow map each
		shadowConfig.maxPointLights * 6;        // 6 faces each (cubemap)

	m_shadowArrayLayers = totalLayers;

	std::cout << "  - Total shadow layers: " << totalLayers << std::endl;
	std::cout << "  - Directional lights: " << shadowConfig.maxDirectionalLights << " x 4 cascades" << std::endl;
	std::cout << "  - Spot lights: " << shadowConfig.maxSpotLights << std::endl;
	std::cout << "  - Point lights: " << shadowConfig.maxPointLights << " x 6 faces" << std::endl;

	// Create framebuffer for shadow rendering using the FrameBuffer class (reduced bandwidth: D24)
	m_shadowFBO = std::make_unique<FrameBuffer>(
		baseResolution, baseResolution,
		std::vector<GLenum>{}, // No color attachments
		true,                  // Use depth as texture
		true,                  // Use depth as texture array
		totalLayers,           // Number of layers
		false,                 // No stencil
		GL_DEPTH_COMPONENT24   // Switch to D24 for lower bandwidth
	);

	if (!m_shadowFBO->IsComplete()) {
		std::cerr << "[LightManager] Shadow FBO incomplete!" << std::endl;
		m_shadowFBO.reset();
		return;
	}

	// Use the depth array from the framebuffer as our shadow texture array
	m_shadowArrayTexture = m_shadowFBO->GetDepthArray();

	// Configure depth array texture parameters (compare mode, filtering, wrap) ONE-TIME
	GLint prevActiveTexture;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
	glActiveTexture(GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY);
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowArrayTexture);

	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);

	// Hardware depth comparison for PCF
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

	float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
	glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderColor);

	// Restore active texture
	glActiveTexture(prevActiveTexture);

	// Initialize shadow slice management
	m_shadowSlices.resize(totalLayers);
	for (int i = 0; i < totalLayers; ++i) {
		m_shadowSlices[i] = {
			-1,                                    // lightIndex
			i,                                     // arrayIndex
			baseResolution,                        // resolution
			false,                                 // inUse
			BaseLight::LightType::DIRECTIONAL     // lightType (default)
		};
	}

	// allocate cached slices meta
	m_cachedSlices.clear();
	m_cachedSlices.resize(totalLayers);
	for (int i = 0; i < totalLayers; ++i) {
		m_cachedSlices[i].lastMatrix = glm::mat4(1.0f);
		m_cachedSlices[i].resolution = baseResolution;
		m_cachedSlices[i].inUse = false;
		m_cachedSlices[i].age = 0;
		m_cachedSlices[i].dirtyBits = DIRTY_NONE;
		m_cachedSlices[i].lastUpdateFrame = 0;
		m_cachedSlices[i].lastUpdateMs = 0.0f;
	}

	// Initialize GPU buffers for light data and shadow matrices
	glGenBuffers(1, &m_lightDataSSBO);
	glGenBuffers(1, &m_shadowMatricesSSBO);
	glGenBuffers(1, &m_tileDataSSBO);

	// Pre-allocate shadow matrices buffer with identity matrices
	std::vector<glm::mat4> identityMatrices(totalLayers, glm::mat4(1.0f));
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowMatricesSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER,
		totalLayers * sizeof(glm::mat4),
		identityMatrices.data(), GL_DYNAMIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	m_shadowSystemInitialized = true;
	m_buffersInitialized = true;

	// Validate shadow array immediately after creation
	ValidateShadowArrayTexture();

	std::cout << "[LightManager] Shadow system initialized successfully:" << std::endl;
	std::cout << "  - Shadow array texture ID: " << m_shadowArrayTexture << std::endl;
	std::cout << "  - Shadow FBO ID: " << m_shadowFBO->GetFBO() << std::endl;
	std::cout << "  - PCSS enabled: " << (shadowConfig.enablePCSS ? "Yes" : "No") << std::endl;
}

GLuint LightManager::GetShadowArrayTexture() const
{
	return m_shadowArrayTexture;
}

void LightManager::ValidateShadowArrayTexture() const
{
	if (!m_shadowSystemInitialized || m_shadowArrayTexture == 0) {
		std::cerr << "[LightManager] WARNING: Shadow array texture not initialized!" << std::endl;
		return;
	}

	// Save current state
	GLint prevActiveTexture;
	GLint prevArrayBinding;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
	glActiveTexture(GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY);
	glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevArrayBinding);

	// Validate our shadow array is still bound correctly
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadowArrayTexture);

	GLint width = 0, height = 0, layers = 0, format = 0;
	GLint compareMode = 0, compareFunc = 0;

	glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &height);
	glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &layers);
	glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);

	glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, &compareMode);
	glGetTexParameteriv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, &compareFunc);

	bool hasErrors = false;

	if (width != shadowConfig.baseResolution || height != shadowConfig.baseResolution ||
		layers != m_shadowArrayLayers || format != GL_DEPTH_COMPONENT32F) {
		std::cerr << "[LightManager] CRITICAL: Shadow array texture corrupted!" << std::endl;
		std::cerr << "  Expected: " << shadowConfig.baseResolution << "x" << shadowConfig.baseResolution
			<< "x" << m_shadowArrayLayers << " format=" << GL_DEPTH_COMPONENT32F << std::endl;
		std::cerr << "  Actual: " << width << "x" << height << "x" << layers << " format=" << format << std::endl;
		hasErrors = true;
	}

	if (compareMode != GL_COMPARE_REF_TO_TEXTURE || compareFunc != GL_LEQUAL) {
		std::cerr << "[LightManager] WARNING: Shadow comparison parameters incorrect!" << std::endl;

		// CRITICAL FIX: Restore shadow comparison parameters
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

		std::cout << "[LightManager] Fixed shadow comparison parameters" << std::endl;
	}

	if (!hasErrors) {
		static int validationCount = 0;
		if (validationCount++ % 60 == 0) { // Log every 60 validations to avoid spam
			std::cout << "[LightManager] Shadow array validated OK: ID=" << m_shadowArrayTexture
				<< " " << width << "x" << height << "x" << layers
				<< " (validation #" << validationCount << ")" << std::endl;
		}
	}

	// Check OpenGL errors
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		std::cerr << "[LightManager] OpenGL error during shadow validation: " << std::hex << error << std::dec << std::endl;
	}

	// Restore state
	glBindTexture(GL_TEXTURE_2D_ARRAY, prevArrayBinding);
	glActiveTexture(prevActiveTexture);
}

// Conservative sphere vs clip test
static bool SphereIntersectsLightClip(const glm::vec3& centerWorld, float radiusWorld, const glm::mat4& lightSpace)
{
	// Project center to clip -> NDC
	glm::vec4 clip = lightSpace * glm::vec4(centerWorld, 1.0f);
	if (clip.w == 0.0f) return true; // avoid division issues, keep it
	glm::vec3 ndc = glm::vec3(clip) / clip.w; // [-1,1]

	// Simple conservative test: expand clip bounds by a generous margin based on radius
	// Approximate projected radius by using depth-based dilation; larger depth => allow more slack
	float depthFactor = 1.0f + std::abs(ndc.z) * 0.5f;
	float margin = 0.2f * depthFactor; // expand frustum a bit to avoid popping

	if (ndc.x < -1.0f - margin) return false;
	if (ndc.x > 1.0f + margin) return false;
	if (ndc.y < -1.0f - margin) return false;
	if (ndc.y > 1.0f + margin) return false;
	if (ndc.z < -1.0f - margin) return false;
	if (ndc.z > 1.0f + margin) return false;
	return true;
}

// Snap directional cascade to texel grid to minimize jitter
static glm::mat4 SnapCascadeToTexels(const glm::mat4& lightSpace, int shadowMapSize)
{
	// Assumes standard clip [-1,1]; snap translation (row 3) to texel-sized increments.
	glm::mat4 snapped = lightSpace;
	float texel = 2.0f / float(shadowMapSize);
	glm::vec4 col3 = snapped[3];
	col3.x = std::floor(col3.x / texel) * texel;
	col3.y = std::floor(col3.y / texel) * texel;
	snapped[3] = col3;
	return snapped;
}

// Build light caster signature (centroid + count)
static void BuildCasterSignature(const MDIBatch& batch, glm::vec3& centroidSum, int& count)
{
	centroidSum = glm::vec3(0.0f); count = 0; const auto& objs = batch.GetObjects();
	for (const auto& o : objs) { glm::vec3 c = glm::vec3(o.modelMatrix[3]); centroidSum += c; ++count; }
}

void LightManager::RenderShadowMaps(const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const glm::mat4& view, const glm::mat4& projection,
	float nearPlane, float farPlane, float aspect, float fov)
{
	if (!m_shadowSystemInitialized || !sceneGraph || m_shadowShader == 0) {
		std::cout << "[LightManager] Shadow rendering skipped - system not ready" << std::endl;
		return;
	}

	auto frameStart = std::chrono::high_resolution_clock::now();
	++m_frameCounter;
	m_lightShadowInfo.clear();

	// Build full batch once
	MDIBatch fullBatch; sceneGraph->CollectRenderableObjects(fullBatch);

	glUseProgram(m_shadowShader);
	const GLint locObjectIndex = glGetUniformLocation(m_shadowShader, "uObjectIndex");
	const GLint locLS = glGetUniformLocation(m_shadowShader, "lightSpaceMatrix");

	// Seed matrices with cached values
	std::vector<glm::mat4> matrices; matrices.reserve(m_shadowArrayLayers);
	for (int i = 0; i < m_shadowArrayLayers; ++i) {
		matrices.push_back(m_cachedSlices[i].lastMatrix);
		if (m_cachedSlices[i].inUse) m_cachedSlices[i].age++;
	}

	// Prepare FBO state once
	m_shadowFBO->Bind();
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);
	glDisable(GL_BLEND);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);

	// Helper: filter objects and build signature
	auto filterAndSign = [&](const glm::mat4& ls) { MDIBatch filtered; const auto& objs = fullBatch.GetObjects(); for (const auto& obj : objs) { glm::vec3 localCenter = glm::vec3(obj.boundingSphere); float localRadius = obj.boundingSphere.w; glm::vec3 worldCenter = glm::vec3(obj.modelMatrix * glm::vec4(localCenter, 1.0f)); float sx = glm::length(glm::vec3(obj.modelMatrix[0])); float sy = glm::length(glm::vec3(obj.modelMatrix[1])); float sz = glm::length(glm::vec3(obj.modelMatrix[2])); float scaleMax = std::max(sx, std::max(sy, sz)); float worldRadius = localRadius * scaleMax; if (SphereIntersectsLightClip(worldCenter, worldRadius, ls)) filtered.AddObject(obj); } glm::vec3 sigC; int sigN; BuildCasterSignature(filtered, sigC, sigN); return std::tuple<MDIBatch, glm::vec3, int>(std::move(filtered), sigC, sigN); };

	auto decideUpdate = [&](int slice, BaseLight::LightType type, int lightIdx, int subIndex, const glm::mat4& newMatrix, const glm::vec3& curPos, const glm::vec3& curDir, const glm::vec3& casterCentroid, int casterCount, unsigned cadence)->unsigned int {
		auto& c = m_cachedSlices[slice]; unsigned int reason = DIRTY_NONE; bool cadenceHit = (m_frameCounter % cadence) == 0; const float posThresh = 0.01f; const float dirThresh = 0.0025f; if (glm::length(curPos - c.lastLightPos) > posThresh) reason |= DIRTY_LIGHT_TRANSFORM; float d = glm::dot(glm::normalize(curDir), glm::normalize(c.lastLightDir)); if (d < 1.0f - dirThresh) reason |= DIRTY_LIGHT_TRANSFORM; if (type == BaseLight::LightType::DIRECTIONAL) { // detect camera cascade shift by matrix translation difference
			if (!glm::all(glm::epsilonEqual(glm::vec4(newMatrix[3]), glm::vec4(c.lastMatrix[3]), 1e-4f))) reason |= DIRTY_CAMERA_CASCADE;
		}
		if (casterCount != c.casterCount || glm::length(casterCentroid - c.casterCentroidSum) > 1e-4f) reason |= DIRTY_CASTERS_CHANGED;
		if (reason == DIRTY_NONE && !cadenceHit) return DIRTY_NONE; // reuse
		return reason; // update (may still be DIRTY_NONE if cadence forced a refresh – we allow that)
		};

	auto renderSlice = [&](int sliceIndex, const glm::mat4& lightSpace, MDIBatch& filtered, BaseLight::LightType type, int lightIdx, int subIndex) 
		{ auto sliceStart = std::chrono::high_resolution_clock::now(); glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadowArrayTexture, 0, sliceIndex); glViewport(0, 0, shadowConfig.baseResolution, shadowConfig.baseResolution); glClear(GL_DEPTH_BUFFER_BIT); if (locLS >= 0) glUniformMatrix4fv(locLS, 1, GL_FALSE, glm::value_ptr(lightSpace)); bool useOffset = (type == BaseLight::LightType::DIRECTIONAL && subIndex == 0); if (useOffset) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(2.0f, 4.0f); } if (locObjectIndex < 0) filtered.RenderBatchedByVAO(GL_TRIANGLES, GL_UNSIGNED_INT); else filtered.RenderBatchedByVAOWithUniform(GL_TRIANGLES, GL_UNSIGNED_INT, locObjectIndex); if (useOffset) glDisable(GL_POLYGON_OFFSET_FILL); auto sliceEnd = std::chrono::high_resolution_clock::now(); float ms = std::chrono::duration<float, std::milli>(sliceEnd - sliceStart).count(); auto& c = m_cachedSlices[sliceIndex]; c.lastMatrix = lightSpace; c.lastLightPos = m_activeLights[lightIdx]->GetPosition(); c.lastLightDir = m_activeLights[lightIdx]->GetDirection(); c.type = type; c.lightIndex = lightIdx; c.subIndex = subIndex; c.inUse = true; c.age = 0; c.lastUpdateFrame = m_frameCounter; c.lastUpdateMs = ms; };

	int currentSlice = 0;

	for (size_t li = 0; li < m_activeLights.size() && currentSlice < m_shadowArrayLayers; ++li) {
		auto& light = m_activeLights[li]; if (!light->CastsShadows()) continue; int startSliceForLight = currentSlice;
		if (light->GetLightType() == BaseLight::LightType::DIRECTIONAL) {
			std::vector<float> splits = ShadowMapper::ComputeCascadeSplits(nearPlane, farPlane, 4, 0.6f); glm::vec3 lightDir = glm::normalize(light->GetDirection()); glm::vec3 lightPos = light->GetPosition(); float prev = nearPlane; for (int cIdx = 0; cIdx < 4 && currentSlice < m_shadowArrayLayers; ++cIdx) {
				float cNear = (cIdx == 0) ? nearPlane : prev; float cFar = splits[cIdx]; prev = cFar; glm::mat4 ls = ShadowMapper::ComputeCascadeLightSpace(cNear, cFar, view, projection, lightPos, lightDir, aspect, fov, cIdx, shadowConfig.baseResolution); ls = SnapCascadeToTexels(ls, shadowConfig.baseResolution); auto tuple = filterAndSign(ls); MDIBatch filtered = std::move(std::get<0>(tuple)); glm::vec3 sigC = std::get<1>(tuple); int sigN = std::get<2>(tuple); unsigned cadence = (cIdx == 0) ? 1u : (cIdx == 1) ? 2u : (cIdx == 2) ? 3u : 7u; unsigned int reason = decideUpdate(currentSlice, BaseLight::LightType::DIRECTIONAL, (int)li, cIdx, ls, lightPos, lightDir, sigC, sigN, cadence); if (reason != DIRTY_NONE) { renderSlice(currentSlice, ls, filtered, BaseLight::LightType::DIRECTIONAL, (int)li, cIdx); m_cachedSlices[currentSlice].dirtyBits = reason; m_cachedSlices[currentSlice].casterCentroidSum = sigC; m_cachedSlices[currentSlice].casterCount = sigN; matrices[currentSlice] = ls; }
				else { // reuse
					m_cachedSlices[currentSlice].inUse = true; m_cachedSlices[currentSlice].type = BaseLight::LightType::DIRECTIONAL; m_cachedSlices[currentSlice].lightIndex = (int)li; m_cachedSlices[currentSlice].subIndex = cIdx;
				}
				++currentSlice;
			}
			m_lightShadowInfo[light.get()] = { startSliceForLight, 4 };
		}
		else if (light->GetLightType() == BaseLight::LightType::SPOT) {
			bool rrSelected = ((m_roundRobinSpot++) % 2) == 0; glm::vec3 lightPos = light->GetPosition(); glm::vec3 lightDir = glm::normalize(light->GetDirection()); glm::vec3 up = (fabs(glm::dot(lightDir, glm::vec3(0, 1, 0))) > 0.95f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0); glm::mat4 viewL = glm::lookAt(lightPos, lightPos + lightDir, up); glm::mat4 projL = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, light->GetRange() * 1.1f); glm::mat4 ls = projL * viewL; auto tuple = filterAndSign(ls); MDIBatch filtered = std::move(std::get<0>(tuple)); glm::vec3 sigC = std::get<1>(tuple); int sigN = std::get<2>(tuple); unsigned cadence = rrSelected ? 1u : 4u; unsigned reason = decideUpdate(currentSlice, BaseLight::LightType::SPOT, (int)li, 0, ls, lightPos, lightDir, sigC, sigN, cadence); if (reason != DIRTY_NONE) { renderSlice(currentSlice, ls, filtered, BaseLight::LightType::SPOT, (int)li, 0); m_cachedSlices[currentSlice].dirtyBits = reason; m_cachedSlices[currentSlice].casterCentroidSum = sigC; m_cachedSlices[currentSlice].casterCount = sigN; matrices[currentSlice] = ls; }
			else { m_cachedSlices[currentSlice].inUse = true; m_cachedSlices[currentSlice].type = BaseLight::LightType::SPOT; m_cachedSlices[currentSlice].lightIndex = (int)li; m_cachedSlices[currentSlice].subIndex = 0; }
			++currentSlice; m_lightShadowInfo[light.get()] = { startSliceForLight,1 };
		}
		else if (light->GetLightType() == BaseLight::LightType::POINT) {
			unsigned faceUpdate = (m_roundRobinPoint++) % 6; float range = light->GetRange(); glm::mat4 proj90 = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, range * 1.2f); const glm::vec3 dirs[6] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} }; const glm::vec3 ups[6] = { {0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0} }; for (int face = 0; face < 6 && currentSlice < m_shadowArrayLayers; ++face) {
				glm::vec3 lightPos = light->GetPosition(); glm::mat4 viewFace = glm::lookAt(lightPos, lightPos + dirs[face], ups[face]); glm::mat4 ls = proj90 * viewFace; auto tuple = filterAndSign(ls); MDIBatch filtered = std::move(std::get<0>(tuple)); glm::vec3 sigC = std::get<1>(tuple); int sigN = std::get<2>(tuple); unsigned cadence = (face == (int)faceUpdate) ? 1u : 8u; unsigned reason = decideUpdate(currentSlice, BaseLight::LightType::POINT, (int)li, face, ls, lightPos, dirs[face], sigC, sigN, cadence); if (reason != DIRTY_NONE) { renderSlice(currentSlice, ls, filtered, BaseLight::LightType::POINT, (int)li, face); m_cachedSlices[currentSlice].dirtyBits = reason; m_cachedSlices[currentSlice].casterCentroidSum = sigC; m_cachedSlices[currentSlice].casterCount = sigN; matrices[currentSlice] = ls; }
				else { m_cachedSlices[currentSlice].inUse = true; m_cachedSlices[currentSlice].type = BaseLight::LightType::POINT; m_cachedSlices[currentSlice].lightIndex = (int)li; m_cachedSlices[currentSlice].subIndex = face; }
				++currentSlice;
			}
			m_lightShadowInfo[light.get()] = { startSliceForLight,6 };
		}
	}

	// Pad remaining matrices
	while ((int)matrices.size() < m_shadowArrayLayers) matrices.push_back(glm::mat4(1.0f));

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_shadowMatricesSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, matrices.size() * sizeof(glm::mat4), matrices.data(), GL_DYNAMIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	FrameBuffer::Unbind();

	auto frameEnd = std::chrono::high_resolution_clock::now();
	stats.shadowUpdateTime = std::chrono::duration<float, std::milli>(frameEnd - frameStart).count();
}

std::vector<glm::mat4> LightManager::RenderDirectionalLightShadows(const std::shared_ptr<SceneGraph>&, std::shared_ptr<BaseLight>, const glm::mat4&, const glm::mat4&, float, float, float, float, int) { return {}; }
glm::mat4 LightManager::RenderSpotLightShadow(const std::shared_ptr<SceneGraph>&, std::shared_ptr<BaseLight>, int) { return glm::mat4(1.0f); }
std::vector<glm::mat4> LightManager::RenderPointLightShadows(const std::shared_ptr<SceneGraph>&, std::shared_ptr<BaseLight>, int) { return {}; }

void LightManager::UpdateGPUBuffers()
{
	if (!m_buffersInitialized) return; auto startTime = std::chrono::high_resolution_clock::now(); std::vector<LightData> arr; arr.reserve(m_activeLights.size());
	for (auto& light : m_activeLights) { LightData d{}; d.position = glm::vec4(light->GetPosition(), (float)light->GetLightType()); d.direction = glm::vec4(light->GetDirection(), 0.0f); d.color = glm::vec4(light->GetEffectiveColor(), light->GetIntensity()); glm::vec3 att = light->GetAttenuation(); d.attenuation = glm::vec4(att, light->GetRange()); auto it = m_lightShadowInfo.find(light.get()); if (it != m_lightShadowInfo.end()) d.shadowData = glm::vec4((float)it->second.startSlice, (float)it->second.count, light->CastsShadows() ? 1.0f : 0.0f, shadowConfig.enablePCSS ? 1.0f : 0.0f); else d.shadowData = glm::vec4(-1, 0, 0, 0); if (light->GetLightType() == BaseLight::LightType::SPOT) { if (auto s = std::dynamic_pointer_cast<SpotLight>(light)) { float inner = glm::cos(glm::radians(s->GetCutOff())); float outer = glm::cos(glm::radians(s->GetOuterCutOff())); d.spotData = glm::vec4(inner, outer, 0, 0); } else d.spotData = glm::vec4(glm::cos(glm::radians(20.0f)), glm::cos(glm::radians(30.0f)), 0, 0); } else d.spotData = glm::vec4(0); arr.push_back(d); } if (!arr.empty()) { glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lightDataSSBO); glBufferData(GL_SHADER_STORAGE_BUFFER, arr.size() * sizeof(LightData), arr.data(), GL_DYNAMIC_DRAW); glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0); } auto endTime = std::chrono::high_resolution_clock::now(); stats.bufferUpdateTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
}

void LightManager::UpdateActiveLights()
{
	m_activeLights.clear(); for (auto& p : m_lights) if (p.second && p.second->IsEnabled()) m_activeLights.push_back(p.second); std::sort(m_activeLights.begin(), m_activeLights.end(), [](const std::shared_ptr<BaseLight>& a, const std::shared_ptr<BaseLight>& b) { if (a->GetLightType() != b->GetLightType()) return (int)a->GetLightType() < (int)b->GetLightType(); return a->GetIntensity() > b->GetIntensity(); });
}

std::string LightManager::GenerateLightName(BaseLight::LightType type)
{
 switch (type) { case BaseLight::LightType::DIRECTIONAL: return "DirectionalLight_" + std::to_string(++m_directionalCount); case BaseLight::LightType::POINT: return "PointLight_" + std::to_string(++m_pointCount); case BaseLight::LightType::SPOT: return "SpotLight_" + std::to_string(++m_spotCount); default: return "UnknownLight_" + std::to_string(++m_areaCount); }
}

void LightManager::CollectLightsFromScene(std::shared_ptr<SceneGraph> sceneGraph)
{
	if (!sceneGraph) return; auto lightNodes = sceneGraph->FindNodesByType(SceneNode::LIGHT); for (auto& node : lightNodes) if (auto ln = std::dynamic_pointer_cast<LightNode>(node)) if (ln->GetLight()) RegisterLightNode(ln);
}

void LightManager::UpdateLights(float dt)
{
	for (auto& p : m_lights) if (p.second) p.second->Update(dt); for (auto& ln : m_lightNodes) if (ln) ln->UpdateLightFromTransform();
}

void LightManager::UpdateLightProxies()
{
	m_lightProxies.clear(); m_lightProxies.reserve(m_lightNodes.size()); for (auto& ln : m_lightNodes) if (ln && ln->GetLight()) { LightProxy pr; pr.lightNode = ln; pr.UpdateProxy(); m_lightProxies.push_back(pr); }
}

void LightManager::ClearAllLights()
{
	m_lights.clear(); m_activeLights.clear(); m_lightNodes.clear(); m_lightProxies.clear(); m_lightShadowInfo.clear(); m_directionalCount = m_pointCount = m_spotCount = m_areaCount = 0;
}

std::shared_ptr<LightNode> LightManager::FindLightAtRay(const glm::vec3& rayOrigin, const glm::vec3& rayDir) const
{
	float closest = std::numeric_limits<float>::max(); std::shared_ptr<LightNode> best = nullptr; for (auto& proxy : m_lightProxies) { float dist; if (proxy.RayIntersects(rayOrigin, rayDir, dist) && dist < closest) { closest = dist; best = proxy.lightNode; } } return best;
}

void LightManager::EnableAllLights() { for (auto& p : m_lights) p.second->SetEnabled(true); UpdateActiveLights(); }
void LightManager::DisableAllLights() { for (auto& p : m_lights) p.second->SetEnabled(false); UpdateActiveLights(); }
void LightManager::EnableLightsByType(BaseLight::LightType t) { for (auto& p : m_lights) if (p.second->GetLightType() == t) p.second->SetEnabled(true); UpdateActiveLights(); }
void LightManager::DisableLightsByType(BaseLight::LightType t) { for (auto& p : m_lights) if (p.second->GetLightType() == t) p.second->SetEnabled(false); UpdateActiveLights(); }

size_t LightManager::GetEnabledLightCount() const { return m_activeLights.size(); }
size_t LightManager::GetShadowCastingLightCount() const { return GetShadowCastingLights().size(); }
size_t LightManager::GetDirectionalLightCount() const { return GetLightsByType(BaseLight::LightType::DIRECTIONAL).size(); }
size_t LightManager::GetPointLightCount() const { return GetLightsByType(BaseLight::LightType::POINT).size(); }
size_t LightManager::GetSpotLightCount() const { return GetLightsByType(BaseLight::LightType::SPOT).size(); }

void LightManager::PrintLightInfo() const
{
	std::cout << "[LightManager] Lights total=" << m_lights.size() << " enabled=" << GetEnabledLightCount() << " shadowCasting=" << GetShadowCastingLightCount() << std::endl;
}

void LightManager::PrintPerformanceStats() const
{
	std::cout << "[LightManager] Shadow ms=" << stats.shadowUpdateTime << " Buffer ms=" << stats.bufferUpdateTime << " Culling ms=" << stats.lightCullingTime << std::endl;
}

void LightManager::PerformLightCulling(const glm::mat4&, const glm::mat4&, int screenWidth, int screenHeight, int tileSize)
{
	auto start = std::chrono::high_resolution_clock::now(); m_tileCount.x = (screenWidth + tileSize - 1) / tileSize; m_tileCount.y = (screenHeight + tileSize - 1) / tileSize; m_tiles.resize(m_tileCount.x * m_tileCount.y); for (auto& tile : m_tiles) { tile.lightCount = std::min((int)m_activeLights.size(), 64); for (int i = 0; i < tile.lightCount; ++i) tile.lightIndices[i] = i; } if (m_tileDataSSBO && !m_tiles.empty()) { glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_tileDataSSBO); glBufferData(GL_SHADER_STORAGE_BUFFER, m_tiles.size() * sizeof(CullingTile), m_tiles.data(), GL_DYNAMIC_DRAW); glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0); } auto end = std::chrono::high_resolution_clock::now(); stats.lightCullingTime = std::chrono::duration<float, std::milli>(end - start).count(); stats.visibleLights = (int)m_activeLights.size();
}

std::vector<LightManager::SliceDebugInfo> LightManager::GetShadowSliceDebug() const
{
	std::vector<SliceDebugInfo> out;
	out.reserve(m_cachedSlices.size());
	for (size_t i = 0; i < m_cachedSlices.size(); ++i) {
		const auto& c = m_cachedSlices[i];
		SliceDebugInfo d;
		d.arrayIndex = (int)i;
		d.type = c.type;
		d.lightIndex = c.lightIndex;
		d.subIndex = c.subIndex;
		d.age = c.age;
		d.dirtyReason = c.dirtyBits;
		d.lastUpdateMs = c.lastUpdateMs;
		d.inUse = c.inUse;
		out.push_back(d);
	}
	return out;
}