#pragma once
#include <vector>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <GL/glew.h>
#include <glm/glm.hpp>
#include "BaseLight.h"
#include "ShadowMapper.h"
#include "Texture.h"  // Use new Texture class

// Forward declarations
class SceneGraph;
class Camera;
class SpotLight;
class LightNode;

/**
 * Enhanced LightManager for multi-light deferred rendering with shadow system integration.
 * Handles collection, management, and GPU buffer packing of all lights in the scene.
 *
 * Features:
 * - Unified shadow array system for all light types
 * - Cascaded shadow maps for directional lights
 * - Point light cubemap shadows (6 faces)
 * - Spot light cone shadows
 * - Hardware PCF support
 * - Dynamic light management
 * - Performance statistics and profiling
 */
class LightManager
{
public:
	LightManager();
	~LightManager();

	// Enhanced light registration with shadow support
	void RegisterLight(std::shared_ptr<BaseLight> light, const std::string& name = "");
	void RegisterLightNode(std::shared_ptr<LightNode> lightNode, const std::string& name = "");
	void UnregisterLight(const std::string& name);
	void UnregisterLight(std::shared_ptr<BaseLight> light);

	// Light access
	std::shared_ptr<BaseLight> GetLight(const std::string& name) const;
	std::vector<std::shared_ptr<BaseLight>> GetAllLights() const;
	std::vector<std::shared_ptr<BaseLight>> GetLightsByType(BaseLight::LightType type) const;
	std::vector<std::shared_ptr<BaseLight>> GetEnabledLights() const;
	std::vector<std::shared_ptr<BaseLight>> GetShadowCastingLights() const;

	// Enhanced multi-light deferred rendering support
	struct LightData {
		glm::vec4 position;        // w = light type (0=dir,1=point,2=spot)
		glm::vec4 direction;       // xyz = direction, w = unused
		glm::vec4 color;           // w = intensity
		glm::vec4 attenuation;     // xyz = constant,linear,quadratic, w = range
		glm::vec4 shadowData;      // x = startSlice, y = sliceCount, z = enabled, w = pcss enabled
		glm::vec4 spotData;        // x = inner cone (cos), y = outer cone (cos), z,w = reserved

		// Additional data for ray tracing compatibility (matches RTLightData in RTStructures.h)
		glm::vec4 areaData;    // xyz = size (for area lights), w = reserved
		glm::vec4 sampling;        // x = PDF weight, y = solid angle, z,w = reserved
	};

	// Light proxy visualization for editor integration
	struct LightProxy {
		std::shared_ptr<LightNode> lightNode;
		glm::vec3 proxyCenter = glm::vec3(0.0f);
		float proxyRadius = 0.0f;
		bool isSelected = false;

		void UpdateProxy();
		bool RayIntersects(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float& distance) const;
	};

	// GPU buffer management for deferred rendering
	void UpdateGPUBuffers();
	GLuint GetLightDataSSBO() const { return m_lightDataSSBO; }
	GLuint GetShadowMatricesSSBO() const { return m_shadowMatricesSSBO; }
	int GetActiveLightCount() const { return static_cast<int>(m_activeLights.size()); }

	// Shadow system integration
	void InitializeShadowSystem(int maxShadowCastingLights = 8, int baseResolution = 1024);
	GLuint GetShadowArrayTexture() const;

	// Shadow shader management
	void SetShadowShader(GLuint shadowShader) { m_shadowShader = shadowShader; }
	GLuint GetShadowShader() const { return m_shadowShader; }

	// Shadow rendering method
	void RenderShadowMaps(const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const glm::mat4& view, const glm::mat4& projection,
		float nearPlane, float farPlane, float aspect, float fov);

	// Shadow rendering helpers with correct return types
	std::vector<glm::mat4> RenderDirectionalLightShadows(const std::shared_ptr<SceneGraph>& sceneGraph,
		std::shared_ptr<BaseLight> light,
		const glm::mat4& view, const glm::mat4& projection,
		float nearPlane, float farPlane, float aspect, float fov,
		int startSlice);

	glm::mat4 RenderSpotLightShadow(const std::shared_ptr<SceneGraph>& sceneGraph,
		std::shared_ptr<BaseLight> light, int slice);

	std::vector<glm::mat4> RenderPointLightShadows(const std::shared_ptr<SceneGraph>& sceneGraph,
		std::shared_ptr<BaseLight> light, int startSlice);

	// Shadow array validation to detect corruption from IBL
	void ValidateShadowArrayTexture() const;

	// Multi-light shadow configuration
	struct ShadowConfig {
		int maxDirectionalLights = 2;
		int maxSpotLights = 4;
		int maxPointLights = 4;
		int baseResolution = 1024;
		bool enablePCSS = true;
		bool dynamicResolution = true;
		
		// Cascade blend settings - distance in view-space units for smooth transitions
		float cascadeBlendDistance = 100.0f; // Distance over which to blend cascades
		float cascadeBlendFactor = 0.15f;   // Fraction of cascade range to use for blending
		
		// Point light shadow settings
		float pointLightBias = 0.002f;      // Base bias for point light shadows
		float pointLightSlopeBias = 0.005f; // Slope-scaled bias for point lights
		float pointLightNormalOffset = 0.01f; // Normal offset (minimal to avoid floating)
	} shadowConfig;

	// Light culling for tiled/clustered deferred rendering
	struct CullingTile {
		int lightCount;
		int lightIndices[64];
	};

