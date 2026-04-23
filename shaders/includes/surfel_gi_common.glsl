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

float SurfelDepthValidityWeight(SurfelRecord s, vec3 receiverWorldPos, vec3 receiverNormal)
{
    if (s.depthMoments.z <= 1.0) {
        return 1.0;
    }

    vec3 surfelNormal = normalize(s.worldNormalRecycle.xyz);
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
