#ifndef SURFEL_GI_COMMON_GLSL
#define SURFEL_GI_COMMON_GLSL

#define SURFEL_FLAG_VALID 1u
#define SURFEL_FLAG_DORMANT 2u
#define SURFEL_FLAG_RECYCLED 4u
#define SURFEL_FLAG_TRANSFORM_INVALID 8u

struct SurfelRecord {
    vec4 worldPositionRadius; // xyz=world position, w=world radius
    vec4 localPositionAge;    // xyz=local position relative to transform, w=age in frames
    vec4 worldNormalRecycle;  // xyz=world normal, w=recycle score
    vec4 localNormalDebug;    // xyz=local normal, w=debug/lifecycle marker
    uvec4 ids;                // x=transform ID, y=flags, z=stable surfel ID, w=spawn seed
    uvec4 frames;             // x=spawn frame, y=last visible, z=last contributing, w=last recycled
    uvec4 grid;               // x=primary cell, y=last primary cell, z=insert count, w=state/debug
    vec4 metrics;             // x=coverage, y=camera distance, z=projected radius, w=relevance
};

struct SurfelPoolHeader {
    uvec4 counts;      // x=max surfels, y=live count, z=free count, w=frame index
    uvec4 frameStats;  // x=spawned, y=recycled, z=dormant, w=flags
    uvec4 tiling;      // x=tileCountX, y=tileCountY, z=tileSize, w=gridCellCount
    uvec4 gridDims;    // x,y,z dimensions, w=max entries per cell
    vec4 gridParams;   // x=central extent, y=near scale, z=far scale, w=central cell world size
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
    return (s.ids.y & SURFEL_FLAG_VALID) != 0u;
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

#endif
