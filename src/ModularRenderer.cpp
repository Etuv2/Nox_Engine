#include "ModularRenderer.h"
#include "FrameBuffer.h"
#include "ScreenQuad.h"
#include "SceneGraph.h"
#include "Camera.h"
#include "DirectionalLight.h"
#include "Skybox.h"
#include "LightManager.h"

#include "passes/GBufferPass.h"
#include "passes/ShadowPass.h"
#include "passes/LPVPass.h"
#include "passes/RTPass.h"  // Path tracing pass
#include "passes/SSAOPass.h"
#include "passes/ScreenSpaceShadowPass.h"
#include "passes/SSGIPass.h"

#include "passes/LightingPass.h"
#include "passes/BloomPass.h"
#include "passes/TAAPass.h"
#include "passes/TransparentForwardPass.h"
#include "passes/PostProcessPass.h"
#include "passes/GUIPass.h"  // Internal GUI rendering
#include "passes/DebugBBoxPass.h"  // Debug bounding box visualization
#include <iostream>
#include <chrono>
#include <array>
#include <unordered_map>
#include <functional>
#include <cstdlib>

ModularRenderer::ModularRenderer()
{
}

ModularRenderer::~ModularRenderer()
{

}

namespace {
	using Clock = std::chrono::high_resolution_clock;
	static constexpr std::size_t kProfilerFramesInFlight = 4;
	static constexpr std::size_t kProfilerReadbackDelay = kProfilerFramesInFlight - 1;

	struct PassQuerySlot {
		std::array<GLuint, 2> timestampQueries{0, 0};
		std::array<GLuint, 2> statsQueries{0, 0};
		bool timerIssued = false;
		bool statsIssued = false;
	};

	struct PassQueryState {
		std::array<PassQuerySlot, kProfilerFramesInFlight> slots{};
		bool initialized = false;
	};

	struct GpuProfilerPool {
		std::unordered_map<std::string, PassQueryState> passStates;
		std::size_t frameIndex = 0;
		bool strictTiming = false;

		GpuProfilerPool() {
			// Diagnostic mode: allows strict (potentially blocking) timing if needed.
			strictTiming = std::getenv("NOX_STRICT_GPU_TIMING") != nullptr;
		}

		std::size_t CurrentSlotIndex() const {
			return (frameIndex - 1) % kProfilerFramesInFlight;
		}

		std::size_t ReadbackSlotIndex() const {
			return ((frameIndex - 1) + kProfilerFramesInFlight - kProfilerReadbackDelay) % kProfilerFramesInFlight;
		}

		bool CanReadback() const {
			return frameIndex > kProfilerReadbackDelay;
		}

		void BeginFrame() {
			++frameIndex;
		}

		PassQueryState& GetOrCreatePassState(const std::string& passName) {
			auto [it, inserted] = passStates.try_emplace(passName);
			if (inserted || !it->second.initialized) {
				InitializePassState(it->second);
			}
			return it->second;
		}

		static bool AreQueryResultsReady(const std::array<GLuint, 2>& queries) {
			GLint availableA = GL_FALSE;
			GLint availableB = GL_FALSE;
			glGetQueryObjectiv(queries[0], GL_QUERY_RESULT_AVAILABLE, &availableA);
			glGetQueryObjectiv(queries[1], GL_QUERY_RESULT_AVAILABLE, &availableB);
			return availableA == GL_TRUE && availableB == GL_TRUE;
		}

