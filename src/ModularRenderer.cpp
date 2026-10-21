#include "ModularRenderer.h"
#include "FrameProfiler.h"
#include "SceneFrameClock.h"
#include "PresentTarget.h"
#include "FrameBuffer.h"
#include "ScreenQuad.h"
#include "ShaderLoader.h"
#include "SceneGraph.h"
#include "RenderSystem.h"
#include "Camera.h"
#include "DirectionalLight.h"
#include "Skybox.h"
#include "LightManager.h"
#include "RTSceneResources.h"

#include "passes/GBufferPass.h"
#include "passes/TransformHistoryPass.h"
#include "passes/ShadowPass.h"
#include "passes/LPVPass.h"
#include "passes/RTPass.h"  // Path tracing pass
#include "passes/HBAOPass.h"
#include "passes/ScreenSpaceShadowPass.h"
#include "passes/IndirectDiffusePass.h"
#include "passes/SurfelGIManager.h"

#include "passes/LightingPass.h"
#include "passes/BloomPass.h"
#include "passes/TAAPass.h"
#include "passes/TransparentForwardPass.h"
#include "passes/PostProcessPass.h"
#include "passes/GUIPass.h"  // Internal GUI rendering
#include "passes/DebugBBoxPass.h"  // Debug bounding box visualization
#include "passes/SelectionOutlinePass.h"  // Editor selection outline
#include "passes/LightClusterPass.h"
#include "passes/ExposurePass.h"
#include "passes/SSRPass.h"
#include "passes/ReflectionProbePass.h"
#include "passes/VolumetricFogPass.h"
#include "passes/MotionBlurPass.h"
#include "passes/RainPass.h"
#include "passes/VehicleFXPass.h"
#include "passes/ParticlePass.h"
#include "passes/DecalPass.h"
#include "passes/LensFlarePass.h"
#include "VehicleFX.h"
#include <iostream>
#include <chrono>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <cstdlib>
#include <queue>
#include <algorithm>

ModularRenderer::ModularRenderer()
{
}

ModularRenderer::~ModularRenderer()
{
	if (m_debugViewShader) {
		rhi::Gpu().DestroyProgram(m_debugViewShader);
		m_debugViewShader = rhi::ProgramHandle{};
	}
	if (m_indirectDiffuseDebugPresentShader) {
		rhi::Gpu().DestroyProgram(m_indirectDiffuseDebugPresentShader);
		m_indirectDiffuseDebugPresentShader = rhi::ProgramHandle{};
	}
}

const SurfelGIFrameStats* ModularRenderer::GetSurfelGIStats() const
{
	return m_surfelGIManager ? &m_surfelGIManager->GetStats() : nullptr;
}

bool ModularRenderer::ReloadSurfelGIShaders()
{
	if (!m_surfelGIManager || !m_surfelGIManager->IsEnabled()) return true;   // nothing is compiled: nothing to reload
	return m_surfelGIManager->ReloadShaders();
}

namespace {
	using Clock = std::chrono::high_resolution_clock;
	static constexpr std::size_t kProfilerFramesInFlight = 4;
	static constexpr std::size_t kProfilerReadbackDelay = kProfilerFramesInFlight - 1;

	static bool IsEnvValueEnabled(const char* value) {
		if (value == nullptr || value[0] == '\0') {
			return false;
		}

		const std::string text(value);
		return !(text == "0" || text == "false" || text == "FALSE" ||
			text == "False" || text == "no" || text == "NO" || text == "No" ||
			text == "off" || text == "OFF" || text == "Off");
	}

	static bool IsEnvVarEnabled(const char* name) {
#if defined(_MSC_VER)
		char* value = nullptr;
		size_t length = 0;
		if (_dupenv_s(&value, &length, name) != 0) {
			return false;
		}
		const bool enabled = IsEnvValueEnabled(value);
		std::free(value);
		return enabled;
#else
		return IsEnvValueEnabled(std::getenv(name));
#endif
	}

