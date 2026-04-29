#ifndef SURFEL_GI_COMMON_GLSL
#define SURFEL_GI_COMMON_GLSL

#define SURFEL_FLAG_VALID 1u
#define SURFEL_FLAG_DORMANT 2u
#define SURFEL_FLAG_RECYCLED 4u
#define SURFEL_FLAG_TRANSFORM_INVALID 8u

#define SURFEL_STATE_FREE 0u
#define SURFEL_STATE_ACTIVE 1u
#define SURFEL_STATE_RECYCLABLE 2u
#define SURFEL_STATE_DEAD 3u

#define SURFEL_SPAWN_NONE 0u
#define SURFEL_SPAWN_COVERAGE_GAP 1u
#define SURFEL_SPAWN_REFINEMENT 2u
#define SURFEL_SPAWN_DUPLICATE_BLOCKED 3u

#define SURFEL_RECYCLE_NONE 0u
#define SURFEL_RECYCLE_POOL_PRESSURE 1u
#define SURFEL_RECYCLE_OVERSAMPLED 2u
#define SURFEL_RECYCLE_STALE 3u
#define SURFEL_RECYCLE_INVALID_TRANSFORM 4u

#define SURFEL_TILE_FLAG_VISIBLE 1u
#define SURFEL_TILE_FLAG_UNDERCOVERED 2u
#define SURFEL_TILE_FLAG_HIGH_MOTION 4u
#define SURFEL_TILE_FLAG_NEWLY_EXPOSED 8u
#define SURFEL_TILE_FLAG_CONFIDENT_COVERED 16u
#define SURFEL_TILE_FLAG_NEAR_CAMERA 32u

#define SURFEL_LIGHTING_STATE_UNINITIALIZED 0u
#define SURFEL_LIGHTING_STATE_BOOTSTRAP 1u
#define SURFEL_LIGHTING_STATE_ACTIVE 2u
#define SURFEL_LIGHTING_STATE_STABLE 3u
#define SURFEL_LIGHTING_STATE_DORMANT 4u

struct SurfelRecord {
    vec4 worldPositionRadius; // xyz=world position, w=world radius
    vec4 localPositionAge;    // xyz=local position relative to transform, w=age in frames
    vec4 worldNormalRecycle;  // xyz=world normal, w=recycle score
    vec4 localNormalDebug;    // xyz=local normal, w=spawn reason/debug marker
    uvec4 ids;                // x=transform ID, y=flags, z=stable surfel ID, w=spawn seed
    uvec4 frames;             // x=spawn frame, y=last visible, z=last contributing, w=last recycled
    uvec4 grid;               // x=primary cell, y=last primary cell, z=insert count, w=lifecycle state
    vec4 metrics;             // x=coverage, y=camera distance, z=projected radius, w=relevance
    vec4 irradianceHistory;   // rgb=accumulated irradiance, w=history sample count
    vec4 shortTermStats;      // x=short luma mean, y=short deviation, z=last delta, w=short samples
    vec4 longTermStats;       // x=long luma mean, y=long variance proxy, z=integrated frames, w=reuse count
    vec4 recycleData;         // x=priority, y=recycle reason, z=redundancy, w=pool pressure
    vec4 depthMoments;        // x=mean signed local depth, y=second moment, z=sample count, w=support radius
    vec4 guidingState;        // xyz=guiding direction seed, w=attachment generation/dominant-bone token
    vec4 rawIrradiance;       // rgb=latest bounded ray estimate before sharing, w=ray samples accumulated this frame
    vec4 sharedIrradiance;    // rgb=post-neighbour sharing estimate, w=share confidence/weight
    vec4 solveState;          // x=requested rays, y=allocated rays, z=solve state/priority, w=last irradiance integration frame
    vec4 lightingState;       // x=lighting state enum, y=history confidence, z=variance proxy, w=debug flags
};