		void ResolvePassMetrics(const std::string& passName, ModularRenderer::PassTimingMetrics& metrics) {
			auto it = passStates.find(passName);
			if (it == passStates.end() || !CanReadback()) {
				return;
			}

			PassQuerySlot& readbackSlot = it->second.slots[ReadbackSlotIndex()];

			if (readbackSlot.statsIssued) {
#ifdef GLEW_ARB_pipeline_statistics_query
				if (GLEW_ARB_pipeline_statistics_query) {
					const bool ready = strictTiming || AreQueryResultsReady(readbackSlot.statsQueries);
					if (ready) {
						auto waitStart = Clock::now();
						GLuint64 primitives = 0;
						GLuint64 computeInvocations = 0;
						glGetQueryObjectui64v(readbackSlot.statsQueries[0], GL_QUERY_RESULT, &primitives);
						glGetQueryObjectui64v(readbackSlot.statsQueries[1], GL_QUERY_RESULT, &computeInvocations);
						auto waitEnd = Clock::now();
						metrics.cpuWaitSyncMs += std::chrono::duration<float, std::milli>(waitEnd - waitStart).count();
						metrics.drawCalls = primitives > 0 ? 1 : 0;
						metrics.dispatchCount = computeInvocations > 0 ? 1 : 0;
						readbackSlot.statsIssued = false;
					}
				}
#endif
			}

			if (readbackSlot.timerIssued && GLEW_ARB_timer_query) {
				const bool ready = strictTiming || AreQueryResultsReady(readbackSlot.timestampQueries);
				if (ready) {
					auto waitStart = Clock::now();
					GLuint64 beginNs = 0;
					GLuint64 endNs = 0;
					glGetQueryObjectui64v(readbackSlot.timestampQueries[0], GL_QUERY_RESULT, &beginNs);
					glGetQueryObjectui64v(readbackSlot.timestampQueries[1], GL_QUERY_RESULT, &endNs);
					auto waitEnd = Clock::now();
					metrics.cpuWaitSyncMs += std::chrono::duration<float, std::milli>(waitEnd - waitStart).count();
					if (endNs >= beginNs) {
						metrics.gpuTimeMs = static_cast<float>(endNs - beginNs) / 1000000.0f;
					}
					readbackSlot.timerIssued = false;
				}
			}
		}

	private:
		static void InitializePassState(PassQueryState& state) {
			for (auto& slot : state.slots) {
				if (GLEW_ARB_timer_query) {
					glGenQueries(2, slot.timestampQueries.data());
				}
#ifdef GLEW_ARB_pipeline_statistics_query
				if (GLEW_ARB_pipeline_statistics_query) {
					glGenQueries(2, slot.statsQueries.data());
				}
#endif
			}
			state.initialized = true;
		}
	};

	struct ScopedPassProfiler {
		ModularRenderer::PassTimingMetrics metrics;
		Clock::time_point cpuStart;
		GpuProfilerPool& queryPool;
		PassQuerySlot* activeSlot = nullptr;

		ScopedPassProfiler(const std::string& name, GpuProfilerPool& profilerPool)
			: queryPool(profilerPool) {
			metrics.name = name;
			cpuStart = Clock::now();
			queryPool.ResolvePassMetrics(name, metrics);

			PassQueryState& passState = queryPool.GetOrCreatePassState(name);
			activeSlot = &passState.slots[queryPool.CurrentSlotIndex()];
			activeSlot->timerIssued = false;
			activeSlot->statsIssued = false;

			if (GLEW_ARB_timer_query) {
				glQueryCounter(activeSlot->timestampQueries[0], GL_TIMESTAMP);
			}

#ifdef GLEW_ARB_pipeline_statistics_query
			if (GLEW_ARB_pipeline_statistics_query) {
				glBeginQuery(GL_PRIMITIVES_SUBMITTED_ARB, activeSlot->statsQueries[0]);
				glBeginQuery(GL_COMPUTE_SHADER_INVOCATIONS_ARB, activeSlot->statsQueries[1]);
			}
#endif
		}

		void Finish() {
			const auto cpuEnd = Clock::now();
			metrics.cpuTimeMs = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();

			if (!activeSlot) {
				return;
			}

			if (GLEW_ARB_timer_query) {
				glQueryCounter(activeSlot->timestampQueries[1], GL_TIMESTAMP);
				activeSlot->timerIssued = true;
			}

#ifdef GLEW_ARB_pipeline_statistics_query
			if (GLEW_ARB_pipeline_statistics_query) {
				glEndQuery(GL_COMPUTE_SHADER_INVOCATIONS_ARB);
				glEndQuery(GL_PRIMITIVES_SUBMITTED_ARB);
				activeSlot->statsIssued = true;
			}
#endif
		}
	};
}




