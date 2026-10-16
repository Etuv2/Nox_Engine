#pragma once

#include "SurfelGIManager.h"
#include "../ComputeShader.h"
#include "../surfel_gi/SurfelGIResources.h"

#include "rhi/RHI.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

class Camera;
class DirectionalLight;
class FrameBuffer;
class SceneGraph;
class Skybox;
struct RenderContext;

struct SurfelGIPipelineResources {
	rhi::BufferHandle surfelBuffer;
	rhi::BufferHandle freeListBuffer;
	rhi::BufferHandle countersBuffer;
	rhi::BufferHandle gridHeaderBuffer;
	rhi::BufferHandle gridEntryBuffer;
	rhi::BufferHandle gridCellSumBuffer;
	rhi::BufferHandle gridCountersBuffer;
	rhi::BufferHandle rayRequestBuffer;
	rhi::BufferHandle rayBuffer;
	rhi::BufferHandle rayHitBuffer;
	rhi::BufferHandle rayAllocScratchBuffer;
	rhi::BufferHandle worldRequestBuffer;
	rhi::BufferHandle cellStampBuffer;
	rhi::BufferHandle radialDepthBuffer;
	rhi::BufferHandle guideMapBuffer;
	rhi::BufferHandle irradianceSnapshotBuffer;
	rhi::BufferHandle coverageTileBuffer;
	// Indirect dispatch arguments for the surfel-wide passes (written by dispatch_args.comp).
	rhi::BufferHandle dispatchArgsBuffer;
	// Ring of two staging buffers for the asynchronous counter readback.
	rhi::BufferHandle counterStaging[2];
	rhi::TextureHandle rawIndirectTexture;
	rhi::TextureHandle coverageTexture;   // gather coverage for the next frame's spawn pass
	rhi::TextureHandle indirectTexture;
	// Previous upscaled output (full resolution), ping-ponged. Read by the temporal step of upscale_filter.comp.
	rhi::TextureHandle historyIndirectTexture[2];
	// Reference mode only: quarter-resolution running mean of diffuse irradiance (rgb) and its sample count.
	rhi::TextureHandle referenceMeanTexture;
	rhi::TextureHandle referenceCountTexture;
};

class SurfelGIPipeline final {
public:
	SurfelGIPipeline() = default;
	~SurfelGIPipeline();

	SurfelGIPipeline(const SurfelGIPipeline&) = delete;
	SurfelGIPipeline& operator=(const SurfelGIPipeline&) = delete;

	bool Init(RenderContext& context, const SurfelGISettings& settings);
	void Resize(RenderContext& context, uint32_t width, uint32_t height);
	void Shutdown();

	void BeginFrame(RenderContext& context,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera);
	void Execute(RenderContext& context,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox);
	void ApplyIndirect(RenderContext& context, FrameBuffer& lightingBuffer);
	void RenderDebug(RenderContext& context);
	bool ReloadShaders();
	void SetSettings(const SurfelGISettings& settings);

	bool IsInitialized() const { return m_initialized; }
	bool IsReady() const;
	const SurfelGIPipelineResources& GetResources() const { return m_resources; }
	const SurfelGIFrameStats& GetStats() const { return m_stats; }

private:
	struct SubpassGpuTiming {
		std::string label;
		double gpuMs = 0.0;
	};

	struct PendingGpuTimingQuery {
		std::string label;
		rhi::QueryHandle beginQuery;
		rhi::QueryHandle endQuery;
		uint32_t issuedFrame = 0;
	};

	bool LoadShaders();
	void ReleaseShaders();
	bool CreateIndirectTexture();
	void ReleaseIndirectTexture();
	// Light, shadow and sky uniforms and textures shared by the software BVH tracers.
	void BindSoftwareTraceLighting(rhi::ProgramHandle program, RenderContext& context,
		const std::shared_ptr<Camera>& camera, const std::shared_ptr<Skybox>& skybox);
	// Reference mode (NOX_GI_REFERENCE=1): path-traced accumulation, and resampling into the indirect texture.
	void ExecuteIndependentReference(RenderContext& context, const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera, const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox);
	void ApplyIndependentReference(RenderContext& context);
	void ReleaseAuxiliaryResources();
	bool CreateAuxiliaryResources();
	void BindCoreResources(rhi::BufferHandle transformBuffer) const;
	void BeginGpuTiming(const char* label, rhi::QueryHandle& beginQuery, rhi::QueryHandle& endQuery);
	void EndGpuTiming(const char* label, rhi::QueryHandle beginQuery, rhi::QueryHandle endQuery);
	void CollectReadyGpuTimings(bool forceDelete);
	void ReleaseGpuTimingQueries();

