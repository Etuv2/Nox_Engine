#pragma once

#include "rhi/RHI.h"
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
	FinalGatherFallback = 36,
	FinalGatherBleed = 37,
	FinalGatherComposite = 38,
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
	DebugDrawPosition = 29,
	StoredSurfelAlbedo = 30,
	RadiusError = 31,
	GridAxisRegion = 32,
	GridOverflow = 33,
	IrradianceConfidence = 34,
	RawIndirectIrradiance = 35
};

struct SurfelGISettings {
	bool enabled = false;
	SurfelGIQualityTier qualityTier = SurfelGIQualityTier::Medium;
	SurfelGIDebugView debugView = SurfelGIDebugView::Off;

	uint32_t maxSurfels = 65536u;
	// Rays traced per frame; shared out by demand when requests exceed it (allocate_rays.comp).
	uint32_t maxRayBudget = 2048u;
	// Path continuation past a ray's first hit (trace_rays.comp): a warp-group of 32 rays continues in one frame of
	// every N, with weight N, so the estimate stays unbiased (plan section 15). 1 = every path continues.
	uint32_t pathContinuationPeriod = 1u;
	// Sun visibility outside the shadow cascades comes from a BVH shadow ray (surfel_scene_trace.glsl), not "lit".
	bool sunShadowBVHFallback = true;
	// World surfels (plan section 17). A path vertex the cache cannot answer queues a request (one per grid cell per frame);
	// spawn.comp's world mode turns up to worldSurfelSpawnsPerFrame of them into surfels per frame. Their population is
	// capped at worldSurfelPopulationFraction of spawnPopulationCap, on top of the screen population (not from it).
	// The quality tier sets this (ModularRenderer.cpp GetSurfelGIPathPreset); the default here is the Ultra path.
	// With it on alone, scene.json energy fell to about 0.66 of the reference (plan section 17.4).
	bool worldSurfels = false;
	uint32_t worldSurfelSpawnsPerFrame = 16u;
	float worldSurfelPopulationFraction = 0.25f;
	// Path depth cap of the surfel trace. 32 is the merged-base behaviour; world mode uses 3.
	uint32_t pathMaxHits = 32u;
	// Path vertices below this depth take the BVH sun fallback for their direct light. 32 is the merged-base behaviour
	// (every vertex); world mode uses 1 (the first hit only).
	uint32_t sunShadowBVHDepth = 32u;
	// Path vertices below this depth read the surfel cache. 2 is the merged-base behaviour; world mode uses 3.
	uint32_t cacheDepth = 2u;
	// Minimum confidence of the surfels that answer a path vertex (0 = any cached weight answers).
	float cacheMinConfidence = 0.0f;
	// Minimum total weight of the surfels that answer a path vertex (the old test was 1e-4).
	float cacheMinWeight = 1.0e-4f;
	// Kernel spread of the cache lookup, in support radii (the default is 3.10).
	float cacheSupportSpread = 3.10f;
	// Hit-referenced ray floor of world surfels (0 = off) and the reference window in frames (plan section 17.9).
	uint32_t worldRayFloor = 0u;
	uint32_t worldRefWindow = 8u;
	// Lower edge of the cache normal weight (the first round used 0.55) and the confidence ramp (1 = on).
	float cacheNormalLo = 0.55f;
	bool cacheConfidenceRamp = true;
	// Long-mean rate of world surfels (0 = the screen floor, 0.02).
	float worldLongRate = 0.0f;
	// World surfels only (plan section 19): MSME-style catch-up gain and its cap on the long rate (gain 0 = off), and the
	// history cap of their cumulative 1/n rate (192 = the screen cap).
	float worldCatchUp = 0.0f;
	float worldCatchUpMax = 0.25f;
	float worldHistoryCap = 192.0f;
	// Screen surfels (plan section 22): the same MSME catch-up, so a surfel whose short-window mean has moved away from
	// its long mean (a lighting change, or a moving object's new surroundings) follows at once. Gain 0 = off.
	float screenCatchUp = 0.0f;
	float screenCatchUpMax = 0.25f;
	// Dormancy gate (plan section 22): a converged surfel stops sending rays only while its short and long means agree
	// to this relative level (0 = no gate, the dormancy of section 20 as before).
	float dormantGap = 0.0f;
	// Round 2 (plan section 22). Disturbance: moved-instance boxes, local change detection and the faster adaptation of
	// disturbed surfels. Priority: young and disturbed surfels are granted first, up to priorityShare of the budget.
	bool disturbance = true;
	uint32_t disturbFrames = 30u;
	bool priorityAlloc = true;
	float priorityShare = 0.6f;
	// Round 2: new surfels start from their cell prior with the weight of 4 rays (spawn.comp), and recycling waits
	// stale grace frames before the stale term starts (recycle_surfels.comp; 30 before round 2).
	bool priorInit = true;
	float staleGrace = 120.0f;
	// Plane-distance sigma scale of the cache weight (1 = the first round's value).
	float cachePlaneScale = 1.0f;
	// Debug only (NOX_SURFEL_GI_TRACE_SKIP): bit mask of trace components to skip, for the cost breakdown (plan section
	// 19). The bits are listed at uTraceSkip in surfel_scene_trace.glsl. 0 = the full trace.
	uint32_t traceSkipMask = 0u;
	// Cache candidate budgets of a hit-point lookup (plan sections 19 and 20): surfels visited in total, and per neighbour
	// cell. The irradiance lookup was 128 and 4; the cached-albedo lookup was 32 and 2. The tier presets set them.
	uint32_t cacheCandidates = 128u;
	uint32_t cachePerCellBudget = 4u;
	uint32_t cacheAlbedoCandidates = 32u;
	uint32_t cacheAlbedoPerCell = 2u;
	// Distance cut of the emitter next-event samples, in metres (0 = no cut). Past it a sample is not taken and a BSDF hit on
	// an emitter there takes full weight, so the estimate stays unbiased (plan section 20).
	float emitterNEEMaxDistance = 0.0f;
	// Bias probe (plan section 19): every Nth ray that a cache lookup answers is also estimated by a continuation of its
	// hit point with no cache. 0 = off. The bucket sums are printed every biasProbeInterval frames.
	uint32_t biasProbeStride = 0u;
	uint32_t biasProbeInterval = 60u;
	uint32_t spawnTileSize = 16u;
	// Search stride for the per-tile lowest-coverage search: 1 tests every pixel of a tile each
	// frame; N > 1 tests a rotating 1/N^2 subset (the lowest coverage over N^2 frames).
	uint32_t spawnSearchStride = 1u;
	// kappa (WP4): population target per visible tile. Live surfels are steered to kappa x visible tiles.
	float spawnPopulationKappa = 1.0f;
	uint32_t maxSurfelsPerCell = 128u;
	uint32_t maxGatherSurfelsPerPixel = 32u;
	// Entries examined per gather texel, accepted or not (bounded lookup). Default 2 x accepted.
	uint32_t maxGatherCandidatesPerPixel = 64u;
	uint32_t gatherNeighborRadius = 1u;
	uint32_t rayUpdateInterval = 1u;
	uint32_t fastFillFrameCount = 16u;
	uint32_t stationaryFastFillFrames = 24u;
	// Ceiling on spawns per frame (tier caps apply). The GPU budget closes the deficit to kappa x visible tiles.
	uint32_t maxSpawnsPerFrame = 64u;
	// Spawn budget ceiling: visible tiles / spawnBudgetTileDivisor, bounded by maxSpawnBudgetPerFrame. The budget
	// follows half of last frame's under-covered tiles, between maxSpawnsPerFrame and that ceiling.
	float spawnBudgetTileDivisor = 16.0f;
	// true: the budget is also capped by the population deficit to kappa x visible tiles. false: only by the coverage demand.
	bool spawnBudgetDeficitLimit = true;
	// Cell averages from every overlapped cell (true) or from the base cell only (false, the default).
	bool cellAverageAllCells = false;
	// Overlap reach beyond a surfel's support, in neighbour cells. 0 = the support sphere only (a surfel is stored only in
	// the cells it touches). 0.75 was the inflated reach of the population round: it fills cells with surfels the lookups
	// do not need, and the bounded lookups then miss the ones they do.
	float gridReachCellScale = 0.0f;
	uint32_t maxSpawnBudgetPerFrame = 512u;
	// Upper bound on the population target (live surfels): kappa x visible tiles is capped here.
	uint32_t spawnPopulationCap = 2048u;
	uint32_t maxProjectedSurfelsPerFrame = 24576u;
	uint32_t maxRecycleCountPerFrame = 256u;

