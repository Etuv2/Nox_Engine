#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

uniform sampler2D uPackedNormalRM;
uniform usampler2D uTransformIDTex;
uniform sampler2D uDepthTex;

uniform int uFrameIndex;
uniform vec2 uResolution;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;
uniform int uSurfelStart;
uniform int uSurfelCount;

vec3 ReconstructWorldPositionAtPixel(ivec2 pixel, float depth)
{
    vec2 uv = (vec2(pixel) + vec2(0.5)) / max(uResolution, vec2(1.0));
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    return world.xyz / max(world.w, 0.00001);
}

void main()
{
    uint localIndex = gl_GlobalInvocationID.x;
    uint surfelCount = uint(max(uSurfelCount, 0));
    if (localIndex >= surfelCount || header.counts.x == 0u) {
        return;
    }

    uint id = (uint(max(uSurfelStart, 0)) + localIndex) % header.counts.x;

    SurfelRecord s = surfels[id];
    if (!IsSurfelValid(s)) {
        return;
    }

    atomicAdd(header.budgetStats.w, 1u);

    vec4 viewPos = uView * vec4(s.worldPositionRadius.xyz, 1.0);
    vec4 clip = uProjection * viewPos;
    if (clip.w <= 0.0) {
        return;
    }

    vec3 ndc = clip.xyz / clip.w;
    if (any(lessThan(ndc.xy, vec2(-1.0))) || any(greaterThan(ndc.xy, vec2(1.0)))) {
        return;
    }

    vec2 uv = ndc.xy * 0.5 + 0.5;
    ivec2 pixel = ivec2(clamp(floor(uv * uResolution), vec2(0.0), max(uResolution - vec2(1.0), vec2(0.0))));
    float sceneDepth = texelFetch(uDepthTex, pixel, 0).r;
    float surfelDepth = ndc.z * 0.5 + 0.5;
    if (sceneDepth >= 1.0 || abs(sceneDepth - surfelDepth) > 0.0045) {
        surfels[id].guidingState.w = max(s.guidingState.w, 0.0);
        return;
    }

    uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
    if (transformID != s.ids.x) {
        return;
    }

    vec3 visibleWorldPos = ReconstructWorldPositionAtPixel(pixel, sceneDepth);
    vec3 normal = SurfelStableNormal(s.worldNormalRecycle.xyz);
    vec3 gbufferNormal = SurfelStableNormal(DecodeNormalOctSurfel(texelFetch(uPackedNormalRM, pixel, 0).xy));
    float normalAlign = dot(normal, gbufferNormal);
    if (normalAlign < 0.70) {
        return;
    }
    s.worldNormalRecycle.xyz = normal;

    float signedLocalDepth = dot(visibleWorldPos - s.worldPositionRadius.xyz, normal);
    float depthSamples = min(s.depthMoments.z + 1.0, 512.0);
    float depthAlpha = depthSamples <= 1.0 ? 1.0 : clamp(1.0 / depthSamples, 0.02, 0.35);
    float depthMean = mix(s.depthMoments.x, signedLocalDepth, depthAlpha);
    float depthSecondMoment = mix(s.depthMoments.y, signedLocalDepth * signedLocalDepth, depthAlpha);

    uint frameIndex = uint(max(uFrameIndex, 0));
    uint ageFrames = frameIndex - min(s.frames.x, frameIndex);
    if (ageFrames > 45u) {
        atomicAdd(header.contributionStats.z, 1u);
    } else {
        atomicAdd(header.contributionStats.w, 1u);
    }

    // This pass validates the surfel-to-G-buffer attachment and radial depth.
    // Ray-traced indirect samples are the only producers of rawIrradiance/solveState.w.
    s.depthMoments = vec4(depthMean, depthSecondMoment, depthSamples, max(s.worldPositionRadius.w * 0.35, 0.001));
    surfels[id] = s;
}
