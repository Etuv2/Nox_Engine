#pragma once

#include <GL/glew.h>
#include <cstdint>
#include <memory>

class Camera;
class DirectionalLight;
class FrameBuffer;
class SceneGraph;
class Skybox;
class SurfelGIPipeline;
struct RenderContext;

enum class SurfelGIQualityTier : uint32_t {
	Low = 0,
	Medium = 1,
	High = 2,
	Ultra = 3
};

enum class SurfelGIDebugView : uint32_t {
	Off = 0,
	SurfelSpheres = 1,
	SurfelNormals = 2,
	SurfelAge = 3,
	SurfelVariance = 4,
	SurfelCoverage = 5,
	SurfelGridCells = 6,
	CellOccupancy = 7,
	SpawnRecycle = 8,
	RayCounts = 9,
	RayGuide = 10,
	RadialDepth = 11,
	IndirectOnly = 12,
	IndirectDifferenceVsSSGI = 13,
	Timings = 14,
	RecycleScore = 15,
	StaleSurfels = 16,
	RayHitRadiance = 17,
	GatherWeights = 18,
	GBufferWorldPosition = 19,
	GBufferNormal = 20,
	GBufferTransformID = 21,
	GBufferMaterialID = 22,
	SpawnCandidates = 23,
	GBufferDepth = 24,
	StoredSurfelWorldPosition = 25,
	StoredSurfelRadius = 26,
	StoredSurfelTransformID = 27,
	StoredSurfelFlags = 28,
	DebugDrawPosition = 29
};

struct SurfelGISettings {
	bool enabled = false;
	SurfelGIQualityTier qualityTier = SurfelGIQualityTier::Medium;
	SurfelGIDebugView debugView = SurfelGIDebugView::Off;

	uint32_t maxSurfels = 131072u;
	uint32_t maxRayBudget = 32768u;
	uint32_t spawnTileSize = 8u;
	uint32_t maxSurfelsPerCell = 64u;
	uint32_t maxGatherSurfelsPerPixel = 512u;
	uint32_t gatherNeighborRadius = 1u;
	uint32_t rayUpdateInterval = 2u;
	uint32_t radialDepthUpdateInterval = 4u;
	uint32_t spawnPasses = 2u;
	uint32_t fastFillSpawnPasses = 4u;
	uint32_t fastFillFrameCount = 16u;
	uint32_t stationaryFastFillFrames = 12u;

	float targetSurfelScreenRadiusPx = 3.0f;
	float minSurfelRadius = 0.03f;
	float maxSurfelRadius = 5.0f;
	float spawnCoverageThreshold = 0.85f;
	float stationaryCameraEpsilon = 0.0025f;
	float recyclePressureStart = 0.85f;
	float normalRejectCos = 0.25f;
	float finalGatherNormalCos = 0.15f;
	float radialDepthSigmaScale = 1.0f;
	float indirectIntensity = 1.0f;
	float skyMissRadianceMultiplier = 0.0f;
	float finalGatherResolutionScale = 1.0f;

	bool useNonLinearGrid = true;
	bool useRadialDepth = false;
	bool useRayGuiding = false;
	bool useRayBinning = true;
	bool useIrradianceSharing = true;
	bool useScreenSpaceTrace = true;
	bool useSoftwareBVHTrace = true;
	bool useSurfelFallbackTrace = true;
	bool placementValidationMode = false;
};

struct SurfelGIFrameStats {
	bool configuredEnabled = false;
	bool ready = false;
	uint32_t liveSurfels = 0;
	uint32_t spawnedThisFrame = 0;
	uint32_t recycledThisFrame = 0;
	uint32_t requestedRays = 0;
	uint32_t allocatedRays = 0;
	uint32_t overflowSurfels = 0;
	uint32_t overflowGridEntries = 0;
	uint32_t rejectedInvalidDepth = 0;
	uint32_t rejectedInvalidTransform = 0;
	uint32_t rejectedInvalidMaterial = 0;
	uint32_t rejectedOutsideGrid = 0;
	uint32_t rejectedInvalidWorldPos = 0;
	uint32_t rejectedInvalidNormal = 0;
	uint32_t rejectedInvalidRadius = 0;
	uint32_t rejectedPoolFull = 0;
	float updateTimeMs = 0.0f;
	float traceTimeMs = 0.0f;
	float applyTimeMs = 0.0f;
};

class SurfelGIManager final {
public:
	SurfelGIManager();
	~SurfelGIManager();

	SurfelGIManager(const SurfelGIManager&) = delete;
	SurfelGIManager& operator=(const SurfelGIManager&) = delete;

	bool Init(RenderContext& context, const SurfelGISettings& settings = SurfelGISettings{});
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
	void RenderDebug(RenderContext& context) const;
	bool ReloadShaders();

	bool IsInitialized() const { return m_initialized; }
	bool IsEnabled() const;
	void SetEnabled(bool enabled);
	void SetSettings(const SurfelGISettings& settings);
	const SurfelGISettings& GetSettings() const { return m_settings; }
	const SurfelGIFrameStats& GetStats() const { return m_stats; }

	GLuint GetSurfelBuffer() const;
	GLuint GetCountersBuffer() const;
	GLuint GetGridHeaderBuffer() const;
	GLuint GetGridEntryBuffer() const;
	GLuint GetIndirectTexture() const;

private:
	bool EnsurePipeline(RenderContext& context);
	bool HasReadyPipeline() const;

	std::unique_ptr<SurfelGIPipeline> m_pipeline;
	SurfelGISettings m_settings{};
	SurfelGIFrameStats m_stats{};
	bool m_initialized = false;
};