struct SurfelIrradianceHeader {
    uvec4 rayStats;      // x=requested rays, y=allocated rays, z=eligible surfels, w=active surfels
    uvec4 passStats;     // x=ray-evaluated surfels, y=shared surfels, z=depth updates, w=bleed rejections
    uvec4 debugStats;    // x=bootstrap surfels, y=dormant surfels, z=guiding updates, w=RT/validation skips
    uvec4 config;        // x=ray budget, y=max rays per surfel, z=surfel start, w=surfel count
    uvec4 rayDebugStats;       // x=primary hits, y=primary misses, z=shadow visible, w=shadow occluded
    uvec4 rayDebugStats2;      // x=zero-radiance samples, y=backface corrections, z=rays dispatched, w=rays skipped by budget
    vec4 rayDebugSums;         // x=hit distance sum, y=hit albedo luma sum, z=direct radiance luma sum, w=raw incoming luma sum
    vec4 irradianceDebugSums;  // x=raw irradiance luma sum, y=accumulated irradiance luma sum, z=shared irradiance luma sum, w=sample count
    uvec4 eligibilityReject0;  // x=invalid lifecycle, y=invalid transform, z=invalid normal, w=invalid radius
    uvec4 eligibilityReject1;  // x=missing spatial cell, y=not visible/recent, z=outside residency, w=marked dormant
    uvec4 eligibilityReject2;  // x=zero history confidence, y=sample count zero, z=already solved, w=pool pressure
    uvec4 eligibilityReject3;  // x=no free ids, y=budget scale zero, z=max ray-traced surfels zero, w=max rays per surfel zero
    uvec4 eligibilityReject4;  // x=invalid irradiance slot, y=material/tlas rejection, z=selection/budget capped, w=reserved
    uvec4 gatherStats;         // x=final-gather candidates, y=final-gather accepted, z=fallback used, w=reserved
    vec4 gatherDebugSums;      // x=gathered irradiance luma sum, y=final indirect luma sum, z=weight sum, w=pixel count
    uvec4 rayDebugSumsFixed;        // fixed-point mirrors of rayDebugSums for atomic debug accumulation
    uvec4 irradianceDebugSumsFixed; // fixed-point mirrors of irradianceDebugSums for atomic debug accumulation
    uvec4 gatherDebugSumsFixed;     // fixed-point mirrors of gatherDebugSums for atomic debug accumulation
};

struct SurfelGridCellAverage {
    vec4 irradianceWeight; // rgb=cell-average accumulated irradiance, w=cell confidence
    vec4 normalCount;      // xyz=confidence-weighted average normal, w=lit surfel count
};

#define SURFEL_GI_VALIDATION_MODE_PRODUCTION 0
#define SURFEL_GI_VALIDATION_MODE_BRUTE_FORCE 1
#define SURFEL_GI_VALIDATION_MODE_DIRECT_LIGHT_ONLY SURFEL_GI_VALIDATION_MODE_BRUTE_FORCE
#define SURFEL_GI_VALIDATION_MODE_CONSTANT_INJECTION 2
#define SURFEL_GI_VALIDATION_MODE_SINGLE_SURFEL 3

#define SURFEL_WINNER_ID_BITS 17u
#define SURFEL_WINNER_ID_MASK ((1u << SURFEL_WINNER_ID_BITS) - 1u)

struct SurfelTileMeta {
    uvec4 coverage; // x=coverage sum Q10, y=min coverage Q10, z=candidate pixel packed (y<<16|x), w=last updated frame+1
    uvec4 geom;     // x=min depth Q16, y=max depth Q16, z=normal variance Q16, w=priority Q16
    uvec4 state;    // x=last covered frame+1, y=last spawn frame+1, z=flags, w=last queued frame+1
    uvec4 stats;    // x=processed count, y=confidence skips, z=duplicate rejects, w=spawn rejects
};

struct SurfelPoolHeader {
    uvec4 counts;      // x=max surfels, y=live count, z=free count, w=frame index
    uvec4 frameStats;  // x=spawned, y=recycled, z=dormant, w=spawn attempts
    uvec4 tiling;      // x=tileCountX, y=tileCountY, z=tileSize, w=gridCellCount
    uvec4 gridDims;    // x,y,z dimensions, w=max entries per cell
    vec4 gridParams;   // x=central extent, y=near scale, z=far scale, w=central cell world size
    uvec4 coverageStats;     // x=coverage prevented spawn, y=refinement spawns, z=no free ID, w=probability skipped
    uvec4 contributionStats; // x=old coverage hits, y=new coverage hits, z=old integrated, w=new integrated
    uvec4 recycleStats;      // x=candidates, y=budget recycled, z=invalid-transform recycled, w=lifecycle violations
    uvec4 coverageMetricStats; // x=visible G-buffer pixels, y=valid covered pixels, z/w reserved
    uvec4 tileWorkStats;     // x=tiles scanned, y=tiles skipped by confidence, z=undercovered queued, w=spawn candidates evaluated
    uvec4 budgetStats;       // x=projected surfels processed, y=lifecycle surfels processed, z=recycle surfels processed, w=integrated surfels processed
    uvec4 runtimeState;      // x=tile scan cursor, y=lifecycle cursor, z=recycle cursor, w=projected cursor
    uvec4 queueStats;        // x=queue overflow, y=spawn budget rejected, z=grid rebuild interval, w=grid rebuild countdown
};

