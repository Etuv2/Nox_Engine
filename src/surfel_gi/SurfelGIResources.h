#pragma once

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>

struct SurfelGridSettings {
    glm::uvec3 resolution{ 32u, 32u, 24u };
    uint32_t maxSurfelsPerCell = 64u;
    glm::vec3 worldExtent{ 24.0f };
    float nearScale = 1.0f;
    float farScale = 120.0f;
    bool useNonLinearGrid = true;
    uint32_t centralResolution = 32u;
    uint32_t axisLateralResolution = 32u;
    uint32_t axisSliceCount = 24u;
    float centralHalfExtent = 12.0f;
    float sliceGrowth = 1.22f;

    uint64_t CellCount() const
    {
        if (!useNonLinearGrid) {
            return static_cast<uint64_t>(resolution.x) *
                static_cast<uint64_t>(resolution.y) *
                static_cast<uint64_t>(resolution.z);
        }

        const uint64_t central = static_cast<uint64_t>(centralResolution) *
            static_cast<uint64_t>(centralResolution) *
            static_cast<uint64_t>(centralResolution);
        const uint64_t axisRegion = static_cast<uint64_t>(axisLateralResolution) *
            static_cast<uint64_t>(axisLateralResolution) *
            static_cast<uint64_t>(axisSliceCount);
        return central + 6ull * axisRegion;
    }
};

enum SurfelFlags : uint32_t {
    SURFEL_DEAD = 0u,
    SURFEL_ALIVE = 1u << 0,
    SURFEL_NEW = 1u << 1,
    SURFEL_DYNAMIC = 1u << 2,
    SURFEL_SKINNED = 1u << 3,
    SURFEL_EMISSIVE = 1u << 4,
    SURFEL_INVALID = 1u << 31
};

enum class SurfelGIBinding : GLuint {
    Surfels = 10u,
    SurfelFreeList = 11u,
    SurfelCounters = 12u,
    GridHeaders = 13u,
    GridEntries = 14u,
    RayRequests = 15u,
    Rays = 16u,
    SortedRays = 17u,
    RayHits = 18u,
    RadialDepth = 19u,
    GuideMap = 20u,
    GuideScale = 21u,
    Transforms = 22u,
    Lights = 23u,
    BvhNodes = 24u,
    BvhTriangles = 25u,
    GridAverages = 26u,
    RayBins = 27u,
    RayCounters = 28u,
    GridCounters = 29u,
    Settings = 30u,
    ShadowMatrices = 31u,
    CoverageTiles = 32u
};

constexpr GLuint ToGLuint(SurfelGIBinding binding)
{
    return static_cast<GLuint>(binding);
}

static constexpr uint32_t kInvalidIndex = 0xffffffffu;

struct alignas(16) Surfel {
    glm::vec4 worldPos_radius{ 0.0f };
    glm::vec4 worldNormal_age{ 0.0f };
    glm::vec4 localPos_spawnRadius{ 0.0f };
    glm::vec4 localNormal_flags{ 0.0f };
    glm::uvec4 ids{ 0u };
    glm::vec4 albedo_life{ 0.0f };
    glm::vec4 irradiance{ 0.0f };
    glm::vec4 shortMean{ 0.0f };
    glm::vec4 shortM2{ 0.0f };
    glm::uvec4 frameInfo{ 0u };
    glm::vec4 debug{ 0.0f };
};
static_assert(sizeof(Surfel) == 176, "Surfel must match std430 GLSL layout.");
static_assert(offsetof(Surfel, worldPos_radius) == 0, "Surfel.worldPos_radius offset mismatch.");
static_assert(offsetof(Surfel, worldNormal_age) == 16, "Surfel.worldNormal_age offset mismatch.");
static_assert(offsetof(Surfel, localPos_spawnRadius) == 32, "Surfel.localPos_spawnRadius offset mismatch.");
static_assert(offsetof(Surfel, localNormal_flags) == 48, "Surfel.localNormal_flags offset mismatch.");
static_assert(offsetof(Surfel, ids) == 64, "Surfel.ids offset mismatch.");
static_assert(offsetof(Surfel, albedo_life) == 80, "Surfel.albedo_life offset mismatch.");
static_assert(offsetof(Surfel, irradiance) == 96, "Surfel.irradiance offset mismatch.");
static_assert(offsetof(Surfel, shortMean) == 112, "Surfel.shortMean offset mismatch.");
static_assert(offsetof(Surfel, shortM2) == 128, "Surfel.shortM2 offset mismatch.");
static_assert(offsetof(Surfel, frameInfo) == 144, "Surfel.frameInfo offset mismatch.");
static_assert(offsetof(Surfel, debug) == 160, "Surfel.debug offset mismatch.");