	float targetSurfelScreenRadiusPx = 8.0f;
	float minSurfelRadius = 0.03f;
	float maxSurfelRadius = 5.0f;
	float spawnCoverageThreshold = 0.85f;
	float stationaryCameraEpsilon = 0.0025f;
	float recyclePressureStart = 0.85f;
	float normalRejectCos = 0.25f;
	float coverageNormalCos = 0.86f;
	float coverageDepthTolerance = 0.0035f;
	float finalGatherNormalCos = 0.15f;
	float radialDepthSigmaScale = 1.0f;
	float indirectIntensity = 1.0f;
	float cellAverageFallbackStrength = 1.0f;
	// Gather kernel cutoff in receiver base-cell sizes (0 = off, the tail weights stay; default 1, plan section 21). A surfel beyond the cutoff gets
	// zero weight, with a (1 - x^2)^2 taper, so the 3x3x3 window stops changing with the receiver's cell (plan section 21).
	float gatherCutoffCells = 1.0f;
	// Cell-average fallback interpolated from the 8 corner cells of the receiver's position (true) or the base cell's
	// constant (false, the GIBS form). Plan section 21.
	bool gatherSmoothFallback = true;
	// Stochastic per-cell gather (plan section 21, round 2): samples per cell in the window, hashed start per pixel and frame.
	bool gatherStochastic = false;
	uint32_t gatherPerCellSamples = 2u;
	float skyMissRadianceMultiplier = 1.0f;
	float finalGatherResolutionScale = 0.5f;

