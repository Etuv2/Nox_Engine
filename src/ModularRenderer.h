#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include "RenderContext.h"
#include "RenderPass.h"

class FrameBuffer;
class ScreenQuad;
class SceneGraph;
class Camera;
class DirectionalLight;
class Skybox;
class LightManager;

// Forward declare pass classes
class GBufferPass;
class TransformHistoryPass;
class ShadowPass;
class SSAOPass;
class ScreenSpaceShadowPass;
class LightingPass;
class BloomPass;
class TAAPass;
class TransparentForwardPass;
class PostProcessPass;
class LPVPass;
class IndirectDiffusePass;
class GUIPass;
class RTPass;
class DebugBBoxPass;

/**
 * ModularRenderer coordinates all rendering passes using a shared RenderContext.
 * This replaces the monolithic Renderer with a cleaner, more maintainable architecture.
 */
class ModularRenderer
{
public:
	ModularRenderer();
	~ModularRenderer();

	// Initialize all passes and shared resources
	bool Initialize(int windowWidth, int windowHeight);

	// Resize all framebuffers and passes
	void Resize(int newWidth, int newHeight);

	// Main render function
	void Render(const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& lighting,
		float deltaTime,
		float exposure,
		float gamma,
		bool enableShadows,
		float shadowBias,
		float shadow_near,
		float shadow_far,
		glm::vec3 envColor,
		int windowWidth,
		int windowHeight);

	// Access to context for UI tuning
	RenderContext& GetContext() { return m_context; }

	// TAA controls
	void ResetTAA();
	int GetTAAFrameIndex() const;


	struct PassTimingMetrics {
		std::string name;
		float cpuTimeMs = 0.0f;
		float gpuTimeMs = 0.0f;
		uint64_t drawCalls = 0;
		uint64_t dispatchCount = 0;
		uint64_t bufferUploadBytes = 0;
		float cpuWaitSyncMs = 0.0f;
	};

	const std::vector<PassTimingMetrics>& GetLastPassMetrics() const { return m_lastPassMetrics; }
	float GetLastCpuWaitSyncMs() const { return m_lastCpuWaitSyncMs; }

	// Performance: Set to false to disable GL error checking and verbose logging in release builds
	static constexpr bool DebugErrorChecking = false;
	static constexpr bool VerboseLogging = false;
	
private:
	bool InitializeSharedResources();
	void UpdateContext(const std::shared_ptr<Camera>& camera,
		float exposure, float gamma,
		bool enableShadows, float shadowBias,
		float shadowNear, float shadowFar,
		glm::vec3 envColor);

	void CheckGLError(const std::string& passName);

	enum class FrameGraphMode {
		DEFERRED = 0,
		DEFERRED_DEBUG = 1,
		PATH_TRACED = 2
	};

	using ResourceHandle = std::string;

	struct PassDescriptor {
		std::string name;
		std::vector<ResourceHandle> inputs;
		std::vector<ResourceHandle> outputs;
		std::function<bool(const RenderContext&)> condition;
		std::function<void()> execute;
	};

	struct FramePlan {
		std::vector<const PassDescriptor*> executionOrder;
	};

	struct PlanCacheKey {
		FrameGraphMode mode = FrameGraphMode::DEFERRED;
		bool enableBloom = false;
		bool enableSSAO = false;
		bool enableIndirectDiffuse = false;
		bool presentIndirectDiffuseDebug = false;
		bool enableScreenSpaceShadows = false;
		bool enableLPV = false;
		bool enableTAA = false;

		bool operator==(const PlanCacheKey& other) const {
			return mode == other.mode &&
				enableBloom == other.enableBloom &&
				enableSSAO == other.enableSSAO &&
				enableIndirectDiffuse == other.enableIndirectDiffuse &&
				presentIndirectDiffuseDebug == other.presentIndirectDiffuseDebug &&
				enableScreenSpaceShadows == other.enableScreenSpaceShadows &&
				enableLPV == other.enableLPV &&
				enableTAA == other.enableTAA;
		}
	};

	struct PlanCacheKeyHash {
		std::size_t operator()(const PlanCacheKey& key) const;
	};

	void BuildPassDescriptors(
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& lighting,
		const std::shared_ptr<Skybox>& skybox);
	FramePlan CompileFramePlan(const PlanCacheKey& key) const;
	std::vector<ResourceHandle> GetRequiredOutputsForKey(const PlanCacheKey& key) const;
	PlanCacheKey BuildPlanCacheKey() const;
	FrameGraphMode DetermineFrameGraphMode() const;
	void ExecuteFramePlan(const FramePlan& plan);
	
	// Debug visualization
	void visualizeDebugMode(RenderContext& ctx);

	RenderContext m_context;

	// Rendering passes (in execution order)
	std::unique_ptr<ShadowPass> m_shadowPass;
	std::unique_ptr<LPVPass> m_lpvPass; 
	std::unique_ptr<GBufferPass> m_gbufferPass;
	std::unique_ptr<TransformHistoryPass> m_transformHistoryPass;
	std::unique_ptr<RTPass> m_rtPass;  // Path tracing pass
	std::unique_ptr<SSAOPass> m_ssaoPass;
	std::unique_ptr<ScreenSpaceShadowPass> m_screenSpaceShadowPass;
	std::unique_ptr<IndirectDiffusePass> m_indirectDiffusePass;
	std::unique_ptr<TAAPass> m_taaPass;
	std::unique_ptr<LightingPass> m_lightingPass;
	std::unique_ptr<BloomPass> m_bloomPass;
	std::unique_ptr<TransparentForwardPass> m_transparentPass;
	std::unique_ptr<PostProcessPass> m_postProcessPass;
	std::unique_ptr<GUIPass> m_guiPass;  // Internal GUI rendering
	std::unique_ptr<DebugBBoxPass> m_debugBBoxPass;  // Debug bounding box visualization
	GLuint m_debugViewShader = 0;
	GLuint m_indirectDiffuseDebugPresentShader = 0;

	std::vector<PassDescriptor> m_passDescriptors;
	std::unordered_map<PlanCacheKey, FramePlan, PlanCacheKeyHash> m_planCache;
	std::unordered_map<ResourceHandle, GLuint> m_namedResources;
	std::function<void(const char*, const std::function<void()>&)> m_profilePassFunc;

	std::vector<PassTimingMetrics> m_lastPassMetrics;
	float m_lastCpuWaitSyncMs = 0.0f;
};
