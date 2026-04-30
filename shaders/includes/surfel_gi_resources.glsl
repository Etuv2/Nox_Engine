#ifndef SURFEL_GI_RESOURCES_GLSL
#define SURFEL_GI_RESOURCES_GLSL

#define SURFEL_GI_INVALID_INDEX 0xffffffffu

#define B_SURFELS 10
#define B_SURFEL_FREELIST 11
#define B_SURFEL_COUNTERS 12
#define B_GRID_HEADERS 13
#define B_GRID_ENTRIES 14
#define B_RAY_REQUESTS 15
#define B_RAYS 16
#define B_RAYS_SORTED 17
#define B_RAY_HITS 18
#define B_RADIAL_DEPTH 19
#define B_GUIDE_MAP 20
#define B_GUIDE_SCALE 21
#define B_TRANSFORMS 22
#define B_LIGHTS 23
#define B_BVH_NODES 24
#define B_BVH_TRIANGLES 25
#define B_GRID_AVERAGES 26
#define B_RAY_BINS 27
#define B_RAY_COUNTERS 28
#define B_GRID_COUNTERS 29
#define B_SURFEL_GI_SETTINGS 30

#define SURFEL_DEAD 0u
#define SURFEL_ALIVE (1u << 0)
#define SURFEL_NEW (1u << 1)
#define SURFEL_DYNAMIC (1u << 2)
#define SURFEL_SKINNED (1u << 3)
#define SURFEL_EMISSIVE (1u << 4)
#define SURFEL_INVALID (1u << 31)

struct Surfel {
    vec4 worldPos_radius;
    vec4 worldNormal_age;
    vec4 localPos_spawnRadius;
    vec4 localNormal_flags;
    uvec4 ids;
    vec4 albedo_life;
    vec4 irradiance;
    vec4 shortMean;
    vec4 shortM2;
    uvec4 frameInfo;
    vec4 debug;
};

struct SurfelCounters {
    uint freeTop;
    uint liveCount;
    uint spawnedThisFrame;
    uint recycledThisFrame;
    uint requestedRays;
    uint allocatedRays;
    uint overflowSurfels;
    uint overflowGridEntries;
};

struct SurfelGISettingsGpu {
    uvec4 budgets;
    uvec4 grid;
    vec4 radius;
    vec4 thresholds;
    uvec4 toggles;
};

struct SurfelCellHeader {
    uint firstEntry;
    uint entryCount;
    uint averagePackedIrradiance;
    uint flags;
};

struct SurfelCellEntry {
    uint surfelID;
    uint next;
};

struct SurfelGridCellAverage {
    vec4 irradianceWeight;
    vec4 normalCount;
};

struct SurfelGridCounters {
    uint cellCount;
    uint entryCapacity;
    uint overflowEntries;
    uint _pad0;
};

struct SurfelRayRequest {
    uint surfelID;
    uint requestedCount;
    uint allocatedCount;
    uint firstRay;
};

struct SurfelRay {
    vec4 origin_tMin;
    vec4 direction_tMax;
    uvec4 ids;
    vec4 throughput;
};

struct SurfelRayHit {
    vec4 position_t;
    vec4 normal_hitKind;
    vec4 radiance_pdf;
    uvec4 ids;
};

struct SurfelRayCounters {
    uint requestedRays;
    uint allocatedRays;
    uint emittedRays;
    uint hitCount;
    uint maxRays;
    uint maxSurfels;
    uint globalBudget;
    uint overflowRays;
};

#endif
