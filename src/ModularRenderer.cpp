#include "ModularRenderer.h"
#include "FrameBuffer.h"
#include "ScreenQuad.h"
#include "ShaderLoader.h"
#include "SceneGraph.h"
#include "Camera.h"
#include "DirectionalLight.h"
#include "Skybox.h"
#include "LightManager.h"

#include "passes/GBufferPass.h"
#include "passes/TransformHistoryPass.h"
#include "passes/ShadowPass.h"
#include "passes/LPVPass.h"
#include "passes/RTPass.h"  // Path tracing pass
#include "passes/SSAOPass.h"
#include "passes/ScreenSpaceShadowPass.h"
#include "passes/IndirectDiffusePass.h"

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
		glDeleteProgram(m_debugViewShader);
		m_debugViewShader = 0;
	}
	if (m_indirectDiffuseDebugPresentShader) {
		glDeleteProgram(m_indirectDiffuseDebugPresentShader);
		m_indirectDiffuseDebugPresentShader = 0;
	}
}

namespace {
	using Clock = std::chrono::high_resolution_clock;
	static constexpr std::size_t kProfilerFramesInFlight = 4;
	static constexpr std::size_t kProfilerReadbackDelay = kProfilerFramesInFlight - 1;

	static bool IsEnvVarEnabled(const char* name) {
#if defined(_MSC_VER)
		char* value = nullptr;
		size_t length = 0;
		if (_dupenv_s(&value, &length, name) != 0) {
			return false;
		}
		const bool enabled = value != nullptr;
		std::free(value);
		return enabled;
#else
		return std::getenv(name) != nullptr;
#endif
	}

	struct PassQuerySlot {
		std::array<GLuint, 2> timestampQueries{ 0, 0 };
		std::array<GLuint, 2> statsQueries{ 0, 0 };
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