	bool useNonLinearGrid = true;
	bool useRadialDepth = true;
	bool useRayGuiding = true;
	bool useIrradianceSharing = true;
	bool useScreenSpaceTrace = true;
	bool useSoftwareBVHTrace = true;
	bool useSurfelFallbackTrace = true;
	bool placementValidationMode = false;
	// NOX_GI_REFERENCE=1: independent path-traced diffuse GI replaces the surfel irradiance (debug; off by default).
	bool referenceMode = false;
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
	uint32_t overCoverageRecycled = 0;
	uint32_t staleRecycled = 0;
	uint32_t pressureRecycled = 0;
	uint32_t underCoveredTileCount = 0;
	uint32_t highPriorityTileCount = 0;
	uint32_t coverageSpawnedTileCount = 0;
	uint32_t coverageVisibleTileCount = 0;
	uint32_t coverageInvalidTileCount = 0;
	uint32_t projectedSurfels = 0;
	uint32_t coverageDepthRejected = 0;
	uint32_t coverageNormalRejected = 0;
	uint32_t coverageMaterialRejected = 0;
	uint32_t coverageRadiusRejected = 0;
	uint32_t rayHitGrid = 0;
	uint32_t rayHitScreen = 0;
	uint32_t rayHitBVH = 0;
	uint32_t rayHitEnvironment = 0;
	uint32_t rayHitMiss = 0;
	uint32_t acceptedRaySamples = 0;
	uint32_t temporalClampEvents = 0;
	uint32_t sharingAppliedSurfels = 0;
	uint32_t sharingRejectedDarkLift = 0;
	uint32_t finalGatherDirectPixels = 0;
	uint32_t finalGatherFallbackPixels = 0;
	uint32_t finalGatherBleedPixels = 0;
	uint32_t finalGatherValidPixels = 0;
	uint32_t finalGatherEmptyPixels = 0;
	// Debug counters (per frame, read back through the async counter path).
	uint32_t youngSurfels = 0;
	uint32_t rayShortfall = 0;
	uint32_t staleRaySlots = 0;
	uint32_t lookupsSharing = 0;
	uint32_t entriesSharing = 0;
	uint32_t lookupsGather = 0;
	uint32_t entriesGather = 0;
	uint32_t lookupsTrace = 0;
	uint32_t entriesTrace = 0;
	uint32_t lookupsSpawn = 0;
	uint32_t entriesSpawn = 0;
	uint32_t recycleOverTarget = 0;
	uint32_t recycleStale = 0;
	uint32_t recycleFar = 0;
	uint32_t recycleForced = 0;
	uint32_t spawnDecisions = 0;
	uint32_t spawnBudgetDenied = 0;
	uint32_t radiusCapped = 0;
	uint32_t recycledYoung = 0;
	// World surfels (plan section 17), per frame unless noted: requests queued, requests dropped at the queue cap,
	// world surfels spawned, requests skipped because the coverage already held, live world surfels (persistent) and
	// counted this frame, and path cache lookups that hit or missed.
	uint32_t worldRequests = 0;
	uint32_t worldRequestDropped = 0;
	uint32_t worldSpawned = 0;
	uint32_t worldSkippedCovered = 0;
	uint32_t liveWorld = 0;
	uint32_t liveWorldCounted = 0;
	uint32_t cacheHits = 0;
	uint32_t cacheMisses = 0;
	// World surfel ray and sample accounting (per frame): rays requested and granted to world surfels, the sums of their
	// ages and sample histories (divide by liveWorldCounted for the means).
	uint32_t requestedWorld = 0;
	uint32_t allocatedWorld = 0;
	uint32_t worldAgeSum = 0;
	uint32_t worldHistorySum = 0;
	// RT scene update of the latest frame (CPU ms and upload flags from RTSceneResources).
	float rtInstanceListMs = 0.0f;
	float rtUploadMs = 0.0f;
	uint32_t rtUploadRan = 0;
	uint32_t rtGeometryUploaded = 0;
	uint32_t rtInstanceBufferRewritten = 0;
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

	rhi::TextureHandle GetIndirectTexture() const;

private:
	bool EnsurePipeline(RenderContext& context);
	bool HasReadyPipeline() const;

	std::unique_ptr<SurfelGIPipeline> m_pipeline;
	SurfelGISettings m_settings{};
	SurfelGIFrameStats m_stats{};
	bool m_initialized = false;
};
