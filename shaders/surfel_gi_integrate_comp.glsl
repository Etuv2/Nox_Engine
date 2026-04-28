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
uniform sampler2D uAlbedoAO;
uniform sampler2D uEmissive;
uniform usampler2D uTransformIDTex;
uniform sampler2D uDepthTex;

uniform int uFrameIndex;
uniform vec2 uResolution;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uInvViewProj;
uniform vec3 uDirectionalLightDir;
uniform vec3 uDirectionalLightColor;
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

    vec3 gbufferNormal = DecodeNormalOctSurfel(texelFetch(uPackedNormalRM, pixel, 0).xy);
    float normalAlign = dot(normalize(s.worldNormalRecycle.xyz), gbufferNormal);
    if (normalAlign < 0.70) {
        return;
    }

    vec3 emissive = texelFetch(uEmissive, pixel, 0).rgb;
    vec3 normal = normalize(s.worldNormalRecycle.xyz);
    vec3 visibleWorldPos = ReconstructWorldPositionAtPixel(pixel, sceneDepth);
    float signedLocalDepth = dot(visibleWorldPos - s.worldPositionRadius.xyz, normal);
    vec3 lightDir = normalize(-uDirectionalLightDir);
    float nDotL = max(dot(normal, lightDir), 0.0);
    vec3 directIrradiance = max(uDirectionalLightColor, vec3(0.0)) * nDotL;
    vec3 visibleSurfaceIrradiance = max(directIrradiance + emissive + vec3(0.025), vec3(0.0));

    float historySamples = max(s.irradianceHistory.w, 0.0);
    vec3 previous = s.irradianceHistory.rgb;
    float luma = LumaSurfel(visibleSurfaceIrradiance);

    float shortSamples = min(s.shortTermStats.w + 1.0, 64.0);
    float shortAlpha = shortSamples <= 1.0 ? 1.0 : 0.18;
    float shortResidual = luma - s.shortTermStats.x;
    float shortMean = mix(s.shortTermStats.x, luma, shortAlpha);
    float shortVariance = mix(max(s.shortTermStats.y, 0.0), shortResidual * shortResidual, shortAlpha);

    float longSamples = min(s.longTermStats.z + 1.0, 4096.0);
    float longAlpha = clamp(1.0 / max(longSamples, 1.0), 0.002, 0.03);
    float longResidual = luma - s.longTermStats.x;
    float longMean = mix(s.longTermStats.x, luma, longAlpha);
    float longVariance = mix(max(s.longTermStats.y, 0.0), longResidual * longResidual, longAlpha);
    float instability = clamp(abs(shortMean - s.longTermStats.x) / max(sqrt(max(longVariance, 0.000001)), 0.01), 0.0, 1.0);
    float alpha = historySamples < 1.0 ? 1.0 : mix(0.025, 0.22, smoothstep(0.35, 1.0, instability));
    vec3 integrated = mix(previous, visibleSurfaceIrradiance, alpha);
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

    s.irradianceHistory = vec4(integrated, min(historySamples + 1.0, 65535.0));
    s.rawIrradiance = vec4(visibleSurfaceIrradiance, 1.0);
    s.sharedIrradiance = vec4(visibleSurfaceIrradiance, 0.0);
    s.shortTermStats = vec4(shortMean, shortVariance, instability, shortSamples);
    s.longTermStats = vec4(longMean, longVariance, longSamples, min(s.longTermStats.w + 1.0, 65535.0));
    s.depthMoments = vec4(depthMean, depthSecondMoment, depthSamples, max(s.worldPositionRadius.w * 0.35, 0.001));
    vec3 incidentDir = normalize(-uDirectionalLightDir);
    vec3 hemiDir = SurfelWorldToHemi(incidentDir, normal);
    float guideAlpha = clamp(0.05 + LumaSurfel(visibleSurfaceIrradiance) * 0.02, 0.05, 0.20);
    s.guidingState.xyz = mix(s.guidingState.xyz, hemiDir, guideAlpha);
    s.solveState.w = float(frameIndex);
    s.frames.z = frameIndex;
    surfels[id] = s;
}
