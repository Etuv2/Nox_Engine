#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 24, std430) buffer TileCoverageBuffer {
    uvec4 tileCoverage[];
};

uniform int uFrameIndex;
uniform vec2 uResolution;
uniform mat4 uView;
uniform mat4 uProjection;
uniform float uCoverageThreshold;

void AccumulateTileCoverage(ivec2 tile, ivec2 tileDims, vec2 centerPx, float radiusPx, uint frameIndex)
{
    if (tile.x < 0 || tile.y < 0 || tile.x >= tileDims.x || tile.y >= tileDims.y) {
        return;
    }

    uint tileIndex = uint(tile.y) * header.tiling.x + uint(tile.x);
    vec2 tileCenter = (vec2(tile) + vec2(0.5)) * float(max(header.tiling.z, 1u));
    float normalizedDistance = length(tileCenter - centerPx) / max(radiusPx + float(header.tiling.z) * 0.55, 1.0);
    float weight = 1.0 - smoothstep(0.55, 1.15, normalizedDistance);
    uint packedWeight = uint(clamp(weight * 1024.0, 0.0, 2048.0));
    if (packedWeight == 0u) {
        return;
    }

    atomicAdd(tileCoverage[tileIndex].x, packedWeight);
    atomicMax(tileCoverage[tileIndex].w, frameIndex + 1u);
}

void main()
{
    uint surfelID = gl_GlobalInvocationID.x;
    if (surfelID >= header.counts.x) {
        return;
    }

    SurfelRecord s = surfels[surfelID];
    if (!IsSurfelValid(s)) {
        return;
    }

    vec4 viewPos = uView * vec4(s.worldPositionRadius.xyz, 1.0);
    vec4 clip = uProjection * viewPos;
    if (clip.w <= 0.0) {
        return;
    }

    vec3 ndc = clip.xyz / clip.w;
    float radiusPx = max(s.metrics.z, 1.0);
    vec2 margin = vec2(radiusPx) / max(uResolution, vec2(1.0)) * 2.0;
    if (ndc.x < -1.0 - margin.x || ndc.x > 1.0 + margin.x ||
        ndc.y < -1.0 - margin.y || ndc.y > 1.0 + margin.y) {
        return;
    }

    vec2 uv = ndc.xy * 0.5 + 0.5;
    vec2 centerPx = uv * uResolution;
    uint tileSize = max(header.tiling.z, 1u);
    ivec2 tileDims = ivec2(int(header.tiling.x), int(header.tiling.y));
    ivec2 centerTile = ivec2(floor(centerPx / float(tileSize)));
    int tileRadius = int(clamp(ceil(radiusPx / float(tileSize)) + 1.0, 1.0, 2.0));
    uint frameIndex = uint(max(uFrameIndex, 0));
    surfels[surfelID].frames.y = max(surfels[surfelID].frames.y, frameIndex);

    for (int y = -tileRadius; y <= tileRadius; ++y) {
        for (int x = -tileRadius; x <= tileRadius; ++x) {
            AccumulateTileCoverage(centerTile + ivec2(x, y), tileDims, centerPx, radiusPx, frameIndex);
        }
    }
}