bool ModularRenderer::Initialize(int windowWidth, int windowHeight)
{
	m_context.width = windowWidth;
	m_context.height = windowHeight;

	// Initialize shared resources (FBOs, screen quad)
	if (!InitializeSharedResources()) {
		std::cerr << "[ModularRenderer] Failed to initialize shared resources.\n";
		return false;
	}

	// Create and initialize all passes
	m_shadowPass = std::make_unique<ShadowPass>();
	m_lpvPass = std::make_unique<LPVPass>(); //Create LPV pass
	m_gbufferPass = std::make_unique<GBufferPass>();
	m_rtPass = std::make_unique<RTPass>();  // Create path tracing pass
	m_ssaoPass = std::make_unique<SSAOPass>();
	m_screenSpaceShadowPass = std::make_unique<ScreenSpaceShadowPass>();
	m_ssgiPass = std::make_unique<SSGIPass>(); //Create SSGI pass
	m_lightingPass = std::make_unique<LightingPass>();
	m_bloomPass = std::make_unique<BloomPass>();
	m_taaPass = std::make_unique<TAAPass>();
	m_transparentPass = std::make_unique<TransparentForwardPass>();
	m_postProcessPass = std::make_unique<PostProcessPass>();
	m_guiPass = std::make_unique<GUIPass>();
	m_debugBBoxPass = std::make_unique<DebugBBoxPass>();  // Debug bounding box visualization

	bool success = true;
	success &= m_shadowPass->Initialize(m_context);
	success &= m_lpvPass->Initialize(m_context);
	success &= m_gbufferPass->Initialize(m_context);
	success &= m_rtPass->Initialize(m_context);  // Initialize path tracing pass
	success &= m_ssaoPass->Initialize(m_context);
	success &= m_screenSpaceShadowPass->Initialize(m_context);
	success &= m_ssgiPass->Initialize(m_context);
	success &= m_lightingPass->Initialize(m_context);
	success &= m_bloomPass->Initialize(m_context);
	success &= m_taaPass->Initialize(m_context);
	success &= m_transparentPass->Initialize(m_context);
	success &= m_postProcessPass->Initialize(m_context);
	success &= m_guiPass->Initialize(m_context);
	success &= m_debugBBoxPass->Initialize(m_context);  // Initialize debug bounding box pass

	if (!success) {
		std::cerr << "[ModularRenderer] Failed to initialize one or more passes.\n";
		return false;
	}

	std::cout << "[ModularRenderer] Initialized successfully with " << windowWidth
		<< "x" << windowHeight << " resolution.\n";
	return true;
}

bool ModularRenderer::InitializeSharedResources()
{
	// Enable seamless cubemap sampling for IBL
	// This must be enabled before any cubemap is created or sampled
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

	// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
	// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
	// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
	// RT3: R8UI - Material ID (0=Standard PBR, 1=SpecGloss, 2=Transmission, etc.)
	// RT4: RGBA16F - Emissive color (RGB) + unused (A)
	m_context.gbufferFBO = std::make_unique<FrameBuffer>(
		m_context.width, m_context.height,
		std::vector<GLenum>{
		GL_RGBA8,    // RT0: Oct normal + roughness/metallic
			GL_RGBA16F,  // RT1: Albedo + occlusion
			GL_RGBA16F,  // RT2: Specular F0 (full RGB) + emissive strength
			GL_R8UI,     // RT3: Material ID
			GL_RGBA16F   // RT4: Emissive color (RGB)
		},
		true,  // useDepthAsTexture
		false  // useDepthAsTextureArray
	);

	if (!m_context.gbufferFBO->IsComplete()) {
		std::cerr << "[ModularRenderer] G-buffer FBO not complete!\n";
		return false;
	}

	// Create HDR FBO
	m_context.hdrFBO = std::make_unique<FrameBuffer>(
		m_context.width, m_context.height,
		std::vector<GLenum>{ GL_RGB16F },
		false, false, 1, false, GL_DEPTH24_STENCIL8
	);

	if (!m_context.hdrFBO->IsComplete()) {
		std::cerr << "[ModularRenderer] HDR FBO not complete!\n";
		return false;
	}

	// Create screen quad
	m_context.screenQuad = std::make_unique<ScreenQuad>();

	std::cout << "[ModularRenderer] Shared resources initialized.\n";
	return true;
}