	SurfelGISettings m_settings{};
	SurfelGIPipelineResources m_resources{};
	SurfelGIFrameStats m_stats{};
	std::vector<SubpassGpuTiming> m_subpassTimings;
	std::vector<PendingGpuTimingQuery> m_pendingTimingQueries;
	SurfelPool m_pool;
	SurfelGrid m_grid;
	SurfelRayQueue m_rayQueue;
	std::unique_ptr<ComputeShader> m_updateShader;
	std::unique_ptr<ComputeShader> m_beginFrameShader;
	std::unique_ptr<ComputeShader> m_countLiveShader;
	std::unique_ptr<ComputeShader> m_dispatchArgsShader;
	std::unique_ptr<ComputeShader> m_recycleShader;
	std::unique_ptr<ComputeShader> m_coverageShader;
	std::unique_ptr<ComputeShader> m_spawnShader;
	std::unique_ptr<ComputeShader> m_gridCountShader;
	std::unique_ptr<ComputeShader> m_gridScanShader;
	std::unique_ptr<ComputeShader> m_buildGridShader;
	std::unique_ptr<ComputeShader> m_requestRaysShader;
	std::unique_ptr<ComputeShader> m_allocateRaysShader;
	std::unique_ptr<ComputeShader> m_generateRaysShader;
	std::unique_ptr<ComputeShader> m_traceRaysShader;
	std::unique_ptr<ComputeShader> m_integrateShader;
	std::unique_ptr<ComputeShader> m_radialDepthShader;
	std::unique_ptr<ComputeShader> m_irradianceSnapshotShader;
	std::unique_ptr<ComputeShader> m_irradianceSharingShader;
	std::unique_ptr<ComputeShader> m_applyIndirectShader;
	std::unique_ptr<ComputeShader> m_upscaleFilterShader;
	std::unique_ptr<ComputeShader> m_referenceShader;
	rhi::ProgramHandle m_debugProgram;
	rhi::ProgramHandle m_debugPresentProgram;
	rhi::VertexInputHandle m_debugVAO;
	glm::vec3 m_lastCameraPosition{ 0.0f };
	glm::mat4 m_lastView{ 1.0f };
	// Reference mode: the accumulation restarts when the view changes or the textures are reallocated.
	glm::mat4 m_referenceView{ 1.0f };
	bool m_hasReferenceView = false;
	bool m_referenceResetPending = true;
	std::vector<glm::vec4> m_lastLightingSignature;
	float m_lightingChangeFactor = 0.0f;
	float m_lightingChangeResponse = 0.0f;
	uint32_t m_lightingChangeHoldFrames = 0;
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_indirectWidth = 0;
	uint32_t m_indirectHeight = 0;
	uint32_t m_coverageTileCountX = 0;
	uint32_t m_coverageTileCountY = 0;
	uint32_t m_coverageTileCapacity = 0;
	uint32_t m_frameIndex = 0;
	// Async counter readback: at most one copy in flight, mapped kGpuTimingReadbackFrameDelay later.
	void IssueCounterReadback();
	void PumpCounterReadback();
	bool m_counterReadbackPending = false;
	uint32_t m_counterReadbackSlot = 0;
	uint32_t m_counterReadbackFrame = 0;
	uint32_t m_stationaryFrameCount = 0;
	uint32_t m_lastTransformCount = 0;
	uint32_t m_lastSpawnPassCount = 1;
	uint32_t m_subpassMetricsInterval = 120;
	uint32_t m_lastTextureDumpFrame = 0;
	bool m_hasLastCameraState = false;
	uint32_t m_historyReadIndex = 0;
	bool m_hasTemporalHistory = false;
	bool m_loggedGBufferBindings = false;
	bool m_wasPlacementValidationMode = false;
	bool m_logSubpassMetrics = false;
	bool m_initialized = false;
};