	void PerformLightCulling(const glm::mat4& view, const glm::mat4& projection,
		int screenWidth, int screenHeight, int tileSize = 16);
	GLuint GetTileDataSSBO() const { return m_tileDataSSBO; }
	glm::ivec2 GetTileCount() const { return m_tileCount; }

	// Light management
	void EnableAllLights();
	void DisableAllLights();
	void EnableLightsByType(BaseLight::LightType type);
	void DisableLightsByType(BaseLight::LightType type);

	// Update all lights (for animated lights)
	void UpdateLights(float deltaTime);

	// Scene integration
	void CollectLightsFromScene(std::shared_ptr<SceneGraph> sceneGraph);
	void ClearAllLights();

	// Statistics and profiling
	size_t GetLightCount() const { return m_lights.size(); }
	size_t GetEnabledLightCount() const;
	size_t GetShadowCastingLightCount() const;
	size_t GetDirectionalLightCount() const;
	size_t GetPointLightCount() const;
	size_t GetSpotLightCount() const;

	// Performance profiling
	struct PerformanceStats {
		float shadowUpdateTime = 0.0f;
		float lightCullingTime = 0.0f;
		float bufferUpdateTime = 0.0f;
		int visibleLights = 0;
		int shadowCascades = 0;
	} stats;

	// Debug
	void PrintLightInfo() const;
	void PrintPerformanceStats() const;

	// Per-slice debug info for ImGui
	struct SliceDebugInfo {
		int arrayIndex = -1;
		BaseLight::LightType type = BaseLight::LightType::DIRECTIONAL;
		int lightIndex = -1;
		int subIndex = 0;
		unsigned age = 0;
		unsigned int dirtyReason = 0;
		float lastUpdateMs = 0.0f;
		bool inUse = false;
	};
	std::vector<SliceDebugInfo> GetShadowSliceDebug() const;

	// Light proxy management for editor integration
	void UpdateLightProxies();
	std::vector<LightProxy> GetLightProxies() const { return m_lightProxies; }
	std::shared_ptr<LightNode> FindLightAtRay(const glm::vec3& rayOrigin, const glm::vec3& rayDir) const;

private:
	// Light storage
	std::map<std::string, std::shared_ptr<BaseLight>> m_lights;
	std::vector<std::shared_ptr<LightNode>> m_lightNodes;
	std::vector<std::shared_ptr<BaseLight>> m_activeLights;

	// GPU buffer objects for deferred rendering
	GLuint m_lightDataSSBO = 0;
	GLuint m_shadowMatricesSSBO = 0;
	GLuint m_tileDataSSBO = 0;

	// Shadow system using new Texture class
	std::unique_ptr<class FrameBuffer> m_shadowFBO;
	TexturePtr m_shadowArrayTexture;  // Using new Texture class for shadow array
	GLuint m_shadowShader = 0;
	int m_shadowArrayLayers = 0;

	// Per-light shadow slice mapping
	struct LightShadowInfo {
		int startSlice = -1;
		int count = 0;
	};
	std::unordered_map<BaseLight*, LightShadowInfo> m_lightShadowInfo;

	// Dirty bit flags
	enum DirtyBits : unsigned int {
		DIRTY_NONE = 0,
		DIRTY_LIGHT_TRANSFORM = 1u << 0,
		DIRTY_CASTERS_CHANGED = 1u << 1,
		DIRTY_CAMERA_CASCADE = 1u << 2
	};

	// Cached slice metadata for shadow reuse
	struct CachedSlice {
		glm::mat4 lastMatrix = glm::mat4(1.0f);
		glm::vec3 lastLightPos = glm::vec3(0.0f);
		glm::vec3 lastLightDir = glm::vec3(0.0f, -1.0f, 0.0f);
		BaseLight::LightType type = BaseLight::LightType::DIRECTIONAL;
		int lightIndex = -1;
		int subIndex = 0;
		int resolution = 1024;
		bool inUse = false;
		unsigned age = 0;
		unsigned int dirtyBits = DIRTY_NONE;
		int lastUpdateFrame = 0;
		float lastUpdateMs = 0.0f;
		glm::vec3 casterCentroidSum = glm::vec3(0.0f);
		int casterCount = 0;
	};
	std::vector<CachedSlice> m_cachedSlices;

	// Frame / scheduling state
	int m_frameCounter = 0;
	unsigned m_roundRobinSpot = 0;
	unsigned m_roundRobinPoint = 0;

	// Light culling data
	glm::ivec2 m_tileCount = glm::ivec2(0);
	std::vector<CullingTile> m_tiles;

	// Helper methods
	std::string GenerateLightName(BaseLight::LightType type);
	void UpdateActiveLights();

	// Shadow map management
	struct ShadowMapSlice {
		int lightIndex = -1;
		int arrayIndex = -1;
		int resolution = 1024;
		bool inUse = false;
		BaseLight::LightType lightType = BaseLight::LightType::DIRECTIONAL;
	};
	std::vector<ShadowMapSlice> m_shadowSlices;

	// Light type counters for naming
	mutable int m_directionalCount = 0;
	mutable int m_pointCount = 0;
	mutable int m_spotCount = 0;
	mutable int m_areaCount = 0;

	// Light proxy system for editor integration
	std::vector<LightProxy> m_lightProxies;

	// Initialization flags
	bool m_shadowSystemInitialized = false;
	bool m_buffersInitialized = false;
};