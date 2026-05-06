#ifndef NOX_SURFEL_GI_COMMON_GLSL
#define NOX_SURFEL_GI_COMMON_GLSL

#define B_SURFELS 10
#define B_SURFEL_FREELIST 11
#define B_SURFEL_COUNTERS 12
#define B_GRID_HEADERS 13
#define B_GRID_ENTRIES 14
#define B_RAY_REQUESTS 15
#define B_RAYS 16
#define B_RAYS_IN 16
#define B_RAYS_SORTED 17
#define B_RAY_HITS 18
#define B_RADIAL_DEPTH 19
#define B_GUIDE_MAP 20
#define B_GUIDE_SCALE 21
#define B_IRRADIANCE_SNAPSHOT 21
#define B_TRANSFORMS 22
#define B_LIGHTS 23
#define B_BVH_NODES 24
#define B_BVH_TRIANGLES 25
#define B_GRID_AVERAGES 26
#define B_RAY_BINS 27
#define B_RAY_COUNTERS 28
#define B_GRID_COUNTERS 29
#define B_SURFEL_GI_SETTINGS 30
#define B_SHADOW_MATRICES 31
#define B_COVERAGE_TILES 32
#define B_RT_INSTANCES 33
#define B_RT_INSTANCE_NODES 34

#define T_SURFEL_GBUFFER_NORMAL_RM 0
#define T_SURFEL_GBUFFER_ALBEDO_AO 1
#define T_SURFEL_GBUFFER_MATERIAL_ID 2
#define T_SURFEL_GBUFFER_EMISSIVE 3
#define T_SURFEL_GBUFFER_TRANSFORM_ID 4
#define T_SURFEL_GBUFFER_DEPTH 5

#include "../includes/transform_tracking_contract.glsl"

#define SURFEL_DEAD 0x00000000u
#define SURFEL_ALIVE 0x00000001u
#define SURFEL_NEW 0x00000002u
#define SURFEL_DYNAMIC 0x00000004u
#define SURFEL_SKINNED 0x00000008u
#define SURFEL_EMISSIVE 0x00000010u
#define SURFEL_INVALID 0x80000000u

#define GPU_TRANSFORM_INVALID 0x80000000u

#define SURFEL_CELL_HAS_AVERAGE 0x00000001u
#define SURFEL_CELL_OVERFLOWED 0x00000002u
#define SURFEL_GRID_REGION_CENTRAL 0u
#define SURFEL_GRID_REGION_POS_X 1u
#define SURFEL_GRID_REGION_NEG_X 2u
#define SURFEL_GRID_REGION_POS_Y 3u
#define SURFEL_GRID_REGION_NEG_Y 4u
#define SURFEL_GRID_REGION_POS_Z 5u
#define SURFEL_GRID_REGION_NEG_Z 6u

#define SURFEL_COVERAGE_TILE_VISIBLE (1u << 0)
#define SURFEL_COVERAGE_TILE_UNDER_COVERED (1u << 1)
#define SURFEL_COVERAGE_TILE_HIGH_PRIORITY (1u << 2)
#define SURFEL_COVERAGE_TILE_SPAWNED_RECENTLY (1u << 3)
#define SURFEL_COVERAGE_TILE_FINAL_GI_VALID (1u << 4)

const uint SURFEL_RADIAL_DEPTH_TEXELS = 16u;
const uint SURFEL_GUIDE_CELLS = 36u;
const uint SURFEL_INVALID_INDEX = 0xffffffffu;
const uint SURFEL_SPAWN_TILE_CANDIDATES = 8u;
const float SURFEL_PI = 3.14159265358979323846;
const float SURFEL_RADIAL_DEPTH_MIN_VISIBILITY = 0.05;

struct Surfel
{
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
    uvec4 lifecycle;
    vec4 debug;
};