	static std::string GetEnvVarString(const char* name) {
#if defined(_MSC_VER)
		char* value = nullptr;
		size_t length = 0;
		if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
			return {};
		}
		std::string result(value);
		std::free(value);
		return result;
#else
		const char* value = std::getenv(name);
		return value ? std::string(value) : std::string{};
#endif
	}

	static float GetEnvVarFloat(const char* name, float fallback) {
		const std::string value = GetEnvVarString(name);
		if (value.empty()) {
			return fallback;
		}
		char* end = nullptr;
		const float parsed = std::strtof(value.c_str(), &end);
		return end != value.c_str() ? parsed : fallback;
	}
	static int GetEnvVarInt(const char* name, int fallback) {
		const std::string value = GetEnvVarString(name);
		if (value.empty()) {
			return fallback;
		}
		char* end = nullptr;
		const long parsed = std::strtol(value.c_str(), &end, 10);
		return end != value.c_str() ? static_cast<int>(parsed) : fallback;
	}

	struct SurfelGIBudgetCaps {
		uint32_t maxSurfels = 131072u;
		uint32_t maxRayBudget = 32768u;
		uint32_t minSpawnTileSize = 32u;
		uint32_t maxSurfelsPerCell = 64u;
		uint32_t maxGatherSurfelsPerPixel = 512u;
		uint32_t gatherNeighborRadius = 0u;
		float finalGatherResolutionScale = 1.0f;
		float fallbackStrength = 0.20f;
		bool allowRayGuiding = false;
		bool allowRadialDepth = false;
		bool allowScreenTrace = false;
		bool allowSoftwareBVHTrace = false;
		float maxIndirectIntensity = 1.0f;
		uint32_t minRayUpdateInterval = 2u;
		uint32_t maxStationaryFastFillFrames = 12u;
		uint32_t maxSpawnsPerFrame = 96u;
		uint32_t maxProjectedSurfelsPerFrame = 16384u;
		uint32_t maxRecycleCountPerFrame = 2048u;
		// Spawn budget ceiling = visible tiles / spawnBudgetTileDivisor, bounded by maxSpawnBudget (spawn.comp).
		float spawnBudgetTileDivisor = 16.0f;
		uint32_t maxSpawnBudget = 512u;
		uint32_t populationCap = 2048u;
	};

	static SurfelGIBudgetCaps GetSurfelGIBudgetCaps(SurfelGIQualityTier tier) {
		switch (tier) {
		case SurfelGIQualityTier::Low:
			// The software BVH trace is on: without it the rays cannot reach the scene geometry, and Low measured about 6%
			// of the reference (plan section 20.4). Screen trace stays off.
			return { 24576u, 1024u, 16u, 48u, 16u, 1u, 0.50f, 1.0f, true, true, false, true, 1.0f, 2u, 8u, 65536u, 24576u, 2048u, 64.0f, 128u, 1024u };
		case SurfelGIQualityTier::High:
			return { 131072u, 4096u, 16u, 96u, 48u, 1u, 0.50f, 1.0f, true, true, true, true, 1.0f, 1u, 48u, 65536u, 131072u, 8192u, 8.0f, 1024u, 4096u };
		case SurfelGIQualityTier::Ultra:
			return { 262144u, 8192u, 16u, 128u, 64u, 1u, 0.50f, 1.0f, true, true, true, true, 1.0f, 1u, 64u, 65536u, 262144u, 16384u, 8.0f, 1024u, 8192u };
		case SurfelGIQualityTier::Medium:
		default:
			// Software BVH on (without it rays the screen and surfel-grid traces cannot resolve lose
			// their light). The ray budget is a value (the default for the tier), not a cap: the
			// per-frame ray budget is shared out by demand (allocate_rays.comp). Medium is 2048.
			return { 65536u, 2048u, 16u, 64u, 32u, 1u, 0.50f, 1.0f, true, true, true, true, 1.0f, 1u, 20u, 64u, 24576u, 256u, 16.0f, 512u, 2048u };
		}
	}

	// Path, cache and emitter-sampling presets of each quality tier (plan section 20). The ray budget, gather and population
	// caps are in GetSurfelGIBudgetCaps above. Each value is the default of its NOX_SURFEL_GI_* variable, which overrides it
	// inside the validation block (SyncSurfelGISettings).
	struct SurfelGIPathPreset {
		bool worldSurfels;            // world mode: cache-first, misses queue world surfels (plan section 17)
		uint32_t pathMaxHits;         // depth cap of the continuation past the first hit
		uint32_t sunShadowBVHDepth;   // vertices below this use the BVH sun fallback
		uint32_t cacheDepth;          // vertices below this read the cache
		float cacheMinConfidence;     // hybrid continuation: a cache answer below this confidence is not used
		uint32_t cacheCandidates;     // irradiance lookup budget per hit: surfels visited, and per neighbour cell
		uint32_t cachePerCellBudget;
		uint32_t cacheAlbedoCandidates; // cached-albedo lookup budget (neutral BVH hits only)
		uint32_t cacheAlbedoPerCell;
		float emitterNEEMaxDistance;  // metres, 0 = no cut (plan section 20)
	};

	static SurfelGIPathPreset GetSurfelGIPathPreset(SurfelGIQualityTier tier) {
		switch (tier) {
		case SurfelGIQualityTier::Low:
			// Cache-first, no continuation: a hit is answered by the cache or by its own direct light and NEE only.
			return { true, 1u, 1u, 1u, 0.0f, 16u, 1u, 4u, 1u, 40.0f };
		case SurfelGIQualityTier::High:
			// Cache-first world mode, deeper continuation on a cache miss, larger lookup budgets.
			return { true, 4u, 2u, 3u, 0.0f, 32u, 2u, 8u, 1u, 40.0f };
		case SurfelGIQualityTier::Ultra:
			// Full path continuation with the cache at the first two vertices (the merged-base path, reference quality).
			return { false, 32u, 32u, 2u, 0.0f, 128u, 4u, 32u, 2u, 0.0f };
		case SurfelGIQualityTier::Medium:
		default:
			// Cache-first world mode: a cache miss continues the path to depth 3 with roulette. Measured: the hybrid
			// (continuing only at low-confidence cache answers, cacheMinConfidence 0.3) lowered energy at both depth 2 and 3.
			return { true, 3u, 1u, 3u, 0.0f, 16u, 1u, 4u, 1u, 40.0f };
		}
	}

	static void ApplySurfelGIRealtimeBudget(SurfelGISettings& settings, bool captureMode) {
		const SurfelGIBudgetCaps caps = GetSurfelGIBudgetCaps(settings.qualityTier);
		if (!captureMode) {
			settings.maxSurfels = std::min(settings.maxSurfels, caps.maxSurfels);
			settings.spawnTileSize = std::max(settings.spawnTileSize, caps.minSpawnTileSize);
			settings.maxSurfelsPerCell = std::min(settings.maxSurfelsPerCell, caps.maxSurfelsPerCell);
			settings.maxGatherSurfelsPerPixel = std::min(settings.maxGatherSurfelsPerPixel, caps.maxGatherSurfelsPerPixel);
			settings.useRayGuiding = settings.useRayGuiding && caps.allowRayGuiding;
			settings.useRadialDepth = settings.useRadialDepth && caps.allowRadialDepth;
			settings.useScreenSpaceTrace =
				(settings.useScreenSpaceTrace || settings.qualityTier == SurfelGIQualityTier::Ultra) &&
				caps.allowScreenTrace;
			settings.useSoftwareBVHTrace = settings.useSoftwareBVHTrace && caps.allowSoftwareBVHTrace;
			settings.indirectIntensity = std::min(settings.indirectIntensity, caps.maxIndirectIntensity);
			settings.cellAverageFallbackStrength = caps.fallbackStrength;
			settings.rayUpdateInterval = std::max(settings.rayUpdateInterval, caps.minRayUpdateInterval);
			settings.stationaryFastFillFrames = std::min(settings.stationaryFastFillFrames, caps.maxStationaryFastFillFrames);
			settings.maxSpawnsPerFrame = std::min(settings.maxSpawnsPerFrame, caps.maxSpawnsPerFrame);
			settings.maxProjectedSurfelsPerFrame = std::min(settings.maxProjectedSurfelsPerFrame, caps.maxProjectedSurfelsPerFrame);
			settings.maxRecycleCountPerFrame = std::min(settings.maxRecycleCountPerFrame, caps.maxRecycleCountPerFrame);
		}

		settings.spawnBudgetTileDivisor = caps.spawnBudgetTileDivisor;
		settings.maxSpawnBudgetPerFrame = caps.maxSpawnBudget;
		settings.spawnPopulationCap = caps.populationCap;
		settings.gatherNeighborRadius = captureMode ? std::max(caps.gatherNeighborRadius, 2u) : caps.gatherNeighborRadius;
		settings.finalGatherResolutionScale = captureMode ? 1.0f : std::clamp(caps.finalGatherResolutionScale, 0.25f, 1.0f);
		settings.cellAverageFallbackStrength = captureMode ? std::min(settings.cellAverageFallbackStrength, caps.fallbackStrength) : settings.cellAverageFallbackStrength;
	}

	struct PassQuerySlot {
		std::array<rhi::QueryHandle, 2> timestampQueries{};
		std::array<rhi::QueryHandle, 2> statsQueries{};
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
			strictTiming = IsEnvVarEnabled("NOX_STRICT_GPU_TIMING");
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

		static bool AreQueryResultsReady(const std::array<rhi::QueryHandle, 2>& queries) {
			const bool availableA = rhi::Gpu().IsQueryResultAvailable(queries[0]);
			const bool availableB = rhi::Gpu().IsQueryResultAvailable(queries[1]);
			return availableA && availableB;
		}

		void ResolvePassMetrics(const std::string& passName, ModularRenderer::PassTimingMetrics& metrics) {
			auto it = passStates.find(passName);
			if (it == passStates.end() || !CanReadback()) {
				return;
			}

			PassQuerySlot& readbackSlot = it->second.slots[ReadbackSlotIndex()];

			if (readbackSlot.statsIssued) {
				if (rhi::Gpu().Limits().pipelineStatisticsQueries) {
					const bool ready = strictTiming || AreQueryResultsReady(readbackSlot.statsQueries);
					if (ready) {
						auto waitStart = Clock::now();
						uint64_t primitives = rhi::Gpu().GetQueryResult(readbackSlot.statsQueries[0]);
						uint64_t computeInvocations = rhi::Gpu().GetQueryResult(readbackSlot.statsQueries[1]);
						auto waitEnd = Clock::now();
						metrics.cpuWaitSyncMs += std::chrono::duration<float, std::milli>(waitEnd - waitStart).count();
						metrics.drawCalls = primitives > 0 ? 1 : 0;
						metrics.dispatchCount = computeInvocations > 0 ? 1 : 0;
						readbackSlot.statsIssued = false;
					}
				}
			}

			if (readbackSlot.timerIssued) {
				const bool ready = strictTiming || AreQueryResultsReady(readbackSlot.timestampQueries);
				if (ready) {
					auto waitStart = Clock::now();
					uint64_t beginNs = rhi::Gpu().GetQueryResult(readbackSlot.timestampQueries[0]);
					uint64_t endNs = rhi::Gpu().GetQueryResult(readbackSlot.timestampQueries[1]);
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
				// Timer queries are core in the OpenGL 4.6 backend.
				for (auto& query : slot.timestampQueries) {
					query = rhi::Gpu().CreateQuery(rhi::QueryType::Timestamp);
				}
				if (rhi::Gpu().Limits().pipelineStatisticsQueries) {
					slot.statsQueries[0] = rhi::Gpu().CreateQuery(rhi::QueryType::PrimitivesSubmitted);
					slot.statsQueries[1] = rhi::Gpu().CreateQuery(rhi::QueryType::ComputeInvocations);
				}
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

			rhi::Gpu().WriteTimestamp(activeSlot->timestampQueries[0]);

			if (rhi::Gpu().Limits().pipelineStatisticsQueries) {
				rhi::Gpu().BeginQuery(activeSlot->statsQueries[0]);
				rhi::Gpu().BeginQuery(activeSlot->statsQueries[1]);
			}
		}

		void Finish() {
			const auto cpuEnd = Clock::now();
			metrics.cpuTimeMs = std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();

			if (!activeSlot) {
				return;
			}

			rhi::Gpu().WriteTimestamp(activeSlot->timestampQueries[1]);
			activeSlot->timerIssued = true;

			if (rhi::Gpu().Limits().pipelineStatisticsQueries) {
				rhi::Gpu().EndQuery(activeSlot->statsQueries[1]);
				rhi::Gpu().EndQuery(activeSlot->statsQueries[0]);
				activeSlot->statsIssued = true;
			}
		}
	};

	namespace ResourceNames {
		static const std::string SSAO = "SSAO";
		static const std::string ScreenSpaceShadow = "ScreenSpaceShadow";
		static const std::string BounceableRadiance = "BounceableRadiance";
		static const std::string IndirectDiffuse = "IndirectDiffuse";
		static const std::string IndirectDiffuseDebug = "IndirectDiffuseDebug";
		static const std::string SurfelGIIndirect = "SurfelGIIndirect";
		static const std::string Bloom = "Bloom";
		static const std::string LPVIndirect = "LPVIndirect";
	}

}




bool ModularRenderer::Initialize(int windowWidth, int windowHeight)
{
	m_context.width = windowWidth;
	m_context.height = windowHeight;
	m_context.rtSceneResources = std::make_shared<RTSceneResources>();

	// Initialize shared resources (FBOs, screen quad)
	if (!InitializeSharedResources()) {
		std::cerr << "[ModularRenderer] Failed to initialize shared resources.\n";
		return false;
	}

	// Create and initialize all passes
	m_shadowPass = std::make_unique<ShadowPass>();
	m_lpvPass = std::make_unique<LPVPass>(); //Create LPV pass
	m_gbufferPass = std::make_unique<GBufferPass>();
	m_transformHistoryPass = std::make_unique<TransformHistoryPass>();
	m_rtPass = std::make_unique<RTPass>();  // Create path tracing pass
	m_hbaoPass = std::make_unique<HBAOPass>();
	m_screenSpaceShadowPass = std::make_unique<ScreenSpaceShadowPass>();
	m_indirectDiffusePass = std::make_unique<IndirectDiffusePass>();
	m_surfelGIManager = std::make_unique<SurfelGIManager>();
	m_lightingPass = std::make_unique<LightingPass>();
	m_bloomPass = std::make_unique<BloomPass>();
	m_taaPass = std::make_unique<TAAPass>();
	m_transparentPass = std::make_unique<TransparentForwardPass>();
	m_postProcessPass = std::make_unique<PostProcessPass>();
	m_guiPass = std::make_unique<GUIPass>();
	m_debugBBoxPass = std::make_unique<DebugBBoxPass>();  // Debug bounding box visualization
	m_selectionOutlinePass = std::make_unique<SelectionOutlinePass>();
	m_lightClusterPass = std::make_unique<LightClusterPass>();
	m_exposurePass = std::make_unique<ExposurePass>();
	m_ssrPass = std::make_unique<SSRPass>();
	m_reflectionProbePass = std::make_unique<ReflectionProbePass>();
	m_volumetricFogPass = std::make_unique<VolumetricFogPass>();
	m_motionBlurPass = std::make_unique<MotionBlurPass>();
	m_rainPass = std::make_unique<RainPass>();
	m_vehicleFXPass = std::make_unique<VehicleFXPass>();
	m_particlePass = std::make_unique<ParticlePass>();
	m_decalPass = std::make_unique<DecalPass>();
	m_lensFlarePass = std::make_unique<LensFlarePass>();

	bool success = true;
	success &= m_shadowPass->Initialize(m_context);
	success &= m_lpvPass->Initialize(m_context);
	success &= m_gbufferPass->Initialize(m_context);
	success &= m_transformHistoryPass->Initialize(m_context);
	success &= m_rtPass->Initialize(m_context);  // Initialize path tracing pass
	success &= m_hbaoPass->Initialize(m_context);
	success &= m_screenSpaceShadowPass->Initialize(m_context);
	success &= m_indirectDiffusePass->Initialize(m_context);
	success &= m_surfelGIManager->Init(m_context, SurfelGISettings{});
	success &= m_lightingPass->Initialize(m_context);
	success &= m_bloomPass->Initialize(m_context);
	success &= m_taaPass->Initialize(m_context);
	success &= m_transparentPass->Initialize(m_context);
	success &= m_postProcessPass->Initialize(m_context);
	success &= m_guiPass->Initialize(m_context);
	success &= m_debugBBoxPass->Initialize(m_context);  // Initialize debug bounding box pass
	success &= m_selectionOutlinePass->Initialize(m_context);
	success &= m_lightClusterPass->Initialize(m_context);
	success &= m_exposurePass->Initialize(m_context);
	success &= m_ssrPass->Initialize(m_context);
	success &= m_reflectionProbePass->Initialize(m_context);
	success &= m_volumetricFogPass->Initialize(m_context);
	success &= m_motionBlurPass->Initialize(m_context);
	success &= m_rainPass->Initialize(m_context);
	m_rainPass->SetFogPass(m_volumetricFogPass.get());
	success &= m_vehicleFXPass->Initialize(m_context);
	m_vehicleFXPass->SetFogPass(m_volumetricFogPass.get());
	success &= m_particlePass->Initialize(m_context);
	success &= m_decalPass->Initialize(m_context);
	success &= m_lensFlarePass->Initialize(m_context);

	m_debugViewShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", "shaders/debug_view_frag.glsl");
	if (!m_debugViewShader) {
		std::cerr << "[ModularRenderer] Failed to create debug view shader.\n";
		return false;
	}
	m_indirectDiffuseDebugPresentShader = CreateShaderProgram(
		"shaders/fullscreen_vert.glsl",
		"shaders/indirect_diffuse_debug_present_frag.glsl");
	if (!m_indirectDiffuseDebugPresentShader) {
		std::cerr << "[ModularRenderer] Failed to create indirect diffuse debug present shader.\n";
		return false;
	}

	if (!success) {
		std::cerr << "[ModularRenderer] Failed to initialize one or more passes.\n";
		return false;
	}

	std::cout << "[ModularRenderer] Initialized successfully with " << windowWidth
		<< "x" << windowHeight << " resolution.\n";
	return true;
}

std::size_t ModularRenderer::PlanCacheKeyHash::operator()(const PlanCacheKey& key) const
{
	std::size_t seed = static_cast<std::size_t>(key.mode);
	const auto hashCombine = [&seed](bool value) {
		seed ^= static_cast<std::size_t>(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		};
	hashCombine(key.enableBloom);
	hashCombine(key.enableSSAO);
	hashCombine(key.enableIndirectDiffuse);
	hashCombine(key.enableSurfelGI);
	hashCombine(key.presentIndirectDiffuseDebug);
	hashCombine(key.enableScreenSpaceShadows);
	hashCombine(key.enableLPV);
	hashCombine(key.enableTAA);
	hashCombine(key.enableVolumetricFog);
	hashCombine(key.enableMotionBlur);
	hashCombine(key.enableRain);
	hashCombine(key.enableSelectionOutline);
	return seed;
}

ModularRenderer::FrameGraphMode ModularRenderer::DetermineFrameGraphMode() const
{
	if (m_context.rendererMode == RenderContext::RendererMode::PATH_TRACED) {
		return FrameGraphMode::PATH_TRACED;
	}
	switch (m_context.debugMode) {
	case RenderContext::DebugMode::ALBEDO:
	case RenderContext::DebugMode::NORMAL:
	case RenderContext::DebugMode::DEPTH:
	case RenderContext::DebugMode::SHADOW_MAPS:
	case RenderContext::DebugMode::MOTION_VECTORS:
	case RenderContext::DebugMode::MATERIAL_ID:
	case RenderContext::DebugMode::TRANSFORM_ID:
		return FrameGraphMode::DEFERRED_DEBUG;
	default:
		break;
	}
	return FrameGraphMode::DEFERRED;
}

ModularRenderer::PlanCacheKey ModularRenderer::BuildPlanCacheKey() const
{
	PlanCacheKey key;
	key.mode = DetermineFrameGraphMode();
	key.enableBloom = m_context.enableBloom;
	key.enableSSAO = m_context.enableSSAO;
	key.enableIndirectDiffuse = m_context.enableIndirectDiffuse;
	key.enableSurfelGI = m_surfelGIManager && m_surfelGIManager->GetSettings().enabled;
	key.presentIndirectDiffuseDebug =
		m_context.enableIndirectDiffuse &&
		m_context.indirectDiffuseDebugStage > 0 &&
		key.mode == FrameGraphMode::DEFERRED;
	key.enableScreenSpaceShadows = m_context.enableScreenSpaceShadows;
	key.enableLPV = m_context.enableLPV;
	key.enableTAA = m_context.enableTAA;
	key.enableVolumetricFog = m_context.enableVolumetricFog;
	key.enableMotionBlur = m_context.enableMotionBlur;
	key.enableRain = m_context.weatherRain > 0.0f;
	key.enableSelectionOutline = m_context.selectionOutline.enabled;
	return key;
}

void ModularRenderer::SyncSurfelGISettings()
{
	if (!m_surfelGIManager) {
		return;
	}

	SurfelGISettings settings = m_surfelGIManager->GetSettings();
	const bool forcedOnForValidation = IsEnvVarEnabled("NOX_ENABLE_SURFEL_GI");
	const bool forcedOffForValidation = IsEnvVarEnabled("NOX_DISABLE_SURFEL_GI");
	settings.enabled = !forcedOffForValidation && (m_context.enableSurfelGI || forcedOnForValidation);
	if (forcedOnForValidation) {
		m_context.enableSurfelGI = true;
	}
	if (forcedOffForValidation) {
		m_context.enableSurfelGI = false;
	}
	// NOX_GI_REFERENCE=1: independent path-traced diffuse GI replaces the surfel irradiance (reference_gi.comp).
	// It runs inside the surfel pipeline, and is off unless this variable is set.
	settings.referenceMode = IsEnvVarEnabled("NOX_GI_REFERENCE") && !forcedOffForValidation;
	if (settings.referenceMode) {
		settings.enabled = true;
		m_context.enableSurfelGI = true;
	}

	settings.qualityTier = static_cast<SurfelGIQualityTier>(
		std::clamp(m_context.surfelGIQualityTier, 0, 3));
	settings.qualityTier = static_cast<SurfelGIQualityTier>(
		std::clamp(GetEnvVarInt("NOX_SURFEL_GI_QUALITY_TIER", static_cast<int>(settings.qualityTier)), 0, 3));
	// The tier's path, cache and emitter presets apply in every run. The validation block below can override each value.
	{
		const SurfelGIPathPreset pathPreset = GetSurfelGIPathPreset(settings.qualityTier);
		settings.worldSurfels = pathPreset.worldSurfels;
		settings.pathMaxHits = pathPreset.pathMaxHits;
		settings.sunShadowBVHDepth = pathPreset.sunShadowBVHDepth;
		settings.cacheDepth = pathPreset.cacheDepth;
		settings.cacheMinConfidence = pathPreset.cacheMinConfidence;
		settings.cacheCandidates = pathPreset.cacheCandidates;
		settings.cachePerCellBudget = pathPreset.cachePerCellBudget;
		settings.cacheAlbedoCandidates = pathPreset.cacheAlbedoCandidates;
		settings.cacheAlbedoPerCell = pathPreset.cacheAlbedoPerCell;
		settings.emitterNEEMaxDistance = pathPreset.emitterNEEMaxDistance;
	}
	settings.debugView = static_cast<SurfelGIDebugView>(
		std::clamp(m_context.surfelGIDebugView, 0, 38));
	settings.maxSurfels = static_cast<uint32_t>(std::max(m_context.surfelGIMaxSurfels, 1024));
	// 0 (the context default) means the quality tier's value. An explicit value is used as given.
	settings.maxRayBudget = m_context.surfelGIMaxRayBudget > 0
		? static_cast<uint32_t>(std::max(m_context.surfelGIMaxRayBudget, 1024))
		: GetSurfelGIBudgetCaps(settings.qualityTier).maxRayBudget;
	settings.pathContinuationPeriod = static_cast<uint32_t>(std::clamp(m_context.surfelGIPathContinuationPeriod, 1, 64));
	settings.spawnTileSize = static_cast<uint32_t>(std::clamp(m_context.surfelGISpawnTileSize, 4, 16));
	settings.maxSurfelsPerCell = static_cast<uint32_t>(std::clamp(m_context.surfelGIMaxSurfelsPerCell, 8, 256));
	settings.maxGatherSurfelsPerPixel = m_context.surfelGIMaxGatherSurfelsPerPixel > 0
		? static_cast<uint32_t>(std::clamp(m_context.surfelGIMaxGatherSurfelsPerPixel, 4, 2048))
		: GetSurfelGIBudgetCaps(settings.qualityTier).maxGatherSurfelsPerPixel;
	settings.maxSpawnsPerFrame = static_cast<uint32_t>(std::clamp(m_context.surfelGIMaxSpawnsPerFrame, 1, 65536));
	settings.maxProjectedSurfelsPerFrame = static_cast<uint32_t>(std::clamp(m_context.surfelGIMaxProjectedSurfelsPerFrame, 1024, 524288));
	settings.maxRecycleCountPerFrame = static_cast<uint32_t>(std::clamp(m_context.surfelGIMaxRecycleCountPerFrame, 1, 65536));
	settings.targetSurfelScreenRadiusPx = std::max(m_context.surfelGITargetRadiusPixels, 1.0f);
	settings.minSurfelRadius = std::max(m_context.surfelGIMinRadius, 0.001f);
	settings.maxSurfelRadius = std::max(m_context.surfelGIMaxRadius, settings.minSurfelRadius);
	settings.spawnCoverageThreshold = std::clamp(m_context.surfelGICoverageThreshold, 0.05f, 2.0f);
	settings.recyclePressureStart = std::clamp(m_context.surfelGIRecyclePressure, 0.1f, 0.99f);
	settings.normalRejectCos = std::clamp(m_context.surfelGINormalReject, -0.2f, 0.95f);
	settings.coverageNormalCos = std::clamp(m_context.surfelGICoverageNormalReject, 0.50f, 0.99f);
	settings.coverageDepthTolerance = std::clamp(m_context.surfelGICoverageDepthTolerance, 0.0002f, 0.05f);
	settings.finalGatherNormalCos = std::clamp(m_context.surfelGIFinalGatherNormalReject, -0.2f, 0.95f);
	settings.radialDepthSigmaScale = std::max(m_context.surfelGIRadialDepthVariance, 1.0e-6f);
	settings.indirectIntensity = std::max(m_context.surfelGIIntensity, 0.0f);
	settings.cellAverageFallbackStrength = std::clamp(m_context.surfelGICellAverageFallbackStrength, 0.0f, 1.0f);
	settings.useRadialDepth = m_context.surfelGIUseRadialDepth;
	settings.useRayGuiding = m_context.surfelGIUseRayGuiding;
	settings.useScreenSpaceTrace = m_context.surfelGIUseScreenTrace;
	settings.useSurfelFallbackTrace = m_context.surfelGIUseSurfelFallbackTrace;
	settings.placementValidationMode = m_context.surfelGIPlacementValidation ||
		IsEnvVarEnabled("NOX_SURFEL_GI_PLACEMENT_VALIDATION");

	if (forcedOnForValidation) {
		auto applyEnvToggle = [&settings](const char* name, bool& target) {
			const std::string value = GetEnvVarString(name);
			if (value.empty()) {
				return;
			}

			target = !(value == "0" || value == "false" || value == "FALSE" ||
				value == "False" || value == "no" || value == "NO" || value == "No");
		};

		settings.debugView = static_cast<SurfelGIDebugView>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_DEBUG_VIEW", static_cast<int>(settings.debugView)), 0, 38));
		settings.debugExposure = GetEnvVarFloat("NOX_SURFEL_GI_DEBUG_EXPOSURE", settings.debugExposure);
		settings.maxSurfels = static_cast<uint32_t>(
			std::max(GetEnvVarInt("NOX_SURFEL_GI_MAX_SURFELS", static_cast<int>(settings.maxSurfels)), 1024));
		settings.maxRayBudget = static_cast<uint32_t>(
			std::max(GetEnvVarInt("NOX_SURFEL_GI_RAY_BUDGET", static_cast<int>(settings.maxRayBudget)), 1024));
		settings.maxSpawnsPerFrame = static_cast<uint32_t>(
			std::max(GetEnvVarInt("NOX_SURFEL_GI_MAX_SPAWNS", static_cast<int>(settings.maxSpawnsPerFrame)), 1));
		settings.maxProjectedSurfelsPerFrame = static_cast<uint32_t>(
			std::max(GetEnvVarInt("NOX_SURFEL_GI_MAX_PROJECTED_SURFELS", static_cast<int>(settings.maxProjectedSurfelsPerFrame)), 1024));
		settings.maxRecycleCountPerFrame = static_cast<uint32_t>(
			std::max(GetEnvVarInt("NOX_SURFEL_GI_MAX_RECYCLES", static_cast<int>(settings.maxRecycleCountPerFrame)), 1));
		settings.pathContinuationPeriod = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_PATH_PERIOD", static_cast<int>(settings.pathContinuationPeriod)), 1, 64));
		settings.indirectIntensity = std::max(GetEnvVarFloat("NOX_SURFEL_GI_INTENSITY", settings.indirectIntensity), 0.0f);
		settings.cellAverageFallbackStrength = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_FALLBACK_STRENGTH", settings.cellAverageFallbackStrength),
			0.0f,
			1.0f);
		// Gather experiments (plan section 21): kernel cutoff in base cells, and the interpolated cell-average fallback.
		settings.gatherCutoffCells = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_GATHER_CUTOFF", settings.gatherCutoffCells), 0.0f, 4.0f);
		applyEnvToggle("NOX_SURFEL_GI_GATHER_SMOOTH_FALLBACK", settings.gatherSmoothFallback);
		applyEnvToggle("NOX_SURFEL_GI_UPSCALE_EDGE", settings.gatherEdgeAwareUpsample);
		applyEnvToggle("NOX_SURFEL_GI_HISTORY_REJECT", settings.gatherTemporalRejection);
		applyEnvToggle("NOX_SURFEL_GI_COMPACT_KERNEL", settings.gatherCompactKernel);
		applyEnvToggle("NOX_SURFEL_GI_FULL_CELL_LISTS", settings.gatherFullCellLists);
		applyEnvToggle("NOX_SURFEL_GI_NORMALISED_BLEND", settings.gatherNormalisedBlend);
		settings.constantIrradianceTest = GetEnvVarFloat("NOX_SURFEL_GI_CONST_TEST", settings.constantIrradianceTest);
		applyEnvToggle("NOX_SURFEL_GI_GATHER_STOCHASTIC", settings.gatherStochastic);
		settings.gatherPerCellSamples = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_GATHER_PER_CELL", static_cast<int>(settings.gatherPerCellSamples)), 1, 16));
		applyEnvToggle("NOX_SURFEL_GI_SCREEN_TRACE", settings.useScreenSpaceTrace);
		applyEnvToggle("NOX_SURFEL_GI_BVH_TRACE", settings.useSoftwareBVHTrace);
		applyEnvToggle("NOX_SURFEL_GI_SURFEL_FALLBACK_TRACE", settings.useSurfelFallbackTrace);
		applyEnvToggle("NOX_SURFEL_GI_RAY_GUIDING", settings.useRayGuiding);
		applyEnvToggle("NOX_SURFEL_GI_RADIAL_DEPTH", settings.useRadialDepth);
		applyEnvToggle("NOX_SURFEL_GI_IRRADIANCE_SHARING", settings.useIrradianceSharing);
		applyEnvToggle("NOX_SURFEL_GI_SUN_BVH", settings.sunShadowBVHFallback);
		// World surfels and the path depth cap (plan section 17).
		applyEnvToggle("NOX_SURFEL_GI_WORLD", settings.worldSurfels);
		settings.worldSurfelSpawnsPerFrame = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_WORLD_SPAWNS", static_cast<int>(settings.worldSurfelSpawnsPerFrame)), 0, 4096));
		settings.worldSurfelPopulationFraction = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_WORLD_FRACTION", settings.worldSurfelPopulationFraction), 0.0f, 1.0f);
		// The path depths default to the tier preset (plan sections 17 and 20). Each can be set on its own.
		settings.pathMaxHits = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_PATH_DEPTH", static_cast<int>(settings.pathMaxHits)), 1, 32));
		settings.sunShadowBVHDepth = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_SUN_BVH_DEPTH", static_cast<int>(settings.sunShadowBVHDepth)), 0, 32));
		settings.cacheDepth = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_CACHE_DEPTH", static_cast<int>(settings.cacheDepth)), 1, 32));
		settings.emitterNEEMaxDistance = std::max(
			GetEnvVarFloat("NOX_SURFEL_GI_EMITTER_CULL", settings.emitterNEEMaxDistance), 0.0f);
		settings.cacheMinConfidence = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_CACHE_MIN_CONF", settings.cacheMinConfidence), 0.0f, 1.0f);
		settings.cacheMinWeight = std::max(
			GetEnvVarFloat("NOX_SURFEL_GI_CACHE_MIN_WEIGHT", settings.cacheMinWeight), 0.0f);
		settings.cacheSupportSpread = std::max(
			GetEnvVarFloat("NOX_SURFEL_GI_CACHE_SPREAD", settings.cacheSupportSpread), 0.1f);
		settings.worldRayFloor = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_WORLD_RAY_FLOOR", static_cast<int>(settings.worldRayFloor)), 0, 8));
		settings.worldRefWindow = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_WORLD_REF_WINDOW", static_cast<int>(settings.worldRefWindow)), 1, 4096));
		settings.cacheNormalLo = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_CACHE_NORMAL_LO", settings.cacheNormalLo), 0.0f, 0.94f);
		applyEnvToggle("NOX_SURFEL_GI_CACHE_CONF_RAMP", settings.cacheConfidenceRamp);
		settings.worldLongRate = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_WORLD_LONG_RATE", settings.worldLongRate), 0.0f, 0.5f);
		settings.worldCatchUp = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_WORLD_CATCHUP", settings.worldCatchUp), 0.0f, 10.0f);
		settings.worldCatchUpMax = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_WORLD_CATCHUP_MAX", settings.worldCatchUpMax), 0.0f, 1.0f);
		settings.worldHistoryCap = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_WORLD_HISTORY_CAP", settings.worldHistoryCap), 1.0f, 192.0f);
		settings.screenCatchUp = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_SCREEN_CATCHUP", settings.screenCatchUp), 0.0f, 10.0f);
		settings.screenCatchUpMax = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_SCREEN_CATCHUP_MAX", settings.screenCatchUpMax), 0.0f, 1.0f);
		settings.dormantGap = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_DORMANT_GAP", settings.dormantGap), 0.0f, 1.0f);
		applyEnvToggle("NOX_SURFEL_GI_DISTURB", settings.disturbance);
		settings.disturbFrames = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_DISTURB_FRAMES", static_cast<int>(settings.disturbFrames)), 1, 600));
		applyEnvToggle("NOX_SURFEL_GI_PRIORITY", settings.priorityAlloc);
		settings.priorityShare = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_PRIORITY_SHARE", settings.priorityShare), 0.0f, 1.0f);
		applyEnvToggle("NOX_SURFEL_GI_PRIOR_INIT", settings.priorInit);
		settings.staleGrace = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_STALE_GRACE", settings.staleGrace), 0.0f, 3000.0f);
		settings.cachePlaneScale = std::clamp(
			GetEnvVarFloat("NOX_SURFEL_GI_CACHE_PLANE_SCALE", settings.cachePlaneScale), 0.1f, 20.0f);
		// Debug: trace components to skip (bit mask; plan section 19). Timing ablations only: the output changes.
		settings.traceSkipMask = static_cast<uint32_t>(std::max(GetEnvVarInt("NOX_SURFEL_GI_TRACE_SKIP", 0), 0));
		settings.cacheCandidates = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_CACHE_CANDIDATES", static_cast<int>(settings.cacheCandidates)), 1, 128));
		settings.cachePerCellBudget = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_CACHE_PER_CELL", static_cast<int>(settings.cachePerCellBudget)), 1, 8));
		settings.cacheAlbedoCandidates = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_CACHE_ALBEDO_CANDIDATES", static_cast<int>(settings.cacheAlbedoCandidates)), 1, 128));
		settings.cacheAlbedoPerCell = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_CACHE_ALBEDO_PER_CELL", static_cast<int>(settings.cacheAlbedoPerCell)), 1, 8));
		settings.cacheOwnCellOnly = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_CACHE_OWN_CELL", static_cast<int>(settings.cacheOwnCellOnly)), 0, 1));
		settings.biasProbeStride = static_cast<uint32_t>(std::max(GetEnvVarInt("NOX_SURFEL_GI_BIAS_PROBE", 0), 0));
		settings.biasProbeInterval = static_cast<uint32_t>(
			std::clamp(GetEnvVarInt("NOX_SURFEL_GI_BIAS_PROBE_INTERVAL", static_cast<int>(settings.biasProbeInterval)), 1, 100000));
	}
	// Reference mode reads the surfel composite view too. NOX_GI_REFERENCE=1 with
	// NOX_SURFEL_GI_COMPOSITE_DEBUG=4 then shows the reference term alone (plan section 14, O2).
	const char* compositeDebugEnv = (forcedOnForValidation || settings.referenceMode)
		? "NOX_SURFEL_GI_COMPOSITE_DEBUG"
		: "NOX_LIGHTING_COMPOSITE_DEBUG";
	m_context.lightingCompositeDebugMode = std::clamp(
		GetEnvVarInt(compositeDebugEnv, m_context.lightingCompositeDebugMode),
		0,
		5);
	m_context.lightingGIDebugExposure = GetEnvVarFloat("NOX_SURFEL_GI_DEBUG_EXPOSURE", m_context.lightingGIDebugExposure);

	// Validation override: NOX_SSGI=0/1 switches screen-space indirect diffuse (SSGI) for A/B runs.
	if (const char* ssgiEnv = std::getenv("NOX_SSGI")) {
		m_context.enableIndirectDiffuse = std::atoi(ssgiEnv) != 0;
	}
	const bool captureMode = IsEnvVarEnabled("NOX_SURFEL_GI_CAPTURE");
	ApplySurfelGIRealtimeBudget(settings, captureMode);
	// Examined entries per gather texel (bounded lookup): twice the accepted count, after the tier caps.
	settings.maxGatherCandidatesPerPixel = 2u * settings.maxGatherSurfelsPerPixel;
	// Validation overrides, applied after the tier caps so they can exceed them.
	if (const char* scaleEnv = std::getenv("NOX_SURFEL_GI_FINAL_GATHER_SCALE")) {
		settings.finalGatherResolutionScale = std::clamp(static_cast<float>(std::atof(scaleEnv)), 0.25f, 1.0f);
	}
	// Accepted surfels per gather texel (N of GIBS p.40). The examined bound follows at 2N unless the
	// candidates override is also set. Validation only: this is the cell-order truncation test (plan section 21).
	const std::string acceptedEnv = GetEnvVarString("NOX_SURFEL_GI_GATHER_ACCEPTED");
	if (!acceptedEnv.empty()) {
		settings.maxGatherSurfelsPerPixel = static_cast<uint32_t>(std::clamp(std::atoi(acceptedEnv.c_str()), 1, 2048));
		settings.maxGatherCandidatesPerPixel = 2u * settings.maxGatherSurfelsPerPixel;
	}
	if (const char* candidatesEnv = std::getenv("NOX_SURFEL_GI_GATHER_CANDIDATES")) {
		settings.maxGatherCandidatesPerPixel = static_cast<uint32_t>(std::clamp(std::atoi(candidatesEnv), 1, 1024));
	}
	settings.spawnPopulationKappa = std::clamp(GetEnvVarFloat("NOX_SURFEL_GI_SPAWN_BUDGET_SCALE", settings.spawnPopulationKappa), 0.0f, 16.0f);
	settings.spawnSearchStride = static_cast<uint32_t>(
		std::clamp(GetEnvVarInt("NOX_SURFEL_GI_SPAWN_STRIDE", static_cast<int>(settings.spawnSearchStride)), 1, 4));
	settings.maxSpawnBudgetPerFrame = static_cast<uint32_t>(
		std::max(GetEnvVarInt("NOX_SURFEL_GI_SPAWN_BUDGET_CEILING", static_cast<int>(settings.maxSpawnBudgetPerFrame)), 1));
	settings.spawnBudgetTileDivisor = std::max(
		GetEnvVarFloat("NOX_SURFEL_GI_SPAWN_BUDGET_TILE_DIVISOR", settings.spawnBudgetTileDivisor), 1.0f);
	settings.spawnPopulationCap = static_cast<uint32_t>(
		std::max(GetEnvVarInt("NOX_SURFEL_GI_POP_CAP", static_cast<int>(settings.spawnPopulationCap)), 1));
	settings.spawnBudgetDeficitLimit =
		GetEnvVarInt("NOX_SURFEL_GI_SPAWN_DEFICIT_LIMIT", settings.spawnBudgetDeficitLimit ? 1 : 0) != 0;
	settings.cellAverageAllCells =
		GetEnvVarInt("NOX_SURFEL_GI_CELL_AVG_ALL", settings.cellAverageAllCells ? 1 : 0) != 0;
	settings.gridReachCellScale = std::clamp(
		GetEnvVarFloat("NOX_SURFEL_GI_GRID_REACH", settings.gridReachCellScale), 0.0f, 4.0f);
	// Attribution knob (plan section 24): 0 = linear grid cells instead of the default non-linear (near-dense) grid.
	if (const char* nonLinearEnv = std::getenv("NOX_SURFEL_GI_NONLINEAR_GRID")) {
		settings.useNonLinearGrid = std::atoi(nonLinearEnv) != 0;
	}
	m_context.surfelGIUseScreenTrace = settings.useScreenSpaceTrace;
	m_context.surfelGIUseRadialDepth = settings.useRadialDepth;
	m_context.surfelGIUseRayGuiding = settings.useRayGuiding;
	m_context.surfelGIIntensity = settings.indirectIntensity;
	m_context.surfelGICellAverageFallbackStrength = settings.cellAverageFallbackStrength;
	m_context.surfelGIMaxSurfels = static_cast<int>(settings.maxSurfels);
	// The resolved ray budget is not written back: the context keeps 0 (tier default) so a tier change takes effect.
	m_context.surfelGISpawnTileSize = static_cast<int>(settings.spawnTileSize);
	m_context.surfelGIMaxSurfelsPerCell = static_cast<int>(settings.maxSurfelsPerCell);
	m_context.surfelGIMaxSpawnsPerFrame = static_cast<int>(settings.maxSpawnsPerFrame);
	m_context.surfelGIMaxProjectedSurfelsPerFrame = static_cast<int>(settings.maxProjectedSurfelsPerFrame);
	m_context.surfelGIMaxRecycleCountPerFrame = static_cast<int>(settings.maxRecycleCountPerFrame);

	m_surfelGIManager->SetSettings(settings);
}

std::vector<ModularRenderer::ResourceHandle> ModularRenderer::GetRequiredOutputsForKey(const PlanCacheKey& key) const
{
	if (key.mode == FrameGraphMode::PATH_TRACED) {
		return { "Backbuffer" };
	}
	if (key.mode == FrameGraphMode::DEFERRED_DEBUG) {
		return { "Backbuffer" };
	}
	if (key.presentIndirectDiffuseDebug) {
		return { "Backbuffer", ResourceNames::IndirectDiffuseDebug };
	}
	return { "Backbuffer", "HDRColor" };
}

void ModularRenderer::ExecuteFramePlan(const FramePlan& plan)
{
	for (const PassDescriptor* descriptor : plan.executionOrder) {
		if (!descriptor) {
			continue;
		}
		m_profilePassFunc(descriptor->name.c_str(), descriptor->execute);
		CheckGLError(descriptor->name);
	}
}

ModularRenderer::FramePlan ModularRenderer::CompileFramePlan(const PlanCacheKey& key) const
{
	FramePlan plan;
	std::vector<const PassDescriptor*> activePasses;
	activePasses.reserve(m_passDescriptors.size());
	for (const PassDescriptor& descriptor : m_passDescriptors) {
		if (!descriptor.condition || descriptor.condition(m_context)) {
			activePasses.push_back(&descriptor);
		}
	}

	std::unordered_map<ResourceHandle, std::vector<const PassDescriptor*>> producers;
	for (const PassDescriptor* descriptor : activePasses) {
		for (const ResourceHandle& output : descriptor->outputs) {
			producers[output].push_back(descriptor);
		}
	}

	std::unordered_set<const PassDescriptor*> livePasses;
	std::queue<ResourceHandle> requiredQueue;
	std::unordered_set<ResourceHandle> visitedResources;
	for (const ResourceHandle& output : GetRequiredOutputsForKey(key)) {
		requiredQueue.push(output);
		visitedResources.insert(output);
	}

	while (!requiredQueue.empty()) {
		const ResourceHandle resource = requiredQueue.front();
		requiredQueue.pop();
		const auto producerIt = producers.find(resource);
		if (producerIt == producers.end()) {
			continue;
		}
		for (const PassDescriptor* producer : producerIt->second) {
			if (!livePasses.insert(producer).second) {
				continue;
			}
			for (const ResourceHandle& input : producer->inputs) {
				if (visitedResources.insert(input).second) {
					requiredQueue.push(input);
				}
			}
		}
	}

	std::unordered_map<const PassDescriptor*, int> indegree;
	std::unordered_map<const PassDescriptor*, std::vector<const PassDescriptor*>> graph;
	for (const PassDescriptor* pass : activePasses) {
		if (livePasses.find(pass) == livePasses.end()) {
			continue;
		}
		indegree[pass] = 0;
	}

	std::unordered_map<ResourceHandle, const PassDescriptor*> singleProducer;
	for (const PassDescriptor* pass : activePasses) {
		if (livePasses.find(pass) == livePasses.end()) {
			continue;
		}
		for (const ResourceHandle& output : pass->outputs) {
			singleProducer[output] = pass;
		}
	}

	for (const PassDescriptor* pass : activePasses) {
		if (livePasses.find(pass) == livePasses.end()) {
			continue;
		}
		for (const ResourceHandle& input : pass->inputs) {
			const auto producerIt = singleProducer.find(input);
			if (producerIt == singleProducer.end()) {
				continue;
			}
			const PassDescriptor* producer = producerIt->second;
			if (producer == pass) {
				continue;
			}
			graph[producer].push_back(pass);
			++indegree[pass];
		}
	}

	std::queue<const PassDescriptor*> ready;
	for (const PassDescriptor* pass : activePasses) {
		if (livePasses.find(pass) == livePasses.end()) {
			continue;
		}
		if (indegree[pass] == 0) {
			ready.push(pass);
		}
	}

	while (!ready.empty()) {
		const PassDescriptor* current = ready.front();
		ready.pop();
		plan.executionOrder.push_back(current);
		for (const PassDescriptor* dependent : graph[current]) {
			if (--indegree[dependent] == 0) {
				ready.push(dependent);
			}
		}
	}

	return plan;
}

void ModularRenderer::BuildPassDescriptors(
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& lighting,
	const std::shared_ptr<Skybox>& skybox)
{
	m_passDescriptors.clear();

	auto addPass = [this](PassDescriptor descriptor) {
		m_passDescriptors.push_back(std::move(descriptor));
		};

	addPass({
		"RTPass", {}, { "HDRColor" },
		[this](const RenderContext&) { return DetermineFrameGraphMode() == FrameGraphMode::PATH_TRACED; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_rtPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"ShadowPass", {}, { "ShadowMap" },
		[this](const RenderContext& ctx) {
			if (DetermineFrameGraphMode() == FrameGraphMode::DEFERRED) {
				return true;
			}
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED_DEBUG &&
				ctx.debugMode == RenderContext::DebugMode::SHADOW_MAPS;
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_shadowPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"LightClusterPass", { "ShadowMap" }, { "LightClusters" },
		[this](const RenderContext&) { return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_lightClusterPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"GBufferPass", {}, { "GBufferBase" },
		[this](const RenderContext& ctx) {
			if (DetermineFrameGraphMode() == FrameGraphMode::DEFERRED) {
				return true;
			}
			if (DetermineFrameGraphMode() != FrameGraphMode::DEFERRED_DEBUG) {
				return false;
			}
			return ctx.debugMode != RenderContext::DebugMode::SHADOW_MAPS;
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_gbufferPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		// Same predicate as GBufferPass: every "GBuffer" consumer depends on this pass. It returns
		// early when the scene has no decals.
		"DecalPass", { "GBufferBase" }, { "GBuffer" },
		[this](const RenderContext& ctx) {
			if (DetermineFrameGraphMode() == FrameGraphMode::DEFERRED) {
				return true;
			}
			if (DetermineFrameGraphMode() != FrameGraphMode::DEFERRED_DEBUG) {
				return false;
			}
			return ctx.debugMode != RenderContext::DebugMode::SHADOW_MAPS;
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_decalPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"TransformHistoryPass", { "GBuffer" }, { "TransformHistory" },
		[this](const RenderContext&) { return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			if (m_transformHistoryPass) {
				m_transformHistoryPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			}
		}
		});
	addPass({
		"SurfelGIUpdatePass", { "GBuffer", "TransformHistory" }, { ResourceNames::SurfelGIIndirect },
		[this](const RenderContext&) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				m_surfelGIManager &&
				m_surfelGIManager->GetSettings().enabled;
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_surfelGIManager->BeginFrame(m_context, sceneGraph, camera);
			m_surfelGIManager->Execute(m_context, sceneGraph, camera, lighting, skybox);
			if (m_context.hdrFBO) {
				m_surfelGIManager->ApplyIndirect(m_context, *m_context.hdrFBO);
			}
			m_namedResources[ResourceNames::SurfelGIIndirect] = m_surfelGIManager->GetIndirectTexture();
		}
		});
	addPass({
		"DebugViewPass", {}, { "CompositedColor" },
		[this](const RenderContext&) { return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED_DEBUG; },
		[this]() { visualizeDebugMode(m_context); }
		});
	addPass({
		"LPVPass", { "TransformHistory" }, { ResourceNames::LPVIndirect },
		[](const RenderContext& ctx) {
			return ctx.enableLPV && !(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			if (!m_lpvPass) {
				return;
			}
			m_lpvPass->config.enableLPV = m_context.enableLPV;
			m_lpvPass->config.gridResolution = m_context.lpvGridResolution;
			m_lpvPass->config.voxelSize = m_context.lpvVoxelSize;
			m_lpvPass->config.rsmResolution = m_context.lpvRSMResolution;
			m_lpvPass->config.vplSampleCount = m_context.lpvVPLSampleCount;
			m_lpvPass->config.propagationIterations = m_context.lpvPropagationIterations;
			m_lpvPass->config.enableOcclusion = m_context.lpvEnableOcclusion;
			m_lpvPass->config.giStrength = m_context.lpvGIStrength;
			m_lpvPass->config.updateFrequency = m_context.lpvUpdateFrequency;
			m_lpvPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_namedResources[ResourceNames::LPVIndirect] = m_lpvPass->GetResolvedIndirectTexture();
		}
		});
	addPass({
		"HBAOPass", { "GBuffer" }, { ResourceNames::SSAO },
		[](const RenderContext& ctx) { return ctx.enableSSAO; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_hbaoPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_namedResources[ResourceNames::SSAO] = m_hbaoPass->GetSSAOTexture();
		}
		});
	addPass({
		"ScreenSpaceShadowPass", { "GBuffer" }, { ResourceNames::ScreenSpaceShadow },
		[](const RenderContext& ctx) { return ctx.enableScreenSpaceShadows; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_screenSpaceShadowPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_namedResources[ResourceNames::ScreenSpaceShadow] = m_screenSpaceShadowPass->GetShadowTexture();
		}
		});
	addPass({
		"TAAVelocityPass", { "GBuffer" }, { "Velocity" },
		[](const RenderContext& ctx) {
			return ctx.enableTAA ||
				ctx.enableIndirectDiffuse;
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			if (m_taaPass) {
				m_taaPass->ExecuteVelocity(m_context, sceneGraph, camera);
			}
		}
		});
	addPass({
		"ReflectionProbePass", { "ShadowMap", "LightClusters" }, { "ReflectionProbes" },
		[this](const RenderContext&) { return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_reflectionProbePass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"SSRPass", { "GBuffer", "Velocity" }, { "SSR" },
		[this](const RenderContext&) { return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_ssrPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"BounceableRadiancePass", { "GBuffer", "ShadowMap", "LightClusters", ResourceNames::SSAO, ResourceNames::ScreenSpaceShadow }, { ResourceNames::BounceableRadiance },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED && ctx.enableIndirectDiffuse;
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_lightingPass->SetSSAOTexture(m_namedResources[ResourceNames::SSAO]);
			m_lightingPass->SetScreenSpaceShadowTexture(m_namedResources[ResourceNames::ScreenSpaceShadow]);
			m_lightingPass->ClearIndirectDiffuseSources();
			m_lightingPass->SetOutputMode(LightingPass::OutputMode::BounceableRadiance);
			m_lightingPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_lightingPass->SetOutputMode(LightingPass::OutputMode::FullLighting);
			m_namedResources[ResourceNames::BounceableRadiance] = m_context.hdrFBO->GetColorAttachment(0);
		}
		});
	addPass({
		"IndirectDiffusePass", { "GBuffer", "Velocity", ResourceNames::BounceableRadiance }, { ResourceNames::IndirectDiffuse, ResourceNames::IndirectDiffuseDebug },
		[](const RenderContext& ctx) { return ctx.enableIndirectDiffuse; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			if (!m_indirectDiffusePass) {
				return;
			}
			m_indirectDiffusePass->SetBounceableRadianceTexture(m_namedResources[ResourceNames::BounceableRadiance]);
			m_indirectDiffusePass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_namedResources[ResourceNames::IndirectDiffuse] = m_indirectDiffusePass->GetIndirectDiffuseTexture();
			m_namedResources[ResourceNames::IndirectDiffuseDebug] = m_indirectDiffusePass->GetDebugTexture();
		}
		});
	addPass({
		"LightingPass", { "GBuffer", "ShadowMap", "LightClusters", "SSR", "ReflectionProbes", ResourceNames::SSAO, ResourceNames::ScreenSpaceShadow, ResourceNames::IndirectDiffuse, ResourceNames::LPVIndirect, ResourceNames::SurfelGIIndirect }, { "HDRLit" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_lightingPass->SetSSAOTexture(m_namedResources[ResourceNames::SSAO]);
			m_lightingPass->SetScreenSpaceShadowTexture(m_namedResources[ResourceNames::ScreenSpaceShadow]);
			m_lightingPass->ClearIndirectDiffuseSources();
			const bool surfelValidationActive = m_surfelGIManager &&
				m_surfelGIManager->GetSettings().enabled &&
				m_surfelGIManager->GetSettings().placementValidationMode;
			if (m_context.enableIndirectDiffuse && !surfelValidationActive) {
				m_lightingPass->SetIndirectDiffuseSource(
					0,
					m_namedResources[ResourceNames::IndirectDiffuse],
					m_context.indirectDiffuseStrength);
			}
			if (m_context.enableLPV && !surfelValidationActive) {
				m_lightingPass->SetIndirectDiffuseSource(
					1,
					m_namedResources[ResourceNames::LPVIndirect],
					1.0f);
			}
			if (m_surfelGIManager &&
				m_surfelGIManager->GetSettings().enabled &&
				!surfelValidationActive &&
				m_namedResources[ResourceNames::SurfelGIIndirect].IsValid()) {
				m_lightingPass->SetIndirectDiffuseSource(
					2,
					m_namedResources[ResourceNames::SurfelGIIndirect],
					m_surfelGIManager->GetSettings().indirectIntensity);
			}
			m_lightingPass->SetOutputMode(LightingPass::OutputMode::FullLighting);
			m_lightingPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
		}
		});
	addPass({
		"SkyboxPass", { "HDRLit" }, { "HDRWithSkybox" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &skybox]() {
			m_context.hdrFBO->Bind();
			if (skybox) {
				skybox->Draw(m_context.view, m_context.proj);
			}
		}
		});
	addPass({
		"TransparentForwardPass", { "HDRWithSkybox", "LightClusters" }, { "HDRColor" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_transparentPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"VolumetricFogPass", { "ShadowMap", "LightClusters" }, { "FogVolume" },
		[this](const RenderContext& ctx) { return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED && ctx.enableVolumetricFog; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_volumetricFogPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"RainPass", { "HDRColor", "FogVolume" }, { "HDRRain" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED && ctx.weatherRain > 0.0f &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_rainPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"VolumetricFogApplyPass", { "HDRColor", "HDRRain", "FogVolume" }, { "HDRFogged" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED && ctx.enableVolumetricFog &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this]() { m_volumetricFogPass->Apply(m_context); }
		});
	addPass({
		"VehicleFXPass", { "HDRColor", "HDRRain", "HDRFogged" }, { "HDRVehicleFX" },
		[this](const RenderContext& ctx) {
			// Always scheduled in deferred mode: the frame plan is cached, so a particle-count
			// predicate would be frozen at startup. The pass returns early when nothing is alive.
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_vehicleFXPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_particlePass->Execute(m_context, sceneGraph, camera, lighting, skybox);
		}
		});
	addPass({
		"TAAResolvePass", { "HDRColor", "HDRRain", "HDRFogged", "HDRVehicleFX", "Velocity" }, { "TAAColor" },
		[](const RenderContext& ctx) {
			return ctx.enableTAA && !(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this]() {
			if (m_taaPass) {
				m_taaPass->ExecuteResolve(m_context);
			}
		}
		});
	addPass({
		"MotionBlurPass", { "TAAColor", "HDRFogged", "HDRVehicleFX", "Velocity" }, { "MotionBlurColor" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED && ctx.enableMotionBlur &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_motionBlurPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"AutoExposurePass", { "HDRColor", "HDRFogged", "HDRVehicleFX", "TAAColor", "MotionBlurColor" }, { "ExposureAdapted" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_exposurePass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"BloomPass", { "HDRColor", "HDRFogged", "HDRVehicleFX", "TAAColor", "MotionBlurColor", "ExposureAdapted" }, { ResourceNames::Bloom },
		[this](const RenderContext& ctx) {
			return ctx.enableBloom &&
				DetermineFrameGraphMode() != FrameGraphMode::DEFERRED_DEBUG &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_bloomPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_namedResources[ResourceNames::Bloom] = m_bloomPass->GetBloomResult();
		}
		});
	addPass({
		// After bloom (glare is not bloomed twice) and auto exposure (glare is not metered), before
		// tone mapping. Always scheduled in deferred mode; returns early without flare sources.
		"LensFlarePass", { "HDRFogged", "HDRVehicleFX", "TAAColor", "MotionBlurColor", "ExposureAdapted", ResourceNames::Bloom }, { "LensFlareColor" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_lensFlarePass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"PostProcessPass", { "HDRColor", "HDRFogged", "HDRVehicleFX", "TAAColor", "MotionBlurColor", "ExposureAdapted", ResourceNames::Bloom, "LensFlareColor" }, { "CompositedColor" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() != FrameGraphMode::DEFERRED_DEBUG &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			FrameBuffer::Unbind();
			m_postProcessPass->SetBloomTexture(m_namedResources[ResourceNames::Bloom]);
			m_postProcessPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
		}
		});
	addPass({
		"OverlayComposePass", { "CompositedColor" }, { "OverlayColor" },
		[](const RenderContext& ctx) { return !(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0); },
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			if (m_context.showBoundingBoxes) {
				m_debugBBoxPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			}
		}
		});
	addPass({
		// After tone mapping and TAA, so the outline is neither jittered nor tone mapped. Scheduled only
		// while the editor supplies a selection (never for game or standalone output).
		"SelectionOutlinePass", { "CompositedColor", "GBuffer" }, { "SelectionOutlineColor" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED && ctx.selectionOutline.enabled &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_selectionOutlinePass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"IndirectDiffuseDebugPresent", { ResourceNames::IndirectDiffuseDebug }, { "Backbuffer" },
		[](const RenderContext& ctx) { return ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0; },
		[this]() {
			if (!m_indirectDiffuseDebugPresentShader || !m_context.screenQuad) {
				return;
			}
			rhi::Gpu().BindFramebuffer(PresentTarget::Get());
			rhi::Gpu().SetViewport({ 0, 0, m_context.width, m_context.height });
			rhi::Gpu().SetDepthTest(false);
			rhi::Gpu().SetBlendEnabled(false);
			rhi::Gpu().SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
			rhi::Gpu().Clear(rhi::ClearColor);
			rhi::Gpu().UseProgram(m_indirectDiffuseDebugPresentShader);
			rhi::Gpu().BindTexture(0, m_namedResources[ResourceNames::IndirectDiffuseDebug]);
			if (const rhi::UniformLocation textureLoc = rhi::Gpu().GetUniformLocation(m_indirectDiffuseDebugPresentShader, "uTexture"); textureLoc >= 0) {
				rhi::Gpu().SetUniform(textureLoc, 0);
			}
			m_context.screenQuad->Render();
			rhi::Gpu().SetDepthTest(true);
			rhi::Gpu().SetBlendEnabled(true);
		}
		});
	addPass({
		"GUIPass", { "OverlayColor", "SelectionOutlineColor" }, { "Backbuffer" },
		[](const RenderContext& ctx) { return !(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0); },
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_guiPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
}

bool ModularRenderer::InitializeSharedResources()
{
	// Enable seamless cubemap sampling for IBL
	// This must be enabled before any cubemap is created or sampled
	rhi::Gpu().SetFeature(rhi::Feature::SeamlessCubemaps, true);

	// RT0: RGBA16  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A). 16-bit UNORM: 8-bit oct
	//                normals visibly step mirror reflections on puddles and clear coat (docs/nfs2015).
	// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
	// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
	// RT3: R32UI   - Material routing/debug ID
	// RT4: RGBA16F - Emissive color (RGB) + unused (A)
	// RT5: R32UI   - Stable TransformID for temporal/surfel workflows
	// RT6: RG16F   - Clearcoat factor (R) + clearcoat roughness (G)
	// RT7: RGBA16F - Principled extras: transmission (R), IOR (G), standing water (B), reserved (A)
	m_context.gbufferFBO = std::make_unique<FrameBuffer>(
		m_context.width, m_context.height,
		std::vector<rhi::Format>{
		rhi::Format::RGBA16,   // RT0: Oct normal + roughness/metallic
			rhi::Format::RGBA16F,  // RT1: Albedo + occlusion
			rhi::Format::RGBA16F,  // RT2: Specular F0 (full RGB) + emissive strength
			rhi::Format::R32UI,    // RT3: Material ID
			rhi::Format::RGBA16F,  // RT4: Emissive color (RGB)
			rhi::Format::R32UI,    // RT5: Transform ID
			rhi::Format::RG16F,    // RT6: Clearcoat
			rhi::Format::RGBA16F   // RT7: Principled extras
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
		std::vector<rhi::Format>{ rhi::Format::RGB16F },
		false, false, 1, false, rhi::Format::D24S8
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
	if (m_transformHistoryPass) m_transformHistoryPass->Resize(m_context, newWidth, newHeight);
	if (m_rtPass) m_rtPass->Resize(m_context, newWidth, newHeight);  // Resize path tracing pass
	if (m_hbaoPass) m_hbaoPass->Resize(m_context, newWidth, newHeight);
	if (m_screenSpaceShadowPass) m_screenSpaceShadowPass->Resize(m_context, newWidth, newHeight);
	if (m_indirectDiffusePass) m_indirectDiffusePass->Resize(m_context, newWidth, newHeight);
	if (m_surfelGIManager) m_surfelGIManager->Resize(m_context, static_cast<uint32_t>(newWidth), static_cast<uint32_t>(newHeight));
	if (m_lightingPass) m_lightingPass->Resize(m_context, newWidth, newHeight);
	if (m_bloomPass) m_bloomPass->Resize(m_context, newWidth, newHeight);
	if (m_taaPass) m_taaPass->Resize(m_context, newWidth, newHeight);
	if (m_transparentPass) m_transparentPass->Resize(m_context, newWidth, newHeight);
	if (m_postProcessPass) m_postProcessPass->Resize(m_context, newWidth, newHeight);
	if (m_guiPass) m_guiPass->Resize(m_context, newWidth, newHeight);
	if (m_debugBBoxPass) m_debugBBoxPass->Resize(m_context, newWidth, newHeight);
	if (m_selectionOutlinePass) m_selectionOutlinePass->Resize(m_context, newWidth, newHeight);
	if (m_lightClusterPass) m_lightClusterPass->Resize(m_context, newWidth, newHeight);
	if (m_ssrPass) m_ssrPass->Resize(m_context, newWidth, newHeight);
	if (m_volumetricFogPass) m_volumetricFogPass->Resize(m_context, newWidth, newHeight);
	if (m_motionBlurPass) m_motionBlurPass->Resize(m_context, newWidth, newHeight);
	if (m_lensFlarePass) m_lensFlarePass->Resize(m_context, newWidth, newHeight);

	std::cout << "[ModularRenderer] Resized to " << newWidth << "x" << newHeight << "\n";
}

void ModularRenderer::UpdateContext(const std::shared_ptr<Camera>& camera,
	float exposure, float gamma,
	bool enableShadows, float shadowBias,
	float shadowNear, float shadowFar,
	glm::vec3 envColor)
{
	// Update per-frame parameters (from MainWindow function call)
	m_context.exposure = exposure;
	m_context.gamma = gamma;
	m_context.enableShadows = enableShadows;
	m_context.shadowBias = shadowBias;
	m_context.shadowNear = shadowNear;
	m_context.shadowFar = shadowFar;
	m_context.envColor = envColor;


	// Compute view/projection matrices
	float aspect = static_cast<float>(m_context.width) / static_cast<float>(m_context.height);

	if (m_context.enableTAA && m_taaPass) {
		m_taaPass->PrepareJitter(m_context.taaJitterPattern);
	}
	// History accumulated before TAA was switched off is stale once it is switched back on.
	if (m_context.enableTAA && !m_lastTAAEnabled && m_taaPass) {
		m_taaPass->ResetHistory();
	}
	m_lastTAAEnabled = m_context.enableTAA;

	// Store previous matrices for temporal reprojection
	m_context.prevView = m_context.view;
	m_context.prevProj = m_context.proj;

	// Get current matrices
	auto [view, proj] = camera->GetUpdatedViewProjectionMatrix(
		aspect,
		camera->GetCameraNearPlane(),
		camera->GetCameraFarPlane()
	);

	// Apply TAA jitter if enabled. Jitter is in pixels ([-0.5, 0.5]); one pixel spans 2/size in NDC.
	m_context.prevJitterNdc = m_context.jitterNdc;
	m_context.jitterNdc = glm::vec2(0.0f);
	if (m_context.enableTAA && m_taaPass) {
		const glm::vec2 jitter = m_taaPass->GetCurrentJitter() *
			(2.0f / glm::vec2(m_context.width, m_context.height));
		proj[2][0] += jitter.x;
		proj[2][1] += jitter.y;
		m_context.jitterNdc = jitter;
	}

	m_context.view = view;
	m_context.proj = proj;
	SyncSurfelGISettings();
}


void ModularRenderer::CheckGLError(const std::string& passName)
{
	if constexpr (!DebugErrorChecking) return;

	const char* error = rhi::Gpu().PollError();
	if (error != nullptr) {
		std::cerr << "[ModularRenderer] ERROR after " << passName << ": " << error << std::endl;
	}
}

void ModularRenderer::Render(const std::shared_ptr<SceneGraph>& sceneGraph,
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
	int windowHeight)
{
	m_lastPassMetrics.clear();
	m_lastCpuWaitSyncMs = 0.0f;
	static GpuProfilerPool profilerPool;
	profilerPool.BeginFrame();
	// Clear any stale GL errors from previous frames (only in debug mode)
	if constexpr (DebugErrorChecking) {
		while (rhi::Gpu().PollError() != nullptr) {}
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
	m_context.deltaTime = deltaTime;
	// Scene time advances on live scene frames only: loading and overlay frames (their count depends on how the scene
	// was loaded, async or NOX_SYNC_LOAD) must not move the animated weather, fog or rain.
	if (nox::SceneFrameClock::live) {
		m_context.timeSeconds += deltaTime;
	}
	m_context.lightManager->ResetFrameUploadStats();
	m_context.lightManager->UpdateLights(m_context.deltaTime);
	if (m_context.lightManager->IsLightDataDirty()) {
		m_context.lightManager->UpdateGPUBuffers();
	}
	FrameProfiler::Mark("r_lights");

	// Update context with current frame parameters
	UpdateContext(camera, exposure, gamma, enableShadows, shadowBias, shadow_near, shadow_far, envColor);

	// Camera frustum culling for the frame's render-item submission (docs/nfs2015 M8).
	if (RenderSystem* renderSystem = sceneGraph->GetRenderSystem()) {
		if (m_context.enableFrustumCulling) {
			renderSystem->SetFrustumPlanes(m_context.proj * m_context.view);
		}
		else if (renderSystem->HasValidFrustum()) {
			renderSystem->ClearFrustum();
		}
	}

	// Get skybox from scene
	auto skybox = sceneGraph->GetSkybox();

	// Sync IBL intensity settings from context to skybox
	if (skybox && skybox->IsReady()) {
		skybox->SetIBLIntensity(m_context.iblIntensity);
		skybox->SetSkyboxExposure(m_context.skyboxExposure);
		skybox->SetDiffuseIBLScale(m_context.diffuseIBLScale);
		skybox->SetSpecularIBLScale(m_context.specularIBLScale);
	}
	m_profilePassFunc = [this](const char* name, const std::function<void()>& executePass) {
		ScopedPassProfiler profiler(name, profilerPool);
		executePass();
		profiler.Finish();
		m_lastCpuWaitSyncMs += profiler.metrics.cpuWaitSyncMs;
		m_lastPassMetrics.push_back(profiler.metrics);
		};

	m_context.postSourceFBO = nullptr;

	// Exposure state must be valid before any lighting shader runs this frame.
	if (m_exposurePass) {
		m_profilePassFunc("ExposureBeginPass", [this]() { m_exposurePass->BeginFrame(m_context); });
	}

	m_namedResources.clear();
	m_context.ssaoTexture = rhi::TextureHandle{};
	m_namedResources[ResourceNames::SSAO] = rhi::TextureHandle{};
	m_namedResources[ResourceNames::ScreenSpaceShadow] = rhi::TextureHandle{};
	m_namedResources[ResourceNames::BounceableRadiance] = rhi::TextureHandle{};
	m_namedResources[ResourceNames::IndirectDiffuse] = rhi::TextureHandle{};
	m_namedResources[ResourceNames::IndirectDiffuseDebug] = rhi::TextureHandle{};
	m_namedResources[ResourceNames::SurfelGIIndirect] = rhi::TextureHandle{};
	m_namedResources[ResourceNames::Bloom] = rhi::TextureHandle{};
	m_namedResources[ResourceNames::LPVIndirect] = rhi::TextureHandle{};

	FrameProfiler::Mark("r_setup");
	BuildPassDescriptors(sceneGraph, camera, lighting, skybox);
	const PlanCacheKey key = BuildPlanCacheKey();
	auto planIt = m_planCache.find(key);
	if (planIt == m_planCache.end()) {
		FramePlan compiledPlan = CompileFramePlan(key);
		planIt = m_planCache.emplace(key, std::move(compiledPlan)).first;
	}
	FrameProfiler::Mark("r_plan");
	ExecuteFramePlan(planIt->second);
	FrameProfiler::Mark("r_passes");

	if (m_surfelGIManager &&
		m_surfelGIManager->GetSettings().enabled &&
		m_surfelGIManager->GetSettings().debugView != SurfelGIDebugView::Off) {
		m_profilePassFunc("SurfelGIDebugPass", [this]() {
			m_surfelGIManager->RenderDebug(m_context);
		});
		CheckGLError("SurfelGIDebugPass");
	}

	// Cadence counts live scene frames (SceneFrameClock), so the overlay frame after a commit is not in the sequence.
	if (IsEnvVarEnabled("NOX_LOG_PASS_METRICS") && nox::SceneFrameClock::live) {
		static uint64_t metricsLastTotal = ~uint64_t{ 0 };
		const uint64_t sceneFrame = nox::SceneFrameClock::scene;
		const uint32_t interval = static_cast<uint32_t>(
			std::max(GetEnvVarInt("NOX_LOG_PASS_METRICS_INTERVAL", 120), 1));
		if (metricsLastTotal != nox::SceneFrameClock::total && ((sceneFrame - 1) % interval) == 0u) {
			metricsLastTotal = nox::SceneFrameClock::total;
			std::cout << "[FramePassMetrics] begin frame=" << sceneFrame
				<< " passCount=" << m_lastPassMetrics.size() << std::endl;
			for (const auto& metric : m_lastPassMetrics) {
				std::cout << "[FramePassMetrics] pass=" << metric.name
					<< " cpuMs=" << metric.cpuTimeMs
					<< " gpuMs=" << metric.gpuTimeMs
					<< " dispatches=" << metric.dispatchCount
					<< " drawCalls=" << metric.drawCalls
					<< " cpuWaitMs=" << metric.cpuWaitSyncMs
					<< std::endl;
			}
			std::cout << "[FramePassMetrics] end" << std::endl;
		}
	}
}

void ModularRenderer::visualizeDebugMode(RenderContext& ctx)
{
	// Output debug visualization to backbuffer
	rhi::Gpu().BindFramebuffer(PresentTarget::Get());
	rhi::Gpu().SetViewport({ 0, 0, ctx.width, ctx.height });
	rhi::Gpu().SetDepthTest(false);
	rhi::Gpu().SetBlendEnabled(false);

	rhi::Gpu().SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	rhi::Gpu().Clear(rhi::ClearColor);

	if (!ctx.gbufferFBO || !ctx.screenQuad) {
		std::cerr << "[ModularRenderer] Missing G-buffer or screen quad for debug viz" << std::endl;
		// Restore state before returning
		rhi::Gpu().SetDepthTest(true);
		return;
	}

	if (!m_debugViewShader) {
		rhi::Gpu().SetDepthTest(true);
		return;
	}

	int mode = 0;
	switch (ctx.debugMode) {
	case RenderContext::DebugMode::ALBEDO:
		mode = 1;
		break;
	case RenderContext::DebugMode::NORMAL:
		mode = 2;
		break;
	case RenderContext::DebugMode::DEPTH:
		mode = 3;
		break;
	case RenderContext::DebugMode::SHADOW_MAPS:
		mode = 4;
		break;
	case RenderContext::DebugMode::MATERIAL_ID:
		mode = 5;
		break;
	case RenderContext::DebugMode::TRANSFORM_ID:
		mode = 6;
		break;
	default:
		rhi::Gpu().SetDepthTest(true);
		return;
	}

	rhi::Gpu().UseProgram(m_debugViewShader);

	const rhi::UniformLocation locMode = rhi::Gpu().GetUniformLocation(m_debugViewShader, "uMode");
	const rhi::UniformLocation locShadowLayer = rhi::Gpu().GetUniformLocation(m_debugViewShader, "uShadowLayer");
	if (locMode >= 0) rhi::Gpu().SetUniform(locMode, mode);
	if (locShadowLayer >= 0) rhi::Gpu().SetUniform(locShadowLayer, 0);

	rhi::Gpu().BindTexture(0, ctx.gbufferFBO->GetColorAttachment(1));
	if (rhi::UniformLocation loc = rhi::Gpu().GetUniformLocation(m_debugViewShader, "uAlbedo"); loc >= 0) rhi::Gpu().SetUniform(loc, 0);

	rhi::Gpu().BindTexture(1, ctx.gbufferFBO->GetColorAttachment(0));
	if (rhi::UniformLocation loc = rhi::Gpu().GetUniformLocation(m_debugViewShader, "uNormalPacked"); loc >= 0) rhi::Gpu().SetUniform(loc, 1);

	rhi::Gpu().BindTexture(2, ctx.gbufferFBO->GetColorAttachment(3));
	if (rhi::UniformLocation loc = rhi::Gpu().GetUniformLocation(m_debugViewShader, "uMaterialID"); loc >= 0) rhi::Gpu().SetUniform(loc, 2);

	rhi::Gpu().BindTexture(3, ctx.gbufferFBO->GetColorAttachment(5));
	if (rhi::UniformLocation loc = rhi::Gpu().GetUniformLocation(m_debugViewShader, "uTransformID"); loc >= 0) rhi::Gpu().SetUniform(loc, 3);

	rhi::Gpu().BindTexture(4, ctx.gbufferFBO->GetDepthTexture());
	if (rhi::UniformLocation loc = rhi::Gpu().GetUniformLocation(m_debugViewShader, "uDepth"); loc >= 0) rhi::Gpu().SetUniform(loc, 4);

	rhi::TextureHandle shadowArray = (ctx.lightManager ? ctx.lightManager->GetShadowArrayTexture() : rhi::TextureHandle{});
	if (shadowArray.IsValid() && rhi::Gpu().IsTexture(shadowArray)) {
		rhi::Gpu().BindTexture(5, shadowArray);
	}
	else {
		rhi::Gpu().BindTexture(5, rhi::TextureHandle{});
	}
	if (rhi::UniformLocation loc = rhi::Gpu().GetUniformLocation(m_debugViewShader, "uShadowArray"); loc >= 0) rhi::Gpu().SetUniform(loc, 5);

	ctx.screenQuad->Render();

	// Restore render state
	rhi::Gpu().SetDepthTest(true);
	rhi::Gpu().SetBlendEnabled(true);

	std::cout << "[ModularRenderer] Visualized debug mode: " << static_cast<int>(ctx.debugMode) << std::endl;
}


void ModularRenderer::PrepareScene(const std::shared_ptr<SceneGraph>& sceneGraph)
{
	if (!sceneGraph) return;
	if (m_gbufferPass) m_gbufferPass->PrepareScene(sceneGraph);
	if (m_decalPass) m_decalPass->PrepareScene(sceneGraph);
	if (m_lensFlarePass) m_lensFlarePass->PrepareScene(sceneGraph);
	if (m_reflectionProbePass) m_reflectionProbePass->PrepareScene(m_context, sceneGraph);
}

void ModularRenderer::ResetTAA()
{
	if (m_taaPass) {
		m_taaPass->ResetHistory();
	}
	if (m_exposurePass) {
		m_exposurePass->Reset();
	}
	if (m_ssrPass) {
		m_ssrPass->ResetHistory();
	}
	if (m_volumetricFogPass) {
		m_volumetricFogPass->ResetHistory();
	}
}

void ModularRenderer::RequestReflectionProbeRecapture()
{
	if (m_reflectionProbePass) {
		m_reflectionProbePass->RequestRecapture();
	}
}

int ModularRenderer::GetTAAFrameIndex() const
{
	return m_taaPass ? m_taaPass->GetFrameIndex() : 0;
}

bool ModularRenderer::ExportIndirectDiffuseValidationStages(const std::string& directory) const
{
	if (!m_indirectDiffusePass) {
		return false;
	}
	return m_indirectDiffusePass->ExportValidationStages(directory);
}

bool ModularRenderer::ReadIndirectDiffuseProbe(int stage, int pixelX, int pixelY, IndirectDiffuseProbeSample& outSample) const
{
	if (!m_indirectDiffusePass) {
		return false;
	}

	IndirectDiffusePass::ProbeSample probe;
	if (!m_indirectDiffusePass->ReadValidationProbe(stage, pixelX, pixelY, probe)) {
		return false;
	}

	outSample.value = probe.value;
	outSample.aux = probe.aux;
	outSample.luma = probe.luma;
	outSample.valid = probe.valid;
	return true;
}
