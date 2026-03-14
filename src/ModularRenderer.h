#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
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
class ShadowPass;
class SSAOPass;
class ScreenSpaceShadowPass;
class LightingPass;
class BloomPass;
class TAAPass;
class TransparentForwardPass;
class PostProcessPass;
class LPVPass;
class SSGIPass;
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
		glm::vec3 envColor);

	void CheckGLError(const std::string& passName);
	
	// Debug visualization
	void visualizeDebugMode(RenderContext& ctx);

	RenderContext m_context;

	// Rendering passes (in execution order)
	std::unique_ptr<ShadowPass> m_shadowPass;
	std::unique_ptr<LPVPass> m_lpvPass; 
	std::unique_ptr<GBufferPass> m_gbufferPass;
	std::unique_ptr<RTPass> m_rtPass;  // Path tracing pass
	std::unique_ptr<SSAOPass> m_ssaoPass;
	std::unique_ptr<ScreenSpaceShadowPass> m_screenSpaceShadowPass;
	std::unique_ptr<SSGIPass> m_ssgiPass;
	std::unique_ptr<TAAPass> m_taaPass;
	std::unique_ptr<LightingPass> m_lightingPass;
	std::unique_ptr<BloomPass> m_bloomPass;
	std::unique_ptr<TransparentForwardPass> m_transparentPass;
	std::unique_ptr<PostProcessPass> m_postProcessPass;
	std::unique_ptr<GUIPass> m_guiPass;  // Internal GUI rendering
	std::unique_ptr<DebugBBoxPass> m_debugBBoxPass;  // Debug bounding box visualization

	std::vector<PassTimingMetrics> m_lastPassMetrics;
	float m_lastCpuWaitSyncMs = 0.0f;
};
