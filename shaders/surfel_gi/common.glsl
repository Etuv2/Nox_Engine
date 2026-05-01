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

const uint SURFEL_RADIAL_DEPTH_TEXELS = 16u;
const uint SURFEL_GUIDE_CELLS = 36u;
const uint SURFEL_INVALID_INDEX = 0xffffffffu;
const uint SURFEL_SPAWN_TILE_CANDIDATES = 8u;
const float SURFEL_PI = 3.14159265358979323846;
const float SURFEL_RADIAL_DEPTH_MIN_VISIBILITY = 0.35;

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

uvec2 SurfelSpawnTilePixel(uvec2 tile,
                           uvec2 tileExtent,
                           uint frameIndex,
                           uint passIndex,
                           uint passCount,
                           uint sampleIndex)
{
    uint pixelCount = max(tileExtent.x * tileExtent.y, 1u);
    uint seed = tile.x * 0x8da6b343u ^
        tile.y * 0xd8163841u ^
        frameIndex * 0xcb1ab31fu ^
        (passIndex + passCount * 17u) * 0x165667b1u ^
        sampleIndex * 0x9e3779b9u;
    uint pixelIndex = SurfelHash(seed) % pixelCount;
    return uvec2(pixelIndex % tileExtent.x, pixelIndex / tileExtent.x);
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

uint SurfelFlattenCell(uvec3 cell, uvec3 gridResolution)
{
    uvec3 dims = max(gridResolution, uvec3(1u));
    uvec3 c = clamp(cell, uvec3(0u), dims - uvec3(1u));
    return (c.z * dims.y + c.y) * dims.x + c.x;
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

ivec3 SurfelCubeNeighborOffset(uint index)
{
    uint clampedIndex = min(index, 26u);
    int x = int(clampedIndex % 3u) - 1;
    int y = int((clampedIndex / 3u) % 3u) - 1;
    int z = int(clampedIndex / 9u) - 1;
    return ivec3(x, y, z);
}

ivec3 SurfelCubeRadius2NeighborOffset(uint index)
{
    uint clampedIndex = min(index, 124u);
    int x = int(clampedIndex % 5u) - 2;
    int y = int((clampedIndex / 5u) % 5u) - 2;
    int z = int(clampedIndex / 25u) - 2;
    return ivec3(x, y, z);
}

bool SurfelWorldToGridCell(vec3 worldPos,
                           vec3 gridMin,
                           vec3 gridMax,
                           uvec3 gridResolution,
                           out uvec3 cell)
{
    cell = uvec3(0u);
    if (any(equal(gridResolution, uvec3(0u)))) {
        return false;
    }

    vec3 extent = max(gridMax - gridMin, vec3(0.0001));
    vec3 uvw = (worldPos - gridMin) / extent;
    if (any(lessThan(uvw, vec3(0.0))) || any(greaterThanEqual(uvw, vec3(1.0)))) {
        return false;
    }

    cell = min(uvec3(floor(uvw * vec3(gridResolution))), gridResolution - uvec3(1u));
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
    return max(max(radius * 8.0, cellSize * 1.05), 0.32);
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
    float planeSigma = max(max(radius * 2.0, supportRadius * 0.18), 0.035);
    float plane = exp(-abs(dot(delta, surfelNormal)) / max(planeSigma, 1e-4));
    return disk * normal * plane;
}

#endif