void ModularRenderer::Resize(int newWidth, int newHeight)
{
	if (newWidth <= 0 || newHeight <= 0) {
		std::cerr << "[ModularRenderer] ERROR: Invalid dimensions " << newWidth << "x" << newHeight << "\n";
		return;
	}

	m_context.width = newWidth;
	m_context.height = newHeight;

	// Resize shared FBOs
	m_context.gbufferFBO->Resize(newWidth, newHeight);
	m_context.hdrFBO->Resize(newWidth, newHeight);

	// Resize all passes
	if (m_shadowPass) m_shadowPass->Resize(m_context, newWidth, newHeight);
	if (m_lpvPass) m_lpvPass->Resize(m_context, newWidth, newHeight);
	if (m_gbufferPass) m_gbufferPass->Resize(m_context, newWidth, newHeight);
	if (m_rtPass) m_rtPass->Resize(m_context, newWidth, newHeight);  // Resize path tracing pass
	if (m_ssaoPass) m_ssaoPass->Resize(m_context, newWidth, newHeight);
	if (m_screenSpaceShadowPass) m_screenSpaceShadowPass->Resize(m_context, newWidth, newHeight);
	if (m_ssgiPass) m_ssgiPass->Resize(m_context, newWidth, newHeight);
	if (m_lightingPass) m_lightingPass->Resize(m_context, newWidth, newHeight);
	if (m_bloomPass) m_bloomPass->Resize(m_context, newWidth, newHeight);
	if (m_taaPass) m_taaPass->Resize(m_context, newWidth, newHeight);
	if (m_transparentPass) m_transparentPass->Resize(m_context, newWidth, newHeight);
	if (m_postProcessPass) m_postProcessPass->Resize(m_context, newWidth, newHeight);
	if (m_guiPass) m_guiPass->Resize(m_context, newWidth, newHeight);
	if (m_debugBBoxPass) m_debugBBoxPass->Resize(m_context, newWidth, newHeight);

	std::cout << "[ModularRenderer] Resized to " << newWidth << "x" << newHeight << "\n";
}

void ModularRenderer::UpdateContext(const std::shared_ptr<Camera>& camera,
	float exposure, float gamma,
	bool enableShadows, float shadowBias,
	glm::vec3 envColor)
{
	// Update per-frame parameters (from MainWindow function call)
	m_context.exposure = exposure;
	m_context.gamma = gamma;
	m_context.enableShadows = enableShadows;
	m_context.shadowBias = shadowBias;
	m_context.envColor = envColor;


	// Compute view/projection matrices
	float aspect = static_cast<float>(m_context.width) / static_cast<float>(m_context.height);

	// Store previous matrices for TAA
	m_context.prevView = m_context.view;
	m_context.prevProj = m_context.proj;

	// Get current matrices
	auto [view, proj] = camera->GetUpdatedViewProjectionMatrix(
		aspect,
		camera->GetCameraNearPlane(),
		camera->GetCameraFarPlane()
	);

	m_context.view = view;
	m_context.proj = proj;

	// Apply TAA jitter if enabled
	if (m_context.enableTAA && m_taaPass) {
		glm::vec2 jitter = m_taaPass->GetCurrentJitter();
		// Scale jitter to projection space
		jitter *= (1.0f / glm::vec2(m_context.width, m_context.height));
		m_context.proj[2][0] += jitter.x;
		m_context.proj[2][1] += jitter.y;
	}
}


void ModularRenderer::CheckGLError(const std::string& passName)
{
	if constexpr (!DebugErrorChecking) return;

	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		std::cerr << "[ModularRenderer] ERROR after " << passName << ": 0x"
			<< std::hex << error << std::dec;

		// Decode common error codes
		switch (error) {
		case GL_INVALID_ENUM:
			std::cerr << " (GL_INVALID_ENUM)";
			break;
		case GL_INVALID_VALUE:
			std::cerr << " (GL_INVALID_VALUE)";
			break;
		case GL_INVALID_OPERATION:
			std::cerr << " (GL_INVALID_OPERATION)";
			break;
		case GL_INVALID_FRAMEBUFFER_OPERATION:
			std::cerr << " (GL_INVALID_FRAMEBUFFER_OPERATION)";
			break;
		case GL_OUT_OF_MEMORY:
			std::cerr << " (GL_OUT_OF_MEMORY)";
			break;
		default:
			std::cerr << " (Unknown error)";
			break;
		}
		std::cerr << std::endl;
	}
}