uint HashUInt(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float Hash01(uint x)
{
    return float(HashUInt(x) & 0x00ffffffu) / 16777215.0;
}

float SpatioTemporalBlueNoise01(uint stableID, uint frameIndex)
{
    uint ranked = HashUInt(stableID ^ 0x9e3779b9u);
    float spatialRank = float(ranked & 0x0000ffffu) / 65535.0;
    float temporalRotation = fract(float(frameIndex & 1023u) * 0.61803398875);
    float decorrelator = Hash01(ranked ^ (frameIndex * 747796405u)) * (1.0 / 65536.0);
    return fract(spatialRank + temporalRotation + decorrelator);
}

uint SurfelPackWinner(uint surfelID, uint supportWeightQ10)
{
    uint clampedID = min(surfelID, SURFEL_WINNER_ID_MASK);
    uint score = clamp(supportWeightQ10, 1u, 0x7fffu);
    return (score << SURFEL_WINNER_ID_BITS) | (SURFEL_WINNER_ID_MASK - clampedID);
}

uint SurfelUnpackWinnerID(uint packedWinner)
{
    if (packedWinner == 0u) {
        return 0xffffffffu;
    }
    return SURFEL_WINNER_ID_MASK - (packedWinner & SURFEL_WINNER_ID_MASK);
}

bool IsSurfelValid(SurfelRecord s)
{
    return (s.ids.y & SURFEL_FLAG_VALID) != 0u &&
        (s.grid.w == SURFEL_STATE_ACTIVE || s.grid.w == SURFEL_STATE_RECYCLABLE);
}

bool IsSurfelAllocatable(SurfelRecord s)
{
    return (s.ids.y & SURFEL_FLAG_VALID) == 0u &&
        (s.grid.w == SURFEL_STATE_FREE || s.grid.w == SURFEL_STATE_DEAD);
}

void MarkSurfelLifecycleState(inout SurfelRecord s, uint state)
{
    s.grid.w = state;
}

vec3 SurfelStableNormal(vec3 normal)
{
    if (length(normal) < 0.0001 || any(isnan(normal)) || any(isinf(normal))) {
        return vec3(0.0, 1.0, 0.0);
    }
    return normalize(normal);
}

float SurfelDepthValidityWeight(SurfelRecord s, vec3 receiverWorldPos, vec3 receiverNormal)
{
    if (s.depthMoments.z <= 1.0) {
        return 1.0;
    }

    vec3 surfelNormal = SurfelStableNormal(s.worldNormalRecycle.xyz);
    float normalAgreement = clamp(dot(receiverNormal, surfelNormal), 0.0, 1.0);
    float signedDepth = dot(receiverWorldPos - s.worldPositionRadius.xyz, surfelNormal);
    float meanDepth = s.depthMoments.x;
    float variance = max(s.depthMoments.y - meanDepth * meanDepth, 0.000025);
    float sigma = sqrt(variance);
    float support = max(s.depthMoments.w, s.worldPositionRadius.w * 0.35);
    float normalizedDepthError = abs(signedDepth - meanDepth) / max(sigma * 2.5 + support * 0.15, 0.001);
    return (1.0 - smoothstep(0.75, 1.65, normalizedDepthError)) * normalAgreement;
}

vec3 DecodeNormalOctSurfel(vec2 e)
{
    e = e * 2.0 - 1.0;
    vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        vec2 xy = (1.0 - abs(v.yx)) * sign(v.xy);
        v.x = xy.x;
        v.y = xy.y;
    }
    return normalize(v);
}

