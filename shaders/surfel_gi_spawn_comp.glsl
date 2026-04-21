#version 460 core

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#include "includes/transform_tracking_contract.glsl"
#include "includes/surfel_gi_common.glsl"

layout(binding = 6, std430) readonly buffer TransformBuffer {
    GpuTransformRecord transforms[];
};

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 22, std430) buffer FreeStackBuffer {
    uint freeStack[];
};

layout(binding = 24, std430) buffer TileCoverageBuffer {
    uvec4 tileCoverage[];
};

layout(binding = 25, std430) buffer GridHeaderBuffer {
    uvec4 gridHeaders[];
};

layout(binding = 26, std430) readonly buffer GridEntryBuffer {
    uint gridEntries[];
};

uniform sampler2D uPackedNormalRM;
uniform usampler2D uTransformIDTex;
uniform sampler2D uDepthTex;

uniform int uFrameIndex;
uniform int uMaxTransformID;
uniform vec2 uResolution;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uInvViewProj;
uniform mat4 uInvView;
uniform float uTargetRadiusPixels;
uniform float uCoverageThreshold;
uniform float uNormalReject;
uniform vec3 uCameraPos;

shared float sCoverage[256];
shared uint sPackedPixel[256];

vec3 ReconstructWorldPosition(ivec2 pixel, float depth)
{
    vec2 uv = (vec2(pixel) + vec2(0.5)) / max(uResolution, vec2(1.0));
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    return world.xyz / max(world.w, 0.00001);
}

float EvaluateCoverage(vec3 worldPos, vec3 normal)
{
    vec3 viewPos = (uView * vec4(worldPos, 1.0)).xyz;
    uint cell = GridCellIndexForViewPosition(viewPos, header);
    uint count = min(gridHeaders[cell].x, header.gridDims.w);
    float coverage = 0.0;

    for (uint i = 0u; i < count; ++i) {
        uint surfelID = gridEntries[cell * header.gridDims.w + i];
        SurfelRecord s = surfels[surfelID];
        if (!IsSurfelValid(s)) {
            continue;
        }

        float normalAlign = dot(normal, s.worldNormalRecycle.xyz);
        if (normalAlign < 1.0 - uNormalReject) {
            continue;
        }

        vec3 delta = worldPos - s.worldPositionRadius.xyz;
        float planeDistance = abs(dot(delta, s.worldNormalRecycle.xyz));
        vec3 tangentDelta = delta - s.worldNormalRecycle.xyz * dot(delta, s.worldNormalRecycle.xyz);
        float tangentDistance = length(tangentDelta);
        float radius = max(s.worldPositionRadius.w, 0.001);
        float tangentWeight = 1.0 - smoothstep(radius * 0.35, radius, tangentDistance);
        float planeWeight = 1.0 - smoothstep(0.0, radius * 0.5, planeDistance);
        float disc = tangentWeight * planeWeight;
        coverage += disc * clamp(normalAlign, 0.0, 1.0);
    }

    return coverage;
}