struct alignas(16) SurfelCounters {
    uint32_t freeTop = 0u;
    uint32_t liveCount = 0u;
    uint32_t spawnedThisFrame = 0u;
    uint32_t recycledThisFrame = 0u;
    uint32_t requestedRays = 0u;
    uint32_t allocatedRays = 0u;
    uint32_t overflowSurfels = 0u;
    uint32_t overflowGridEntries = 0u;
    uint32_t rejectedInvalidDepth = 0u;
    uint32_t rejectedInvalidTransform = 0u;
    uint32_t rejectedInvalidMaterial = 0u;
    uint32_t rejectedOutsideGrid = 0u;
    uint32_t rejectedInvalidWorldPos = 0u;
    uint32_t rejectedInvalidNormal = 0u;
    uint32_t rejectedInvalidRadius = 0u;
    uint32_t rejectedPoolFull = 0u;
    uint32_t overCoverageRecycled = 0u;
    uint32_t staleRecycled = 0u;
    uint32_t pressureRecycled = 0u;
    uint32_t underCoveredTileCount = 0u;
    uint32_t highPriorityTileCount = 0u;
    uint32_t coverageSpawnedTileCount = 0u;
    uint32_t coverageVisibleTileCount = 0u;
    uint32_t coverageInvalidTileCount = 0u;
};
static_assert(sizeof(SurfelCounters) == 96, "SurfelCounters must match std430 GLSL layout.");

enum SurfelCoverageTileFlags : uint32_t {
    SURFEL_COVERAGE_TILE_VISIBLE = 1u << 0,
    SURFEL_COVERAGE_TILE_UNDER_COVERED = 1u << 1,
    SURFEL_COVERAGE_TILE_HIGH_PRIORITY = 1u << 2,
    SURFEL_COVERAGE_TILE_SPAWNED_RECENTLY = 1u << 3
};

struct alignas(16) SurfelCoverageTile {
    glm::vec4 lowestCoverage{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::uvec4 state{ 0u };
    glm::uvec4 pixel{ 0u };
};
static_assert(sizeof(SurfelCoverageTile) == 48, "SurfelCoverageTile must match std430 GLSL layout.");

struct alignas(16) SurfelGISettingsGpu {
    glm::uvec4 budgets{ 0u };
    glm::uvec4 grid{ 0u };
    glm::vec4 radius{ 0.0f };
    glm::vec4 thresholds{ 0.0f };
    glm::uvec4 toggles{ 0u };
};
static_assert(sizeof(SurfelGISettingsGpu) == 80, "SurfelGISettingsGpu must match std430 GLSL layout.");

struct alignas(16) SurfelCellHeader {
    uint32_t firstEntry = kInvalidIndex;
    uint32_t entryCount = 0u;
    uint32_t averagePackedIrradiance = 0u;
    uint32_t flags = 0u;
};
static_assert(sizeof(SurfelCellHeader) == 16, "SurfelCellHeader must match std430 GLSL layout.");

struct SurfelCellEntry {
    uint32_t surfelID = kInvalidIndex;
    uint32_t next = kInvalidIndex;
};
static_assert(sizeof(SurfelCellEntry) == 8, "SurfelCellEntry must match std430 GLSL layout.");

struct alignas(16) SurfelGridCellAverage {
    glm::vec4 irradianceWeight{ 0.0f };
    glm::vec4 normalCount{ 0.0f };
};
static_assert(sizeof(SurfelGridCellAverage) == 32, "SurfelGridCellAverage must match std430 GLSL layout.");

struct alignas(16) SurfelGridCounters {
    uint32_t cellCount = 0u;
    uint32_t entryCapacity = 0u;
    uint32_t overflowEntries = 0u;
    uint32_t _pad0 = 0u;
};
static_assert(sizeof(SurfelGridCounters) == 16, "SurfelGridCounters must match std430 GLSL layout.");

struct alignas(16) SurfelRayRequest {
    uint32_t surfelID = kInvalidIndex;
    uint32_t requestedCount = 0u;
    uint32_t allocatedCount = 0u;
    uint32_t firstRay = kInvalidIndex;
};
static_assert(sizeof(SurfelRayRequest) == 16, "SurfelRayRequest must match std430 GLSL layout.");

struct alignas(16) SurfelRay {
    glm::vec4 origin_tMin{ 0.0f };
    glm::vec4 direction_tMax{ 0.0f };
    glm::uvec4 ids{ 0u };
    glm::vec4 throughput{ 0.0f };
};
static_assert(sizeof(SurfelRay) == 64, "SurfelRay must match std430 GLSL layout.");

struct alignas(16) SurfelRayHit {
    glm::vec4 position_t{ 0.0f };
    glm::vec4 normal_hitKind{ 0.0f };
    glm::vec4 radiance_pdf{ 0.0f };
    glm::uvec4 ids{ 0u };
};
static_assert(sizeof(SurfelRayHit) == 64, "SurfelRayHit must match std430 GLSL layout.");

struct alignas(16) SurfelRayCounters {
    uint32_t requestedRays = 0u;
    uint32_t allocatedRays = 0u;
    uint32_t emittedRays = 0u;
    uint32_t hitCount = 0u;
    uint32_t maxRays = 0u;
    uint32_t maxSurfels = 0u;
    uint32_t globalBudget = 0u;
    uint32_t overflowRays = 0u;
};
static_assert(sizeof(SurfelRayCounters) == 32, "SurfelRayCounters must match std430 GLSL layout.");

struct TraceResources {
    GLuint bvhNodeBuffer = 0;
    GLuint bvhTriangleBuffer = 0;
    GLuint lightBuffer = 0;
    GLuint transformBuffer = 0;
};

class SurfelPool {
public:
    SurfelPool();
    ~SurfelPool();