vec3 SurfelFaceForwardToView(vec3 normal, vec3 worldPos, vec3 cameraPos)
{
    vec3 n = normalize(length(normal) > 0.0001 ? normal : vec3(0.0, 1.0, 0.0));
    vec3 toView = cameraPos - worldPos;
    if (dot(toView, toView) > 0.000001 && dot(n, toView) < 0.0) {
        n = -n;
    }
    return n;
}

float ComputeWorldRadiusForProjectedPixels(float viewZ, float targetPixels, float viewportHeight, mat4 projection)
{
    float focalY = max(abs(projection[1][1]), 0.0001);
    float z = max(abs(viewZ), 0.05);
    return max((targetPixels * z) / (max(viewportHeight, 1.0) * focalY), 0.01);
}

float ComputeAutoTargetRadiusPixels(float tileSize, float coverageThreshold)
{
    return 12.0;
}

float ResolveTargetRadiusPixels(float requestedPixels, float tileSize, float coverageThreshold)
{
    if (requestedPixels > 0.5) {
        return clamp(requestedPixels, 2.0, 32.0);
    }
    return ComputeAutoTargetRadiusPixels(tileSize, coverageThreshold);
}

float LumaSurfel(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float SurfelHistoryConfidence(SurfelRecord s)
{
    float sampleConfidence = clamp(s.irradianceHistory.w / 16.0, 0.0, 1.0);
    float variancePenalty = 1.0 - smoothstep(0.02, 0.35, max(s.longTermStats.y, 0.0));
    return max(clamp(s.lightingState.y, 0.0, 1.0), sampleConfidence * variancePenalty);
}

bool SurfelHasAccumulatedIrradiance(SurfelRecord s)
{
    return any(greaterThan(abs(s.irradianceHistory.rgb), vec3(0.000001)));
}

bool SurfelHasInitializedLighting(SurfelRecord s)
{
    return s.irradianceHistory.w > 0.0 &&
        SurfelHistoryConfidence(s) > 0.0001 &&
        SurfelHasAccumulatedIrradiance(s);
}

bool SurfelLightingIsUninitialized(SurfelRecord s)
{
    return s.irradianceHistory.w <= 0.0 ||
        SurfelHistoryConfidence(s) <= 0.0001 ||
        !SurfelHasAccumulatedIrradiance(s);
}

uint SurfelLightingStateFromHistory(SurfelRecord s)
{
    if ((s.ids.y & SURFEL_FLAG_RECYCLED) != 0u || !IsSurfelValid(s)) {
        return SURFEL_LIGHTING_STATE_UNINITIALIZED;
    }
    if (SurfelLightingIsUninitialized(s)) {
        return s.irradianceHistory.w <= 0.0 ? SURFEL_LIGHTING_STATE_UNINITIALIZED : SURFEL_LIGHTING_STATE_BOOTSTRAP;
    }

    float confidence = SurfelHistoryConfidence(s);
    float variance = max(max(s.shortTermStats.y, s.longTermStats.y), 0.0);
    if (s.irradianceHistory.w < 8.0 || confidence < 0.35) {
        return SURFEL_LIGHTING_STATE_BOOTSTRAP;
    }
    if (variance > 0.08 || s.shortTermStats.z > 0.18) {
        return SURFEL_LIGHTING_STATE_ACTIVE;
    }
    if ((s.ids.y & SURFEL_FLAG_DORMANT) != 0u && confidence >= 0.65 && variance < 0.025) {
        return SURFEL_LIGHTING_STATE_DORMANT;
    }
    if (s.irradianceHistory.w >= 32.0 && confidence >= 0.65 && variance < 0.025) {
        return SURFEL_LIGHTING_STATE_STABLE;
    }
    return SURFEL_LIGHTING_STATE_ACTIVE;
}

uint SurfelEncodeDebugSum(float value)
{
    const float SURFEL_DEBUG_SUM_FIXED_SCALE = 256.0;
    return uint(clamp(max(value, 0.0) * SURFEL_DEBUG_SUM_FIXED_SCALE, 0.0, 4294967040.0));
}

void SurfelBuildBasis(vec3 normal, out vec3 tangent, out vec3 bitangent)
{
    vec3 n = normalize(normal);
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, n));
    bitangent = cross(n, tangent);
}

