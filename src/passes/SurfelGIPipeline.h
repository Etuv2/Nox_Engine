#pragma once

#include "SurfelGIManager.h"
#include "../ComputeShader.h"
#include "../surfel_gi/SurfelGIResources.h"

#include <GL/glew.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class Camera;
class DirectionalLight;
class FrameBuffer;
class SceneGraph;
class Skybox;
struct RenderContext;

struct SurfelGIPipelineResources {
	GLuint surfelBuffer = 0;
	GLuint freeListBuffer = 0;
	GLuint countersBuffer = 0;
	GLuint gridHeaderBuffer = 0;
	GLuint gridEntryBuffer = 0;
	GLuint gridAverageBuffer = 0;
	GLuint gridCountersBuffer = 0;
	GLuint rayRequestBuffer = 0;
	GLuint rayBuffer = 0;
	GLuint sortedRayBuffer = 0;
	GLuint rayHitBuffer = 0;
	GLuint rayBinBuffer = 0;
	GLuint rayCountersBuffer = 0;
	GLuint radialDepthBuffer = 0;
	GLuint guideMapBuffer = 0;
	GLuint irradianceSnapshotBuffer = 0;
	GLuint coverageTileBuffer = 0;
	GLuint rawIndirectTexture = 0;
	GLuint filteredIndirectTexture = 0;
	GLuint temporalIndirectTexture = 0;
	GLuint indirectTexture = 0;
	GLuint historyIndirectTexture[2] = { 0, 0 };
	GLuint historyGeometryTexture[2] = { 0, 0 };
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
		GLuint beginQuery = 0;
		GLuint endQuery = 0;
		uint32_t issuedFrame = 0;
	};

	bool LoadShaders();
	void ReleaseShaders();
	bool CreateIndirectTexture();
	void ReleaseIndirectTexture();
	void ReleaseAuxiliaryResources();
	bool CreateAuxiliaryResources();
	void BindCoreResources(GLuint transformBuffer) const;
	void BeginGpuTiming(const char* label, GLuint& beginQuery, GLuint& endQuery);
	void EndGpuTiming(const char* label, GLuint beginQuery, GLuint endQuery);
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
	std::unique_ptr<ComputeShader> m_recycleShader;
	std::unique_ptr<ComputeShader> m_coverageShader;
	std::unique_ptr<ComputeShader> m_spawnShader;
	std::unique_ptr<ComputeShader> m_clearGridShader;
	std::unique_ptr<ComputeShader> m_buildGridShader;
	std::unique_ptr<ComputeShader> m_cellAverageShader;
	std::unique_ptr<ComputeShader> m_requestRaysShader;
	std::unique_ptr<ComputeShader> m_allocateRaysShader;
	std::unique_ptr<ComputeShader> m_generateRaysShader;
	std::unique_ptr<ComputeShader> m_traceRaysShader;
	std::unique_ptr<ComputeShader> m_integrateShader;
	std::unique_ptr<ComputeShader> m_radialDepthShader;
	std::unique_ptr<ComputeShader> m_irradianceSnapshotShader;
	std::unique_ptr<ComputeShader> m_irradianceSharingShader;
	std::unique_ptr<ComputeShader> m_applyIndirectShader;
	std::unique_ptr<ComputeShader> m_spatialFilterShader;
	std::unique_ptr<ComputeShader> m_temporalFilterShader;
	std::unique_ptr<ComputeShader> m_upscaleFilterShader;
	GLuint m_debugProgram = 0;
	GLuint m_debugPresentProgram = 0;
	GLuint m_debugVAO = 0;
	glm::vec3 m_lastCameraPosition{ 0.0f };
	glm::mat4 m_lastView{ 1.0f };
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_indirectWidth = 0;
	uint32_t m_indirectHeight = 0;
	uint32_t m_coverageTileCountX = 0;
	uint32_t m_coverageTileCountY = 0;
	uint32_t m_coverageTileCapacity = 0;
	uint32_t m_frameIndex = 0;
	uint32_t m_stationaryFrameCount = 0;
	uint32_t m_lastTransformCount = 0;
	uint32_t m_lastSpawnPassCount = 1;
	uint32_t m_subpassMetricsInterval = 120;
	uint32_t m_historyReadIndex = 0;
	uint32_t m_lastTextureDumpFrame = 0;
	bool m_hasLastCameraState = false;
	bool m_hasTemporalHistory = false;
	bool m_loggedGBufferBindings = false;
	bool m_wasPlacementValidationMode = false;
	bool m_logSubpassMetrics = false;
	bool m_initialized = false;
};