	namespace ResourceNames {
		static const std::string SSAO = "SSAO";
		static const std::string ScreenSpaceShadow = "ScreenSpaceShadow";
		static const std::string BounceableRadiance = "BounceableRadiance";
		static const std::string IndirectDiffuse = "IndirectDiffuse";
		static const std::string IndirectDiffuseDebug = "IndirectDiffuseDebug";
		static const std::string Bloom = "Bloom";
		static const std::string LPVR = "LPV_R";
		static const std::string LPVG = "LPV_G";
		static const std::string LPVB = "LPV_B";
	}
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
	m_transformHistoryPass = std::make_unique<TransformHistoryPass>();
	m_rtPass = std::make_unique<RTPass>();  // Create path tracing pass
	m_ssaoPass = std::make_unique<SSAOPass>();
	m_screenSpaceShadowPass = std::make_unique<ScreenSpaceShadowPass>();
	m_indirectDiffusePass = std::make_unique<IndirectDiffusePass>();
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
	success &= m_transformHistoryPass->Initialize(m_context);
	success &= m_rtPass->Initialize(m_context);  // Initialize path tracing pass
	success &= m_ssaoPass->Initialize(m_context);
	success &= m_screenSpaceShadowPass->Initialize(m_context);
	success &= m_indirectDiffusePass->Initialize(m_context);
	success &= m_lightingPass->Initialize(m_context);
	success &= m_bloomPass->Initialize(m_context);
	success &= m_taaPass->Initialize(m_context);
	success &= m_transparentPass->Initialize(m_context);
	success &= m_postProcessPass->Initialize(m_context);
	success &= m_guiPass->Initialize(m_context);
	success &= m_debugBBoxPass->Initialize(m_context);  // Initialize debug bounding box pass

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
	hashCombine(key.presentIndirectDiffuseDebug);
	hashCombine(key.enableScreenSpaceShadows);
	hashCombine(key.enableLPV);
	hashCombine(key.enableTAA);
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
	key.presentIndirectDiffuseDebug =
		m_context.enableIndirectDiffuse &&
		m_context.indirectDiffuseDebugStage > 0 &&
		key.mode == FrameGraphMode::DEFERRED;
	key.enableScreenSpaceShadows = m_context.enableScreenSpaceShadows;
	key.enableLPV = m_context.enableLPV;
	key.enableTAA = m_context.enableTAA;
	return key;
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
		"GBufferPass", {}, { "GBuffer" },
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
		"TransformHistoryPass", { "GBuffer" }, { "TransformHistory" },
		[this](const RenderContext&) { return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			if (m_transformHistoryPass) {
				m_transformHistoryPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			}
		}
		});
	addPass({
		"DebugViewPass", {}, { "CompositedColor" },
		[this](const RenderContext&) { return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED_DEBUG; },
		[this]() { visualizeDebugMode(m_context); }
		});
	addPass({
		"LPVPass", { "TransformHistory" }, { ResourceNames::LPVR, ResourceNames::LPVG, ResourceNames::LPVB },
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
			m_lpvPass->config.propagationAttenuation = m_context.lpvPropagationAttenuation;
			m_lpvPass->config.propagationBias = m_context.lpvPropagationBias;
			m_lpvPass->config.enableOcclusion = m_context.lpvEnableOcclusion;
			m_lpvPass->config.giStrength = m_context.lpvGIStrength;
			m_lpvPass->config.updateFrequency = m_context.lpvUpdateFrequency;
			m_lpvPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_namedResources[ResourceNames::LPVR] = m_lpvPass->GetLPVTextureR();
			m_namedResources[ResourceNames::LPVG] = m_lpvPass->GetLPVTextureG();
			m_namedResources[ResourceNames::LPVB] = m_lpvPass->GetLPVTextureB();
		}
		});
	addPass({
		"SSAOPass", { "GBuffer" }, { ResourceNames::SSAO },
		[](const RenderContext& ctx) { return ctx.enableSSAO; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_ssaoPass->Execute(m_context, sceneGraph, camera, lighting, skybox);
			m_namedResources[ResourceNames::SSAO] = m_ssaoPass->GetSSAOTexture();
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
		[](const RenderContext& ctx) { return ctx.enableTAA || ctx.enableIndirectDiffuse; },
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			if (m_taaPass) {
				m_taaPass->ExecuteVelocity(m_context, sceneGraph, camera);
			}
		}
		});
	addPass({
		"BounceableRadiancePass", { "GBuffer", "ShadowMap", ResourceNames::SSAO, ResourceNames::ScreenSpaceShadow }, { ResourceNames::BounceableRadiance },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED && ctx.enableIndirectDiffuse;
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_lightingPass->SetSSAOTexture(m_namedResources[ResourceNames::SSAO]);
			m_lightingPass->SetScreenSpaceShadowTexture(m_namedResources[ResourceNames::ScreenSpaceShadow]);
			m_lightingPass->SetIndirectDiffuseTexture(0);
			m_lightingPass->SetLPVTextures(0, 0, 0);
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
		"LightingPass", { "GBuffer", "ShadowMap", ResourceNames::SSAO, ResourceNames::ScreenSpaceShadow, ResourceNames::IndirectDiffuse, ResourceNames::LPVR, ResourceNames::LPVG, ResourceNames::LPVB }, { "HDRLit" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() {
			m_lightingPass->SetSSAOTexture(m_namedResources[ResourceNames::SSAO]);
			m_lightingPass->SetScreenSpaceShadowTexture(m_namedResources[ResourceNames::ScreenSpaceShadow]);
			m_lightingPass->SetIndirectDiffuseTexture(m_namedResources[ResourceNames::IndirectDiffuse]);
			m_lightingPass->SetLPVTextures(
				m_namedResources[ResourceNames::LPVR],
				m_namedResources[ResourceNames::LPVG],
				m_namedResources[ResourceNames::LPVB]);
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
		"TransparentForwardPass", { "HDRWithSkybox" }, { "HDRColor" },
		[this](const RenderContext& ctx) {
			return DetermineFrameGraphMode() == FrameGraphMode::DEFERRED &&
				!(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0);
		},
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_transparentPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
	addPass({
		"TAAResolvePass", { "HDRColor", "Velocity" }, { "TAAColor" },
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
		"BloomPass", { "HDRColor", "TAAColor" }, { ResourceNames::Bloom },
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
		"PostProcessPass", { "HDRColor", "TAAColor", ResourceNames::Bloom }, { "CompositedColor" },
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
		"IndirectDiffuseDebugPresent", { ResourceNames::IndirectDiffuseDebug }, { "Backbuffer" },
		[](const RenderContext& ctx) { return ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0; },
		[this]() {
			if (!m_indirectDiffuseDebugPresentShader || !m_context.screenQuad) {
				return;
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, m_context.width, m_context.height);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_BLEND);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			glUseProgram(m_indirectDiffuseDebugPresentShader);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, m_namedResources[ResourceNames::IndirectDiffuseDebug]);
			if (const GLint textureLoc = glGetUniformLocation(m_indirectDiffuseDebugPresentShader, "uTexture"); textureLoc >= 0) {
				glUniform1i(textureLoc, 0);
			}
			m_context.screenQuad->Render();
			glEnable(GL_DEPTH_TEST);
			glEnable(GL_BLEND);
		}
		});
	addPass({
		"GUIPass", { "OverlayColor" }, { "Backbuffer" },
		[](const RenderContext& ctx) { return !(ctx.enableIndirectDiffuse && ctx.indirectDiffuseDebugStage > 0); },
		[this, &sceneGraph, &camera, &lighting, &skybox]() { m_guiPass->Execute(m_context, sceneGraph, camera, lighting, skybox); }
		});
}

bool ModularRenderer::InitializeSharedResources()
{
	// Enable seamless cubemap sampling for IBL
	// This must be enabled before any cubemap is created or sampled
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

	// RT0: RGBA8   - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
	// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
	// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
	// RT3: R32UI   - Material routing/debug ID
	// RT4: RGBA16F - Emissive color (RGB) + unused (A)
	// RT5: R32UI   - Stable TransformID for temporal/surfel workflows
	// RT6: RG16F   - Clearcoat factor (R) + clearcoat roughness (G)
	// RT7: RGBA16F - Principled extras: transmission (R), IOR (G), reserved (BA)
	m_context.gbufferFBO = std::make_unique<FrameBuffer>(
		m_context.width, m_context.height,
		std::vector<GLenum>{
		GL_RGBA8,    // RT0: Oct normal + roughness/metallic
			GL_RGBA16F,  // RT1: Albedo + occlusion
			GL_RGBA16F,  // RT2: Specular F0 (full RGB) + emissive strength
			GL_R32UI,    // RT3: Material ID
			GL_RGBA16F,  // RT4: Emissive color (RGB)
			GL_R32UI,    // RT5: Transform ID
			GL_RG16F,    // RT6: Clearcoat
			GL_RGBA16F   // RT7: Principled extras
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
	if (m_transformHistoryPass) m_transformHistoryPass->Resize(m_context, newWidth, newHeight);
	if (m_rtPass) m_rtPass->Resize(m_context, newWidth, newHeight);  // Resize path tracing pass
	if (m_ssaoPass) m_ssaoPass->Resize(m_context, newWidth, newHeight);
	if (m_screenSpaceShadowPass) m_screenSpaceShadowPass->Resize(m_context, newWidth, newHeight);
	if (m_indirectDiffusePass) m_indirectDiffusePass->Resize(m_context, newWidth, newHeight);
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

	// Store previous matrices for temporal reprojection
	m_context.prevView = m_context.view;
	m_context.prevProj = m_context.proj;

	// Get current matrices
	auto [view, proj] = camera->GetUpdatedViewProjectionMatrix(
		aspect,
		camera->GetCameraNearPlane(),
		camera->GetCameraFarPlane()
	);

	// Apply TAA jitter if enabled
	if (m_context.enableTAA && m_taaPass) {
		glm::vec2 jitter = m_taaPass->GetCurrentJitter();
		// Scale jitter to projection space
		jitter *= (1.0f / glm::vec2(m_context.width, m_context.height));
		proj[2][0] += jitter.x;
		proj[2][1] += jitter.y;
	}

	m_context.view = view;
	m_context.proj = proj;
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
	m_context.deltaTime = deltaTime;
	m_context.lightManager->ResetFrameUploadStats();
	m_context.lightManager->UpdateLights(m_context.deltaTime);
	if (m_context.lightManager->IsLightDataDirty()) {
		m_context.lightManager->UpdateGPUBuffers();
	}

	// Update context with current frame parameters
	UpdateContext(camera, exposure, gamma, enableShadows, shadowBias, shadow_near, shadow_far, envColor);

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

	m_namedResources.clear();
	m_namedResources[ResourceNames::SSAO] = 0;
	m_namedResources[ResourceNames::ScreenSpaceShadow] = 0;
	m_namedResources[ResourceNames::BounceableRadiance] = 0;
	m_namedResources[ResourceNames::IndirectDiffuse] = 0;
	m_namedResources[ResourceNames::IndirectDiffuseDebug] = 0;
	m_namedResources[ResourceNames::Bloom] = 0;
	m_namedResources[ResourceNames::LPVR] = 0;
	m_namedResources[ResourceNames::LPVG] = 0;
	m_namedResources[ResourceNames::LPVB] = 0;

	BuildPassDescriptors(sceneGraph, camera, lighting, skybox);
	const PlanCacheKey key = BuildPlanCacheKey();
	auto planIt = m_planCache.find(key);
	if (planIt == m_planCache.end()) {
		FramePlan compiledPlan = CompileFramePlan(key);
		planIt = m_planCache.emplace(key, std::move(compiledPlan)).first;
	}
	ExecuteFramePlan(planIt->second);
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

	if (!m_debugViewShader) {
		glEnable(GL_DEPTH_TEST);
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
		glEnable(GL_DEPTH_TEST);
		return;
	}

	glUseProgram(m_debugViewShader);

	const GLint locMode = glGetUniformLocation(m_debugViewShader, "uMode");
	const GLint locShadowLayer = glGetUniformLocation(m_debugViewShader, "uShadowLayer");
	if (locMode >= 0) glUniform1i(locMode, mode);
	if (locShadowLayer >= 0) glUniform1i(locShadowLayer, 0);

	glActiveTexture(GL_TEXTURE0 + 0);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(1));
	if (GLint loc = glGetUniformLocation(m_debugViewShader, "uAlbedo"); loc >= 0) glUniform1i(loc, 0);

	glActiveTexture(GL_TEXTURE0 + 1);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
	if (GLint loc = glGetUniformLocation(m_debugViewShader, "uNormalPacked"); loc >= 0) glUniform1i(loc, 1);

	glActiveTexture(GL_TEXTURE0 + 2);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(3));
	if (GLint loc = glGetUniformLocation(m_debugViewShader, "uMaterialID"); loc >= 0) glUniform1i(loc, 2);

	glActiveTexture(GL_TEXTURE0 + 3);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(5));
	if (GLint loc = glGetUniformLocation(m_debugViewShader, "uTransformID"); loc >= 0) glUniform1i(loc, 3);

	glActiveTexture(GL_TEXTURE0 + 4);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	if (GLint loc = glGetUniformLocation(m_debugViewShader, "uDepth"); loc >= 0) glUniform1i(loc, 4);

	glActiveTexture(GL_TEXTURE0 + 5);
	GLuint shadowArray = (ctx.lightManager ? ctx.lightManager->GetShadowArrayTexture() : 0);
	if (shadowArray > 0 && glIsTexture(shadowArray)) {
		glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArray);
	}
	else {
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	}
	if (GLint loc = glGetUniformLocation(m_debugViewShader, "uShadowArray"); loc >= 0) glUniform1i(loc, 5);

	ctx.screenQuad->Render();

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