vec3 SurfelWorldToHemi(vec3 dir, vec3 normal)
{
    vec3 tangent;
    vec3 bitangent;
    vec3 n = normalize(normal);
    SurfelBuildBasis(n, tangent, bitangent);
    return vec3(dot(dir, tangent), dot(dir, bitangent), max(dot(dir, n), 0.0));
}

vec3 SurfelHemiToWorld(vec3 hemi, vec3 normal)
{
    vec3 tangent;
    vec3 bitangent;
    vec3 n = normalize(normal);
    SurfelBuildBasis(n, tangent, bitangent);
    return normalize(tangent * hemi.x + bitangent * hemi.y + n * max(hemi.z, 0.0));
}

vec3 SurfelCosineHemisphereSample(float u1, float u2)
{
    float r = sqrt(max(u1, 0.0));
    float phi = 6.28318530718 * u2;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(1.0 - u1, 0.0));
    return vec3(x, y, z);
}

uint SurfelGuideBin(vec3 hemiDir)
{
    vec2 uv = clamp(hemiDir.xy * 0.5 + 0.5, vec2(0.0), vec2(0.999));
    uvec2 texel = uvec2(floor(uv * 6.0));
    return min(texel.y * 6u + texel.x, 35u);
}

uint SurfelRadialDepthBin(vec3 hemiDir)
{
    vec2 uv = clamp(hemiDir.xy * 0.5 + 0.5, vec2(0.0), vec2(0.999));
    uvec2 texel = uvec2(floor(uv * 4.0));
    return min(texel.y * 4u + texel.x, 15u);
}

vec3 NonLinearGridCoord(vec3 viewPos, SurfelPoolHeader header)
{
    vec3 dims = max(vec3(header.gridDims.xyz), vec3(1.0));
    float centralExtent = max(header.gridParams.x, 1.0);
    vec3 normalizedLinear = clamp((viewPos / centralExtent) * 0.5 + 0.5, vec3(0.0), vec3(1.0));

    float zSign = sign(viewPos.z);
    float absZ = abs(viewPos.z);
    float nonLinearDepth = log2(1.0 + absZ / max(header.gridParams.y, 0.01)) /
        log2(1.0 + max(header.gridParams.z, header.gridParams.y + 1.0) / max(header.gridParams.y, 0.01));
    nonLinearDepth = clamp(nonLinearDepth, 0.0, 1.0);

    float perspectiveScale = max(absZ / centralExtent, 1.0);
    vec2 scaledXY = viewPos.xy / (centralExtent * perspectiveScale);
    vec3 normalizedOuter = vec3(
        clamp(scaledXY.x * 0.5 + 0.5, 0.0, 1.0),
        clamp(scaledXY.y * 0.5 + 0.5, 0.0, 1.0),
        zSign >= 0.0 ? 0.5 + nonLinearDepth * 0.5 : 0.5 - nonLinearDepth * 0.5
    );

    bool insideCentral =
        abs(viewPos.x) <= centralExtent &&
        abs(viewPos.y) <= centralExtent &&
        abs(viewPos.z) <= centralExtent;

    vec3 normalized = insideCentral ? normalizedLinear : normalizedOuter;
    return clamp(normalized * dims, vec3(0.0), dims - vec3(0.0001));
}

uint GridCellIndexFromCoord(uvec3 cell, SurfelPoolHeader header)
{
    uvec3 dims = max(header.gridDims.xyz, uvec3(1u));
    cell = clamp(cell, uvec3(0u), dims - uvec3(1u));
    return (cell.z * dims.y + cell.y) * dims.x + cell.x;
}

uint GridCellIndexForViewPosition(vec3 viewPos, SurfelPoolHeader header)
{
    return GridCellIndexFromCoord(uvec3(floor(NonLinearGridCoord(viewPos, header))), header);
}

vec3 GridCoordRadiusForViewSphere(vec3 viewPos, float radius, SurfelPoolHeader header)
{
    vec3 c = NonLinearGridCoord(viewPos, header);
    vec3 rx = abs(NonLinearGridCoord(viewPos + vec3(radius, 0.0, 0.0), header) - c);
    vec3 ry = abs(NonLinearGridCoord(viewPos + vec3(0.0, radius, 0.0), header) - c);
    vec3 rz = abs(NonLinearGridCoord(viewPos + vec3(0.0, 0.0, radius), header) - c);
    return max(max(rx, ry), rz);
}

#endif