struct SurfelCounters
{
    uint freeTop;
    uint liveCount;
    uint spawnedThisFrame;
    uint recycledThisFrame;
    uint requestedRays;
    uint allocatedRays;
    uint overflowSurfels;
    uint overflowGridEntries;
    uint rejectedInvalidDepth;
    uint rejectedInvalidTransform;
    uint rejectedInvalidMaterial;
    uint rejectedOutsideGrid;
    uint rejectedInvalidWorldPos;
    uint rejectedInvalidNormal;
    uint rejectedInvalidRadius;
    uint rejectedPoolFull;
    uint overCoverageRecycled;
    uint staleRecycled;
    uint pressureRecycled;
    uint underCoveredTileCount;
    uint highPriorityTileCount;
    uint coverageSpawnedTileCount;
    uint coverageVisibleTileCount;
    uint coverageInvalidTileCount;
    uint projectedSurfels;
    uint coverageDepthRejected;
    uint coverageNormalRejected;
    uint coverageMaterialRejected;
    uint coverageRadiusRejected;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

struct SurfelCoverageTile
{
    vec4 lowestCoverage;
    uvec4 state;
    uvec4 pixel;
    uvec4 projection;
};

struct RadialDepthTexel
{
    float meanDepth;
    float meanDepthSq;
};

struct GuideCell
{
    uint packedRadiance;
};

struct SurfelCellHeader
{
    uint firstEntry;
    uint entryCount;
    uint averagePackedIrradiance;
    uint flags;
};

struct SurfelCellEntry
{
    uint surfelID;
    uint next;
};

struct SurfelRayRequest
{
    uint surfelID;
    uint requestedCount;
    uint allocatedCount;
    uint firstRay;
};

struct SurfelRay
{
    vec4 origin_tMin;
    vec4 direction_tMax;
    uvec4 ids;
    vec4 throughput;
};

struct SurfelRayHit
{
    vec4 position_t;
    vec4 normal_hitKind;
    vec4 radiance_pdf;
    uvec4 ids;
};

struct SurfelGridAddress
{
    uint cell;
    uint region;
    uvec3 coord;
    vec3 center;
    float cellSize;
};

float SurfelSaturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

vec3 SurfelSafeNormalize(vec3 value)
{
    float lenSq = dot(value, value);
    if (lenSq <= 1e-12 || any(isnan(value)) || any(isinf(value))) {
        return vec3(0.0, 1.0, 0.0);
    }
    return value * inversesqrt(lenSq);
}

uint SurfelFlags(Surfel surfel)
{
    return floatBitsToUint(surfel.localNormal_flags.w);
}

void SurfelSetFlags(inout Surfel surfel, uint flags)
{
    surfel.localNormal_flags.w = uintBitsToFloat(flags);
}

bool SurfelIsAlive(Surfel surfel)
{
    uint flags = SurfelFlags(surfel);
    return (flags & SURFEL_ALIVE) != 0u && (flags & SURFEL_INVALID) == 0u;
}

bool SurfelIsNew(Surfel surfel)
{
    return (SurfelFlags(surfel) & SURFEL_NEW) != 0u;
}

uvec4 SurfelLifecycleCounters(uint failedValidationCount,
                              uint successfulValidationCount,
                              uint lowConfidenceFrameCount,
                              uint recentReuseScore)
{
    return uvec4(failedValidationCount,
                 successfulValidationCount,
                 lowConfidenceFrameCount,
                 recentReuseScore);
}

void SurfelMarkDead(inout Surfel surfel)
{
    SurfelSetFlags(surfel, SURFEL_DEAD);
    surfel.worldPos_radius.w = 0.0;
    surfel.albedo_life.a = 0.0;
}

float SurfelLuminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

uint SurfelHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

const uint SURFEL_SPAWN_TILE_STRATA_X = 4u;
const uint SURFEL_SPAWN_TILE_STRATA_Y = 4u;
const vec2 SURFEL_SPAWN_TILE_R2_STEP = vec2(0.754877666, 0.569840296);

vec2 SurfelSpawnTileStratum(uvec2 tile,
                            uint frameIndex,
                            uint passIndex,
                            uint passCount,
                            uint sampleIndex)
{
    uint tileSeed = tile.x * 0x8da6b343u ^
        tile.y * 0xd8163841u ^
        (frameIndex & 31u) * 0xcb1ab31fu ^
        passIndex * 0x165667b1u ^
        passCount * 0x27d4eb2du;
    // Cranley-Patterson rotate an R2 sequence per tile/frame/pass. This keeps
    // candidates stable and well-spaced without locking them to repeated tile
    // centers or a tiny set of horizontal/vertical sub-strata.
    vec2 rotation = vec2(
        float(SurfelHash(tileSeed) & 0x00ffffffu) / 16777215.0,
        float(SurfelHash(tileSeed ^ 0x68bc21ebu) & 0x00ffffffu) / 16777215.0);
    float sequenceIndex = float(sampleIndex) +
        float((SurfelHash(tileSeed ^ 0x9e3779b9u) & 15u));
    return clamp(fract(rotation + SURFEL_SPAWN_TILE_R2_STEP * sequenceIndex),
                 vec2(0.035),
                 vec2(0.965));
}

uvec2 SurfelSpawnTilePixel(uvec2 tile,
                           uvec2 tileExtent,
                           uint frameIndex,
                           uint passIndex,
                           uint passCount,
                           uint sampleIndex)
{
    vec2 extent = max(vec2(tileExtent), vec2(1.0));
    vec2 stratum = SurfelSpawnTileStratum(tile, frameIndex, passIndex, passCount, sampleIndex);
    uint seed = tile.x * 0x8da6b343u ^
        tile.y * 0xd8163841u ^
        frameIndex * 0xcb1ab31fu ^
        (passIndex + passCount * 17u) * 0x165667b1u ^
        sampleIndex * 0x9e3779b9u;
    vec2 jitter01 = vec2(
        float(SurfelHash(seed) & 0x00ffffffu),
        float(SurfelHash(seed ^ 0x68bc21ebu) & 0x00ffffffu)) / 16777215.0;
    vec2 jitter = (jitter01 - vec2(0.5)) * min(extent, vec2(2.0)) * 0.70;
    vec2 pixel = clamp(stratum * extent + jitter,
                       vec2(0.0),
                       max(extent - vec2(0.001), vec2(0.0)));
    return min(uvec2(pixel), tileExtent - uvec2(1u));
}

vec3 SurfelReconstructWorldPosition(vec2 uv, float depth01, mat4 invProjection, mat4 invView)
{
    vec4 clipPosition = vec4(uv * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
    vec4 viewPosition = invProjection * clipPosition;
    float invW = 1.0 / max(abs(viewPosition.w), 1e-6);
    viewPosition.xyz *= invW;
    viewPosition.w = 1.0;
    vec4 worldPosition = invView * viewPosition;
    return worldPosition.xyz;
}

float SurfelHash01(uint value)
{
    return float(SurfelHash(value) & 0x00ffffffu) / 16777215.0;
}

void SurfelBuildBasis(vec3 normal, out vec3 tangent, out vec3 bitangent)
{
    vec3 n = SurfelSafeNormalize(normal);
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    tangent = SurfelSafeNormalize(cross(up, n));
    bitangent = cross(n, tangent);
}

vec3 SurfelCosineHemisphereSample(float u1, float u2)
{
    float r = sqrt(max(u1, 0.0));
    float phi = 2.0 * SURFEL_PI * u2;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(1.0 - u1, 0.0));
    return vec3(x, y, z);
}

vec3 SurfelHemiToWorld(vec3 hemiDir, vec3 normal)
{
    vec3 tangent;
    vec3 bitangent;
    vec3 n = SurfelSafeNormalize(normal);
    SurfelBuildBasis(n, tangent, bitangent);
    return SurfelSafeNormalize(tangent * hemiDir.x + bitangent * hemiDir.y + n * max(hemiDir.z, 0.0));
}

uint SurfelRadialDepthBin(vec3 hemiDir)
{
    vec2 uv = clamp(hemiDir.xy * 0.5 + 0.5, vec2(0.0), vec2(0.999));
    uvec2 texel = uvec2(floor(uv * 4.0));
    return min(texel.y * 4u + texel.x, SURFEL_RADIAL_DEPTH_TEXELS - 1u);
}

vec3 SurfelWorldToHemiDir(vec3 worldDir, vec3 normal)
{
    vec3 tangent;
    vec3 bitangent;
    vec3 n = SurfelSafeNormalize(normal);
    SurfelBuildBasis(n, tangent, bitangent);
    vec3 dir = SurfelSafeNormalize(worldDir);
    return vec3(dot(dir, tangent), dot(dir, bitangent), max(dot(dir, n), 0.0));
}

uint SurfelRadialDepthIndex(uint surfelID, vec3 surfelToReceiver, vec3 surfelNormal)
{
    return surfelID * SURFEL_RADIAL_DEPTH_TEXELS +
        SurfelRadialDepthBin(SurfelWorldToHemiDir(surfelToReceiver, surfelNormal));
}

bool SurfelRadialDepthMomentsValid(RadialDepthTexel texel)
{
    return texel.meanDepth > 0.0 &&
        texel.meanDepthSq > 0.0 &&
        !isnan(texel.meanDepth) &&
        !isnan(texel.meanDepthSq) &&
        !isinf(texel.meanDepth) &&
        !isinf(texel.meanDepthSq);
}

float SurfelRadialDepthVisibility(RadialDepthTexel texel, float receiverDistance, float minVariance)
{
    if (!SurfelRadialDepthMomentsValid(texel) ||
        receiverDistance <= 0.0 ||
        isnan(receiverDistance) ||
        isinf(receiverDistance)) {
        return 1.0;
    }

    float mean = max(texel.meanDepth, 0.0);
    if (receiverDistance <= mean) {
        return 1.0;
    }

    float variance = max(texel.meanDepthSq - mean * mean, max(minVariance, 1e-6));
    float delta = receiverDistance - mean;
    float pMax = variance / max(variance + delta * delta, 1e-6);
    float visibility = smoothstep(0.05, 1.0, SurfelSaturate(pMax));
    return mix(SURFEL_RADIAL_DEPTH_MIN_VISIBILITY, 1.0, visibility);
}

uint SurfelGuideBin(vec3 hemiDir)
{
    vec2 uv = clamp(hemiDir.xy * 0.5 + 0.5, vec2(0.0), vec2(0.999));
    uvec2 texel = uvec2(floor(uv * 6.0));
    return min(texel.y * 6u + texel.x, SURFEL_GUIDE_CELLS - 1u);
}

uint SurfelPackIrradiance(vec3 irradiance, float confidence)
{
    vec3 mapped = max(irradiance, vec3(0.0));
    mapped = mapped / (mapped + vec3(1.0));
    return packUnorm4x8(vec4(mapped, SurfelSaturate(confidence)));
}

uint SurfelCentralGridResolution(uvec3 gridResolution)
{
    return max(gridResolution.x, 1u);
}

uint SurfelAxisLateralResolution(uvec3 gridResolution)
{
    return max(gridResolution.y, 1u);
}

uint SurfelAxisSliceCount(uvec3 gridResolution)
{
    return max(gridResolution.z, 1u);
}

uint SurfelNonLinearCentralCellCount(uvec3 gridResolution)
{
    uint c = SurfelCentralGridResolution(gridResolution);
    return c * c * c;
}

uint SurfelNonLinearAxisCellsPerRegion(uvec3 gridResolution)
{
    uint lateral = SurfelAxisLateralResolution(gridResolution);
    return lateral * lateral * SurfelAxisSliceCount(gridResolution);
}

uint SurfelNonLinearGridCellCount(uvec3 gridResolution)
{
    return SurfelNonLinearCentralCellCount(gridResolution) +
        6u * SurfelNonLinearAxisCellsPerRegion(gridResolution);
}

uint SurfelFlattenCell(uvec3 cell, uvec3 gridResolution)
{
    uvec3 dims = max(gridResolution, uvec3(1u));
    uvec3 c = clamp(cell, uvec3(0u), dims - uvec3(1u));
    return (c.z * dims.y + c.y) * dims.x + c.x;
}

uint SurfelFlattenCentralCell(uvec3 cell, uint centralResolution)
{
    uint r = max(centralResolution, 1u);
    uvec3 c = clamp(cell, uvec3(0u), uvec3(r - 1u));
    return (c.z * r + c.y) * r + c.x;
}

uint SurfelAxisRegionIndex(uint axis, bool positive)
{
    if (axis == 0u) {
        return positive ? SURFEL_GRID_REGION_POS_X : SURFEL_GRID_REGION_NEG_X;
    }
    if (axis == 1u) {
        return positive ? SURFEL_GRID_REGION_POS_Y : SURFEL_GRID_REGION_NEG_Y;
    }
    return positive ? SURFEL_GRID_REGION_POS_Z : SURFEL_GRID_REGION_NEG_Z;
}

vec2 SurfelRemainingAxes(vec3 value, uint axis)
{
    if (axis == 0u) {
        return value.yz;
    }
    if (axis == 1u) {
        return value.xz;
    }
    return value.xy;
}

vec3 SurfelComposeAxisPosition(uint axis, float axisValue, vec2 lateral)
{
    if (axis == 0u) {
        return vec3(axisValue, lateral.x, lateral.y);
    }
    if (axis == 1u) {
        return vec3(lateral.x, axisValue, lateral.y);
    }
    return vec3(lateral.x, lateral.y, axisValue);
}

float SurfelGridCentralHalfExtent(vec3 gridMin, vec3 gridMax)
{
    vec3 halfExtent = abs(gridMax - gridMin) * 0.5;
    return max(min(min(halfExtent.x, halfExtent.y), halfExtent.z), 0.001);
}

float SurfelGridBaseCellSize(vec3 gridMin, vec3 gridMax, uvec3 gridResolution)
{
    return (2.0 * SurfelGridCentralHalfExtent(gridMin, gridMax)) /
        float(SurfelCentralGridResolution(gridResolution));
}

float SurfelGridSliceGrowth()
{
    return 1.22;
}

float SurfelNonLinearSliceStart(float baseCellSize, float growth, uint slice)
{
    if (slice == 0u) {
        return 0.0;
    }
    return baseCellSize * (pow(growth, float(slice)) - 1.0) / max(growth - 1.0, 1e-4);
}

float SurfelNonLinearSliceSize(float baseCellSize, float growth, uint slice)
{
    return baseCellSize * pow(growth, float(slice));
}

bool SurfelWorldToUniformGridAddress(vec3 worldPos,
                                     vec3 gridMin,
                                     vec3 gridMax,
                                     uvec3 gridResolution,
                                     out SurfelGridAddress address)
{
    address.cell = SURFEL_INVALID_INDEX;
    address.region = SURFEL_GRID_REGION_CENTRAL;
    address.coord = uvec3(0u);
    address.center = vec3(0.0);
    address.cellSize = 1.0;

    if (any(equal(gridResolution, uvec3(0u)))) {
        return false;
    }

    vec3 extent = max(gridMax - gridMin, vec3(0.0001));
    vec3 uvw = (worldPos - gridMin) / extent;
    if (any(lessThan(uvw, vec3(0.0))) || any(greaterThanEqual(uvw, vec3(1.0)))) {
        return false;
    }

    address.coord = min(uvec3(floor(uvw * vec3(gridResolution))), gridResolution - uvec3(1u));
    address.cell = SurfelFlattenCell(address.coord, gridResolution);
    vec3 cellSize = extent / vec3(max(gridResolution, uvec3(1u)));
    address.cellSize = max(max(cellSize.x, cellSize.y), cellSize.z);
    address.center = gridMin + (vec3(address.coord) + vec3(0.5)) * cellSize;
    return true;
}

bool SurfelWorldToGridAddress(vec3 worldPos,
                              vec3 gridMin,
                              vec3 gridMax,
                              uvec3 gridResolution,
                              uint useNonLinearGrid,
                              float gridFarExtent,
                              out SurfelGridAddress address)
{
    if (useNonLinearGrid == 0u) {
        return SurfelWorldToUniformGridAddress(worldPos, gridMin, gridMax, gridResolution, address);
    }

    address.cell = SURFEL_INVALID_INDEX;
    address.region = SURFEL_GRID_REGION_CENTRAL;
    address.coord = uvec3(0u);
    address.center = vec3(0.0);
    address.cellSize = SurfelGridBaseCellSize(gridMin, gridMax, gridResolution);

    uint centralResolution = SurfelCentralGridResolution(gridResolution);
    uint lateralResolution = SurfelAxisLateralResolution(gridResolution);
    uint sliceCount = SurfelAxisSliceCount(gridResolution);
    vec3 gridCenter = (gridMin + gridMax) * 0.5;
    vec3 p = worldPos - gridCenter;
    float centerHalfExtent = SurfelGridCentralHalfExtent(gridMin, gridMax);
    float maxAbs = max(max(abs(p.x), abs(p.y)), abs(p.z));
    float baseCellSize = SurfelGridBaseCellSize(gridMin, gridMax, gridResolution);

    if (maxAbs < centerHalfExtent) {
        vec3 uvw = p / max(2.0 * centerHalfExtent, 1e-4) + vec3(0.5);
        address.coord = min(uvec3(floor(uvw * float(centralResolution))), uvec3(centralResolution - 1u));
        address.cell = SurfelFlattenCentralCell(address.coord, centralResolution);
        address.region = SURFEL_GRID_REGION_CENTRAL;
        address.cellSize = baseCellSize;
        address.center = gridMin + (vec3(address.coord) + vec3(0.5)) * baseCellSize;
        return true;
    }

    uint axis = 0u;
    float dominantAbs = abs(p.x);
    if (abs(p.y) > dominantAbs) {
        axis = 1u;
        dominantAbs = abs(p.y);
    }
    if (abs(p.z) > dominantAbs) {
        axis = 2u;
        dominantAbs = abs(p.z);
    }

    float farExtent = max(gridFarExtent, centerHalfExtent + baseCellSize);
    float growth = SurfelGridSliceGrowth();
    float maxSliceEnd = SurfelNonLinearSliceStart(baseCellSize, growth, sliceCount) +
        SurfelNonLinearSliceSize(baseCellSize, growth, sliceCount - 1u);
    farExtent = min(farExtent, centerHalfExtent + maxSliceEnd);
    if (dominantAbs >= farExtent) {
        return false;
    }

    float d = max(dominantAbs - centerHalfExtent, 0.0);
    float sliceFloat = floor(log(max(1.0 + d * (growth - 1.0) / max(baseCellSize, 1e-4), 1.0)) /
        log(growth));
    uint slice = min(uint(max(sliceFloat, 0.0)), sliceCount - 1u);
    float sliceStart = SurfelNonLinearSliceStart(baseCellSize, growth, slice);
    float sliceSize = SurfelNonLinearSliceSize(baseCellSize, growth, slice);
    float axisDepth = centerHalfExtent + sliceStart + sliceSize * 0.5;
    bool positive = axis == 0u ? p.x >= 0.0 : (axis == 1u ? p.y >= 0.0 : p.z >= 0.0);
    uint region = SurfelAxisRegionIndex(axis, positive);
    vec2 lateral = SurfelRemainingAxes(p, axis);
    vec2 lateralCoord = lateral / max(sliceSize, 1e-4) + vec2(float(lateralResolution) * 0.5);
    if (any(lessThan(lateralCoord, vec2(0.0))) ||
        any(greaterThanEqual(lateralCoord, vec2(float(lateralResolution))))) {
        return false;
    }

    uvec2 uv = min(uvec2(floor(lateralCoord)), uvec2(lateralResolution - 1u));
    address.coord = uvec3(uv, slice);
    uint axisCells = SurfelNonLinearAxisCellsPerRegion(gridResolution);
    uint centralCells = SurfelNonLinearCentralCellCount(gridResolution);
    uint regionZeroBased = max(region, 1u) - 1u;
    address.cell = centralCells + regionZeroBased * axisCells +
        (slice * lateralResolution + uv.y) * lateralResolution + uv.x;
    address.region = region;
    address.cellSize = sliceSize;
    vec2 lateralCenter = (vec2(uv) + vec2(0.5) - vec2(float(lateralResolution) * 0.5)) * sliceSize;
    address.center = gridCenter + SurfelComposeAxisPosition(axis, positive ? axisDepth : -axisDepth, lateralCenter);
    return true;
}

bool SurfelGridCellDebugBounds(vec3 worldPos,
                               vec3 gridMin,
                               vec3 gridMax,
                               uvec3 gridResolution,
                               uint useNonLinearGrid,
                               float gridFarExtent,
                               out vec3 center,
                               out vec3 halfExtent,
                               out uint region)
{
    SurfelGridAddress address;
    if (!SurfelWorldToGridAddress(worldPos, gridMin, gridMax, gridResolution, useNonLinearGrid, gridFarExtent, address)) {
        center = vec3(0.0);
        halfExtent = vec3(0.0);
        region = 0u;
        return false;
    }

    center = address.center;
    halfExtent = vec3(address.cellSize * 0.5);
    region = address.region;
    return true;
}

ivec3 SurfelCrossNeighborOffset(uint index)
{
    if (index == 0u) {
        return ivec3(0, 0, 0);
    }
    if (index == 1u) {
        return ivec3(1, 0, 0);
    }
    if (index == 2u) {
        return ivec3(-1, 0, 0);
    }
    if (index == 3u) {
        return ivec3(0, 1, 0);
    }
    if (index == 4u) {
        return ivec3(0, -1, 0);
    }
    if (index == 5u) {
        return ivec3(0, 0, 1);
    }
    return ivec3(0, 0, -1);
}

ivec3 SurfelRawCubeNeighborOffset(uint index)
{
    uint clampedIndex = min(index, 26u);
    int x = int(clampedIndex % 3u) - 1;
    int y = int((clampedIndex / 3u) % 3u) - 1;
    int z = int(clampedIndex / 9u) - 1;
    return ivec3(x, y, z);
}

ivec3 SurfelOrderedCubeOffset(uint index)
{
    if (index == 0u) {
        return ivec3(0, 0, 0);
    }

    uint target = min(index - 1u, 25u);
    uint seen = 0u;
    for (uint raw = 0u; raw < 27u; ++raw) {
        ivec3 offset = SurfelRawCubeNeighborOffset(raw);
        if (all(equal(offset, ivec3(0)))) {
            continue;
        }
        if (seen == target) {
            return offset;
        }
        ++seen;
    }
    return ivec3(1, 1, 1);
}

ivec3 SurfelCubeNeighborOffset(uint index)
{
    return SurfelOrderedCubeOffset(index);
}

ivec3 SurfelRawCubeRadius2NeighborOffset(uint index)
{
    uint clampedIndex = min(index, 124u);
    int x = int(clampedIndex % 5u) - 2;
    int y = int((clampedIndex / 5u) % 5u) - 2;
    int z = int(clampedIndex / 25u) - 2;
    return ivec3(x, y, z);
}

ivec3 SurfelOrderedCubeRadius2Offset(uint index)
{
    if (index == 0u) {
        return ivec3(0, 0, 0);
    }

    uint target = min(index - 1u, 123u);
    uint seen = 0u;
    for (uint raw = 0u; raw < 125u; ++raw) {
        ivec3 offset = SurfelRawCubeRadius2NeighborOffset(raw);
        if (all(equal(offset, ivec3(0)))) {
            continue;
        }
        if (seen == target) {
            return offset;
        }
        ++seen;
    }
    return ivec3(2, 2, 2);
}

ivec3 SurfelCubeRadius2NeighborOffset(uint index)
{
    return SurfelOrderedCubeRadius2Offset(index);
}

bool SurfelWorldToGridCell(vec3 worldPos,
                           vec3 gridMin,
                           vec3 gridMax,
                           uvec3 gridResolution,
                           out uvec3 cell)
{
    SurfelGridAddress address;
    if (!SurfelWorldToUniformGridAddress(worldPos, gridMin, gridMax, gridResolution, address)) {
        cell = uvec3(0u);
        return false;
    }
    cell = address.coord;
    return true;
}

float SurfelMaxGridCellSize(vec3 gridMin, vec3 gridMax, uvec3 gridResolution)
{
    vec3 cellSize = (gridMax - gridMin) / vec3(max(gridResolution, uvec3(1u)));
    return max(max(cellSize.x, cellSize.y), cellSize.z);
}

float SurfelCoverageSupportRadius(float radius, vec3 gridMin, vec3 gridMax, uvec3 gridResolution)
{
    float cellSize = SurfelMaxGridCellSize(gridMin, gridMax, gridResolution);
    return max(max(radius * 5.75, cellSize * 0.78), 0.28);
}

float SurfelGridCellSizeAtWorld(vec3 worldPos,
                                vec3 gridMin,
                                vec3 gridMax,
                                uvec3 gridResolution,
                                uint useNonLinearGrid,
                                float gridFarExtent)
{
    SurfelGridAddress address;
    if (!SurfelWorldToGridAddress(worldPos, gridMin, gridMax, gridResolution, useNonLinearGrid, gridFarExtent, address)) {
        return SurfelMaxGridCellSize(gridMin, gridMax, max(gridResolution, uvec3(1u)));
    }
    return max(address.cellSize, 0.001);
}

float SurfelCoverageSupportRadiusAt(vec3 worldPos,
                                    float radius,
                                    vec3 gridMin,
                                    vec3 gridMax,
                                    uvec3 gridResolution,
                                    uint useNonLinearGrid,
                                    float gridFarExtent)
{
    float cellSize = SurfelGridCellSizeAtWorld(worldPos, gridMin, gridMax, gridResolution, useNonLinearGrid, gridFarExtent);
    return max(max(radius * 4.50, cellSize * 0.72), 0.24);
}

bool SurfelWorldNeighborAddress(vec3 worldPos,
                                uint neighborIndex,
                                uint neighborRadius,
                                vec3 gridMin,
                                vec3 gridMax,
                                uvec3 gridResolution,
                                uint useNonLinearGrid,
                                float gridFarExtent,
                                out SurfelGridAddress address)
{
    SurfelGridAddress baseAddress;
    if (!SurfelWorldToGridAddress(worldPos, gridMin, gridMax, gridResolution, useNonLinearGrid, gridFarExtent, baseAddress)) {
        address = baseAddress;
        return false;
    }

    ivec3 offset = neighborRadius == 0u
        ? ivec3(0)
        : (neighborRadius == 1u ? SurfelCubeNeighborOffset(neighborIndex) : SurfelCubeRadius2NeighborOffset(neighborIndex));
    vec3 samplePos = baseAddress.center + vec3(offset) * max(baseAddress.cellSize, 0.001);
    return SurfelWorldToGridAddress(samplePos, gridMin, gridMax, gridResolution, useNonLinearGrid, gridFarExtent, address);
}

float SurfelCoverageWeight(vec3 receiverPos,
                           vec3 receiverNormal,
                           Surfel surfel,
                           float normalRejectCos,
                           vec3 gridMin,
                           vec3 gridMax,
                           uvec3 gridResolution)
{
    vec3 surfelPos = surfel.worldPos_radius.xyz;
    vec3 surfelNormal = SurfelSafeNormalize(surfel.worldNormal_age.xyz);
    float radius = max(surfel.worldPos_radius.w, 0.001);
    float supportRadius = SurfelCoverageSupportRadius(radius, gridMin, gridMax, gridResolution);
    vec3 delta = receiverPos - surfelPos;
    float disk = exp(-dot(delta, delta) / max(supportRadius * supportRadius, 1e-4));
    float normal = SurfelSaturate((dot(receiverNormal, surfelNormal) - normalRejectCos) / max(1.0 - normalRejectCos, 1e-4));
    float planeSigma = max(max(radius * 2.45, supportRadius * 0.22), 0.045);
    float plane = exp(-abs(dot(delta, surfelNormal)) / max(planeSigma, 1e-4));
    return disk * normal * plane;
}

float SurfelCoverageWeightAt(vec3 receiverPos,
                             vec3 receiverNormal,
                             Surfel surfel,
                             float normalRejectCos,
                             vec3 gridMin,
                             vec3 gridMax,
                             uvec3 gridResolution,
                             uint useNonLinearGrid,
                             float gridFarExtent)
{
    vec3 surfelPos = surfel.worldPos_radius.xyz;
    vec3 surfelNormal = SurfelSafeNormalize(surfel.worldNormal_age.xyz);
    float radius = max(surfel.worldPos_radius.w, 0.001);
    float supportRadius = SurfelCoverageSupportRadiusAt(surfelPos, radius, gridMin, gridMax, gridResolution, useNonLinearGrid, gridFarExtent);
    vec3 delta = receiverPos - surfelPos;
    float disk = exp(-dot(delta, delta) / max(supportRadius * supportRadius, 1e-4));
    float normal = SurfelSaturate((dot(receiverNormal, surfelNormal) - normalRejectCos) / max(1.0 - normalRejectCos, 1e-4));
    float planeSigma = max(max(radius * 2.45, supportRadius * 0.23), 0.045);
    float plane = exp(-abs(dot(delta, surfelNormal)) / max(planeSigma, 1e-4));
    return disk * normal * plane;
}

#endif