    SurfelPool(const SurfelPool&) = delete;
    SurfelPool& operator=(const SurfelPool&) = delete;

    bool Create(uint32_t maxSurfels);
    bool ResizeReset(uint32_t maxSurfels);
    void Destroy();
    void ResetFreeList();

    GLuint GetSurfelBuffer() const { return m_surfelBuffer; }
    GLuint GetFreeListBuffer() const { return m_freeListBuffer; }
    GLuint GetCountersBuffer() const { return m_countersBuffer; }
    uint32_t GetMaxSurfels() const { return m_maxSurfels; }
    bool IsCreated() const { return m_surfelBuffer != 0 && m_freeListBuffer != 0 && m_countersBuffer != 0; }

private:
    GLuint m_surfelBuffer = 0;
    GLuint m_freeListBuffer = 0;
    GLuint m_countersBuffer = 0;
    uint32_t m_maxSurfels = 0;
};

class SurfelGrid {
public:
    SurfelGrid();
    ~SurfelGrid();

    SurfelGrid(const SurfelGrid&) = delete;
    SurfelGrid& operator=(const SurfelGrid&) = delete;

    bool Create(const SurfelGridSettings& settings);
    bool ResizeReset(const SurfelGridSettings& settings);
    void Destroy();
    void Clear();
    void Build(GLuint surfelBuffer, uint32_t maxSurfels);
    void BuildCellAverages();

    GLuint GetCellHeaderBuffer() const { return m_cellHeaderBuffer; }
    GLuint GetCellEntryBuffer() const { return m_cellEntryBuffer; }
    GLuint GetCellAverageBuffer() const { return m_cellAverageBuffer; }
    GLuint GetCountersBuffer() const { return m_countersBuffer; }
    uint32_t GetCellCount() const { return m_cellCount; }
    uint32_t GetEntryCapacity() const { return m_entryCapacity; }
    const SurfelGridSettings& GetSettings() const { return m_settings; }
    bool IsCreated() const { return m_cellHeaderBuffer != 0 && m_cellEntryBuffer != 0 && m_countersBuffer != 0; }

private:
    GLuint m_cellHeaderBuffer = 0;
    GLuint m_cellEntryBuffer = 0;
    GLuint m_cellAverageBuffer = 0;
    GLuint m_countersBuffer = 0;
    uint32_t m_cellCount = 0;
    uint32_t m_entryCapacity = 0;
    SurfelGridSettings m_settings{};
};

class SurfelRayQueue {
public:
    SurfelRayQueue();
    ~SurfelRayQueue();

    SurfelRayQueue(const SurfelRayQueue&) = delete;
    SurfelRayQueue& operator=(const SurfelRayQueue&) = delete;

    bool Create(uint32_t maxRays, uint32_t maxSurfels);
    bool ResizeReset(uint32_t maxRays, uint32_t maxSurfels);
    void Destroy();
    void Clear();
    void RequestRays(GLuint surfelBuffer);
    void AllocateRays(GLuint surfelBuffer, uint32_t globalBudget);
    void GenerateRays(GLuint surfelBuffer, GLuint gridBuffer);
    void BinAndSortRays();
    void TraceRays(const TraceResources& traceResources);
    void IntegrateHits(GLuint surfelBuffer);

    GLuint GetRequestBuffer() const { return m_requestBuffer; }
    GLuint GetRayBuffer() const { return m_rayBuffer; }
    GLuint GetSortedRayBuffer() const { return m_sortedRayBuffer; }
    GLuint GetRayHitBuffer() const { return m_rayHitBuffer; }
    GLuint GetRayBinBuffer() const { return m_rayBinBuffer; }
    GLuint GetCountersBuffer() const { return m_countersBuffer; }
    uint32_t GetMaxRays() const { return m_maxRays; }
    uint32_t GetMaxSurfels() const { return m_maxSurfels; }
    bool IsCreated() const { return m_requestBuffer != 0 && m_rayBuffer != 0 && m_rayHitBuffer != 0 && m_countersBuffer != 0; }

private:
    GLuint m_requestBuffer = 0;
    GLuint m_rayBuffer = 0;
    GLuint m_sortedRayBuffer = 0;
    GLuint m_rayHitBuffer = 0;
    GLuint m_rayBinBuffer = 0;
    GLuint m_countersBuffer = 0;
    uint32_t m_maxRays = 0;
    uint32_t m_maxSurfels = 0;
};