void SpawnSurfel(ivec2 pixel, float coverage, uint tileIndex, uint frameIndex)
{
    float depth = texelFetch(uDepthTex, pixel, 0).r;
    if (depth >= 1.0) {
        return;
    }

    uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
    if (transformID == 0u || transformID >= uint(max(uMaxTransformID, 1))) {
        return;
    }

    vec4 packedNormal = texelFetch(uPackedNormalRM, pixel, 0);
    vec3 normal = DecodeNormalOctSurfel(packedNormal.xy);
    vec3 worldPos = ReconstructWorldPosition(pixel, depth);
    vec4 viewPos = uView * vec4(worldPos, 1.0);
    float radius = ComputeWorldRadiusForProjectedPixels(viewPos.z, uTargetRadiusPixels, uResolution.y, uProjection);

    uint freeCountBefore = atomicAdd(header.counts.z, uint(-1));
    if (freeCountBefore == 0u) {
        atomicAdd(header.counts.z, 1u);
        return;
    }

    uint surfelID = freeStack[freeCountBefore - 1u];
    GpuTransformRecord transformRecord = transforms[transformID];
    mat4 world = transformRecord.world;
    mat4 invWorld = inverse(world);
    vec3 localPos = (invWorld * vec4(worldPos, 1.0)).xyz;
    vec3 localNormal = normalize(transpose(mat3(world)) * normal);

    SurfelRecord s;
    s.worldPositionRadius = vec4(worldPos, radius);
    s.localPositionAge = vec4(localPos, 0.0);
    s.worldNormalRecycle = vec4(normal, 0.0);
    s.localNormalDebug = vec4(localNormal, 1.0);
    s.ids = uvec4(transformID, SURFEL_FLAG_VALID, surfelID, HashUInt(surfelID ^ frameIndex ^ tileIndex));
    s.frames = uvec4(frameIndex, frameIndex, frameIndex, 0u);
    s.grid = uvec4(0u, 0u, 0u, 1u);
    s.metrics = vec4(coverage, distance(worldPos, uCameraPos), uTargetRadiusPixels, 1.0);
    surfels[surfelID] = s;

    atomicAdd(header.counts.y, 1u);
    atomicAdd(header.frameStats.x, 1u);
    tileCoverage[tileIndex].z = surfelID;
}

void main()
{
    uvec2 local = gl_LocalInvocationID.xy;
    uint localIndex = local.y * 16u + local.x;
    uint tileSize = max(header.tiling.z, 1u);
    uvec2 tile = gl_WorkGroupID.xy;
    uint tileIndex = tile.y * header.tiling.x + tile.x;
    ivec2 pixel = ivec2(tile * tileSize + local);

    float coverage = 9999.0;
    bool validPixel = local.x < tileSize && local.y < tileSize &&
        pixel.x < int(uResolution.x) && pixel.y < int(uResolution.y);

    if (validPixel) {
        float depth = texelFetch(uDepthTex, pixel, 0).r;
        uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
        validPixel = depth < 1.0 && transformID != 0u && transformID < uint(max(uMaxTransformID, 1));
        if (validPixel) {
            vec3 worldPos = ReconstructWorldPosition(pixel, depth);
            vec3 normal = DecodeNormalOctSurfel(texelFetch(uPackedNormalRM, pixel, 0).xy);
            coverage = EvaluateCoverage(worldPos, normal);
        }
    }

    sCoverage[localIndex] = coverage;
    sPackedPixel[localIndex] = (uint(max(pixel.y, 0)) << 16u) | uint(max(pixel.x, 0));
    barrier();

    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (localIndex < stride) {
            uint otherIndex = localIndex + stride;
            if (sCoverage[otherIndex] < sCoverage[localIndex]) {
                sCoverage[localIndex] = sCoverage[otherIndex];
                sPackedPixel[localIndex] = sPackedPixel[otherIndex];
            }
        }
        barrier();
    }

    if (localIndex == 0u) {
        uint frameIndex = uint(max(uFrameIndex, 0));
        float minCoverage = sCoverage[0];
        uint packedPixel = sPackedPixel[0];
        ivec2 leastPixel = ivec2(int(packedPixel & 0xffffu), int(packedPixel >> 16u));
        float jitter = Hash01(HashUInt(tileIndex ^ (frameIndex * 1664525u)));
        float threshold = clamp(uCoverageThreshold + jitter * 0.35, 0.05, 2.5);

        tileCoverage[tileIndex] = uvec4(
            uint(clamp(minCoverage, 0.0, 16.0) * 1024.0),
            packedPixel,
            0xffffffffu,
            minCoverage < threshold ? 1u : 0u);

        if (minCoverage < threshold) {
            SpawnSurfel(leastPixel, minCoverage, tileIndex, frameIndex);
        }
    }
}
