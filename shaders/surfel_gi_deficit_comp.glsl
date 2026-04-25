#version 460 core

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 0, r32ui) uniform readonly uimage2D uProjectedCoverage;
layout(binding = 1, r32f) uniform writeonly image2D uDeficit;
layout(binding = 6, r32f) uniform image2D uCoverageHistory;

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

uniform sampler2D uDepthTex;
uniform usampler2D uTransformIDTex;
uniform vec2 uResolution;
uniform float uCoverageThreshold;
uniform float uCoverageHistoryHysteresis;
uniform int uFrameIndex;
uniform int uPixelUpdateModulo;
uniform int uPixelUpdatePhase;

float ResolveCoverageWithHistory(ivec2 pixel, float currentCoverage)
{
    float historyCoverage = max(imageLoad(uCoverageHistory, pixel).r, 0.0);
    float hysteresis = clamp(uCoverageHistoryHysteresis, 0.0, 1.0);
    float retainedCoverage = historyCoverage * hysteresis;
    float resolvedCoverage = max(currentCoverage, retainedCoverage);
    imageStore(uCoverageHistory, pixel, vec4(resolvedCoverage, 0.0, 0.0, 1.0));
    return resolvedCoverage;
}

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 resolution = ivec2(max(uResolution, vec2(1.0)));
    if (pixel.x >= resolution.x || pixel.y >= resolution.y) {
        return;
    }

    uint updateModulo = uint(max(uPixelUpdateModulo, 1));
    uint updatePhase = uint(max(uPixelUpdatePhase, 0)) % updateModulo;
    uint pixelPhase = (uint(pixel.x) + uint(pixel.y) * 3u + uint(max(uFrameIndex, 0))) % updateModulo;
    if (pixelPhase != updatePhase) {
        return;
    }

    float depth = texelFetch(uDepthTex, pixel, 0).r;
    uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
    if (depth >= 1.0 || transformID == 0u) {
        imageStore(uDeficit, pixel, vec4(0.0, 0.0, 0.0, 1.0));
        imageStore(uCoverageHistory, pixel, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }

    atomicAdd(header.coverageMetricStats.x, 1u);

    uint encodedCoverage = imageLoad(uProjectedCoverage, pixel).r;
    float coverage = ResolveCoverageWithHistory(pixel, float(encodedCoverage) / 1024.0);
    float threshold = clamp(uCoverageThreshold, 0.05, 2.5);
    float deficit = max(threshold - coverage, 0.0);
    if (coverage >= threshold) {
        atomicAdd(header.coverageMetricStats.y, 1u);
    }

    imageStore(uDeficit, pixel, vec4(deficit, 0.0, 0.0, 1.0));
}