void ModularRenderer::Render(const std::shared_ptr<SceneGraph>& sceneGraph,
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
	int windowHeight)
{
	m_lastPassMetrics.clear();
	m_lastCpuWaitSyncMs = 0.0f;
	static GpuProfilerPool profilerPool;
	profilerPool.BeginFrame();
	// Clear any stale GL errors from previous frames (only in debug mode)
	if constexpr (DebugErrorChecking) {
		while (glGetError() != GL_NO_ERROR) {}
	}

	if (!sceneGraph || !camera) {
		std::cerr << "[ModularRenderer] ERROR: Missing scene graph or camera!\n";
		return;
	}

	// Use scene-owned light manager (required by initialization flow)
	m_context.lightManager = sceneGraph->GetLightManager();
	if (!m_context.lightManager) {
		std::cerr << "[ModularRenderer] ERROR: SceneGraph has no LightManager!\n";
		return;
	}

	// Deterministic per-frame light update location
	m_context.lightManager->UpdateLights(0.016f);
	m_context.lightManager->UpdateGPUBuffers();

	// Update context with current frame parameters
	UpdateContext(camera, exposure, gamma, enableShadows, shadowBias, envColor);

	// Get skybox from scene
	auto skybox = sceneGraph->GetSkybox();

	// Sync IBL intensity settings from context to skybox
	if (skybox && skybox->IsReady()) {
		skybox->SetIBLIntensity(m_context.iblIntensity);
		skybox->SetSkyboxExposure(m_context.skyboxExposure);
		skybox->SetDiffuseIBLScale(m_context.diffuseIBLScale);
		skybox->SetSpecularIBLScale(m_context.specularIBLScale);
	}

	auto profilePass = [this, &profilerPool](const char* name, const std::function<void()>& executePass) {
		ScopedPassProfiler profiler(name, profilerPool);
		executePass();
		profiler.Finish();
		m_lastCpuWaitSyncMs += profiler.metrics.cpuWaitSyncMs;
		m_lastPassMetrics.push_back(profiler.metrics);
	};

	// Decide renderer mode once near the top of the frame to keep branch-specific
	// pass dependencies explicit and prevent accidental cross-mode regressions.
	const RenderContext::RendererMode rendererMode = m_context.rendererMode;

	// PATH-TRACED MODE DEPENDENCIES:
	// - Must run RTPass first to produce HDR scene color.
	// - Bloom/PostProcess/GUI consume the composited color chain from RTPass.
	// - Deferred-only passes (shadow map, G-buffer, SSAO/SSGI/TAA, lighting, transparent)
	//   are intentionally skipped in this mode.
	if (rendererMode == RenderContext::RendererMode::PATH_TRACED) {
		if constexpr (VerboseLogging) {
			std::cout << "[ModularRenderer] PATH TRACING MODE - Executing RTPass" << std::endl;
		}

		// 1) Primary path-traced output
		profilePass("RTPass", [&]() { m_rtPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("RTPass");

		// 2) Optional bloom over path-traced HDR color
		GLuint bloomTex = 0;
		if (m_context.enableBloom) {
			profilePass("BloomPass", [&]() { m_bloomPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
			CheckGLError("BloomPass");
			bloomTex = m_bloomPass->GetBloomResult();
		}
		m_postProcessPass->SetBloomTexture(bloomTex);

		// Ensure post-process/gui draw to the backbuffer in this branch as well.
		FrameBuffer::Unbind();

		// 3) Tonemapping/post-effects
		profilePass("PostProcessPass", [&]() { m_postProcessPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("PostProcessPass");

		// 4) GUI overlay
		profilePass("GUIPass", [&]() { m_guiPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("GUIPass");

		return;
	}

	// DEFERRED MODE DEPENDENCIES:
	// ShadowPass -> GBufferPass -> (optional) SSAO/SSS/TAA/SSGI/LPV -> LightingPass.
	// Lighting output is then combined with skybox + transparency and fed into
	// Bloom/PostProcess/GUI. Debug G-buffer visualization is valid only after
	// GBufferPass and only in deferred mode.
	profilePass("ShadowPass", [&]() { m_shadowPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
	CheckGLError("ShadowPass");

	profilePass("GBufferPass", [&]() { m_gbufferPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
	CheckGLError("GBufferPass");

	// DEBUG MODE: Show G-buffer visualizations
	if (m_context.debugMode != RenderContext::DebugMode::NONE) {
		if constexpr (VerboseLogging) {
			std::cout << "[ModularRenderer] DEBUG MODE - Visualizing G-buffer" << std::endl;
		}
		visualizeDebugMode(m_context);

		// Render GUI overlay
		profilePass("GUIPass", [&]() { m_guiPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("GUIPass");

		return; // Early exit - skip rest of deferred pipeline for debug view
	}

	// LPV Global Illumination Pass (AFTER G-buffer, so geometry is available for RSM)
	if (m_context.enableLPV) {
		if (m_lpvPass) {
			m_lpvPass->config.enableLPV = m_context.enableLPV;
			m_lpvPass->config.gridResolution = m_context.lpvGridResolution;
			m_lpvPass->config.voxelSize = m_context.lpvVoxelSize;
			m_lpvPass->config.rsmResolution = m_context.lpvRSMResolution;
			m_lpvPass->config.vplSampleCount = m_context.lpvVPLSampleCount;
			m_lpvPass->config.propagationIterations = m_context.lpvPropagationIterations;
			m_lpvPass->config.propagationAttenuation = m_context.lpvPropagationAttenuation;
			m_lpvPass->config.propagationBias = m_context.lpvPropagationBias;
			m_lpvPass->config.enableOcclusion = m_context.lpvEnableOcclusion;
			m_lpvPass->config.giStrength = m_context.lpvGIStrength;
			m_lpvPass->config.updateFrequency = m_context.lpvUpdateFrequency;

			profilePass("LPVPass", [&]() { m_lpvPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
			CheckGLError("LPVPass");
		}
	}

	// Only execute SSAO if enabled
	GLuint ssaoTex = 0;
	if (m_context.enableSSAO) {
		profilePass("SSAOPass", [&]() { m_ssaoPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("SSAOPass");
		ssaoTex = m_ssaoPass->GetSSAOTexture();
	}

	// Screen-space shadows (contact shadows) if enabled
	GLuint sssTex = 0;
	if (m_context.enableScreenSpaceShadows) {
		profilePass("ScreenSpaceShadowPass", [&]() { m_screenSpaceShadowPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("ScreenSpaceShadowPass");
		sssTex = m_screenSpaceShadowPass->GetShadowTexture();
	}

	// TAA Pass (velocity + resolve) - executes before lighting
	if (m_context.enableTAA) {
		profilePass("TAAPass", [&]() { m_taaPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("TAAPass");
	}

	// SSGI Pass - Screen Space Global Illumination (before lighting)
	if (m_context.enableSSGI && m_ssgiPass) {
		profilePass("SSGIPass", [&]() { m_ssgiPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("SSGIPass");
	}

	// Provide SSAO texture to lighting pass (or 0 if disabled)
	m_lightingPass->SetSSAOTexture(ssaoTex);
	// Provide screen-space shadow texture to lighting pass (or 0 if disabled)
	m_lightingPass->SetScreenSpaceShadowTexture(sssTex);

	// Provide SSGI texture to lighting pass (or 0 if disabled)
	if (m_context.enableSSGI && m_ssgiPass) {
		m_lightingPass->SetSSGITexture(m_ssgiPass->GetSSGITexture());
	}
	else {
		m_lightingPass->SetSSGITexture(0);
	}

	// Provide LPV textures to lighting pass (or 0 if disabled)
	if (m_context.enableLPV && m_lpvPass) {
		GLuint lpvR = m_lpvPass->GetLPVTextureR();
		GLuint lpvG = m_lpvPass->GetLPVTextureG();
		GLuint lpvB = m_lpvPass->GetLPVTextureB();
		m_lightingPass->SetLPVTextures(lpvR, lpvG, lpvB);
	}
	else {
		m_lightingPass->SetLPVTextures(0, 0, 0);
	}

	profilePass("LightingPass", [&]() { m_lightingPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
	CheckGLError("LightingPass");

	// Skybox rendering (background into HDR)
	if (skybox && skybox->IsReady()) {
		m_context.hdrFBO->Bind();
		skybox->Draw(m_context.view, m_context.proj);
		CheckGLError("Skybox");
	}

	// Transparent forward rendering
	profilePass("TransparentForwardPass", [&]() { m_transparentPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
	CheckGLError("TransparentForwardPass");

	// Unbinding HDR FBO after both skybox and transparent rendering
	FrameBuffer::Unbind();

	// Only execute Bloom if enabled
	GLuint bloomTex = 0;
	if (m_context.enableBloom) {
		profilePass("BloomPass", [&]() { m_bloomPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("BloomPass");
		bloomTex = m_bloomPass->GetBloomResult();
	}

	// Provide bloom texture to post-process (or 0 if disabled)
	m_postProcessPass->SetBloomTexture(bloomTex);
	profilePass("PostProcessPass", [&]() { m_postProcessPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
	CheckGLError("PostProcessPass");

	// Debug bounding box visualization (after post-process, renders overlay)
	if (m_context.showBoundingBoxes) {
		profilePass("DebugBBoxPass", [&]() { m_debugBBoxPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
		CheckGLError("DebugBBoxPass");
	}

	// Render internal GUI elements to backbuffer
	profilePass("GUIPass", [&]() { m_guiPass->Execute(m_context, sceneGraph, camera, lighting, skybox); });
	CheckGLError("GUIPass");

	// Capture color history for SSGI
	if (m_ssgiPass) {
		m_ssgiPass->CaptureHistory(m_context);
	}
}

void ModularRenderer::visualizeDebugMode(RenderContext& ctx)
{
	// Output debug visualization to backbuffer
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, ctx.width, ctx.height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (!ctx.gbufferFBO || !ctx.screenQuad) {
		std::cerr << "[ModularRenderer] Missing G-buffer or screen quad for debug viz" << std::endl;
		// Restore state before returning
		glEnable(GL_DEPTH_TEST);
		return;
	}

	// Simple fullscreen quad shader for visualization
	// For now, use glBlitFramebuffer as a quick solution
	GLuint sourceAttachment = 0;

	switch (ctx.debugMode) {
	case RenderContext::DebugMode::ALBEDO:
		sourceAttachment = 1; // Albedo is in RT1
		break;
	case RenderContext::DebugMode::NORMAL:
		sourceAttachment = 0; // Normal is in RT0
		break;
	case RenderContext::DebugMode::MATERIAL_ID:
		sourceAttachment = 3; // Material ID is in RT3
		break;
	case RenderContext::DebugMode::DEPTH:
		// Use depth buffer
		glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.gbufferFBO->GetFBO());
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(
			0, 0, ctx.width, ctx.height,
			0, 0, ctx.width, ctx.height,
			GL_DEPTH_BUFFER_BIT,
			GL_NEAREST
		);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		std::cout << "[ModularRenderer] Visualized depth buffer" << std::endl;
		// Restore state before returning
		glEnable(GL_DEPTH_TEST);
		return;
	case RenderContext::DebugMode::SHADOW_MAPS:
	case RenderContext::DebugMode::MOTION_VECTORS:
		// TODO: Implement these visualizations
		std::cout << "[ModularRenderer] Debug mode not yet implemented" << std::endl;
		// Restore state before returning
		glEnable(GL_DEPTH_TEST);
		return;
	default:
		// Restore state before returning
		glEnable(GL_DEPTH_TEST);
		return;
	}

	// Blit color attachment to backbuffer
	glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.gbufferFBO->GetFBO());
	glReadBuffer(GL_COLOR_ATTACHMENT0 + sourceAttachment);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	glBlitFramebuffer(
		0, 0, ctx.width, ctx.height,
		0, 0, ctx.width, ctx.height,
		GL_COLOR_BUFFER_BIT,
		GL_NEAREST
	);

	// Restore framebuffer state
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glReadBuffer(GL_BACK);

	// Restore render state
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);

	std::cout << "[ModularRenderer] Visualized debug mode: " << static_cast<int>(ctx.debugMode) << std::endl;
}


void ModularRenderer::ResetTAA()
{
	if (m_taaPass) {
		m_taaPass->ResetHistory();
	}
}

int ModularRenderer::GetTAAFrameIndex() const
{
	return m_taaPass ? m_taaPass->GetFrameIndex() : 0;
}
