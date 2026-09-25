#version 460 core
#include "includes/pbr_common.glsl"
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D bounceableDiffuseTex;
layout(binding = 1) uniform sampler2D emissiveTex;
layout(binding = 2) uniform sampler2D previousIndirectTex;
layout(binding = 3) uniform sampler2D linearDepthQuarter;
layout(binding = 4) uniform sampler2D velocityTex;
layout(binding = 5) uniform sampler2D previousLinearDepthQuarter;
layout(binding = 6) uniform sampler2D previousNormalTex;
layout(binding = 7) uniform sampler2D normalFromDepthTex;
layout(binding = 8) uniform sampler2D depthFull;
layout(binding = 9) uniform sampler2D gPackedNormalRM;
layout(binding = 10) uniform sampler2D gAlbedoAO;

layout(binding = 0, rgba16f) writeonly uniform image2D outRadianceQuarter;
layout(binding = 1, rgba16f) writeonly uniform image2D outRadianceCurrent;
layout(binding = 2, rgba16f) writeonly uniform image2D outRadianceReinjection;
layout(binding = 3, rgba16f) writeonly uniform image2D outRadianceProvenance;

uniform int usePreviousIndirect;
uniform float previousIndirectFeedback;
uniform float depthReject;
uniform float normalRejectCos;
uniform float disocclusionReject;
uniform float sourceNormalRejectCos;
uniform float sourceAlbedoReject;
uniform mat4 invProj;

bool TryLoadSourceDepthVS(ivec2 src, ivec2 srcSize, out float depthVS, out float depth01) {
    ivec2 clampedSrc = clamp(src, ivec2(0), srcSize - ivec2(1));
    depth01 = texelFetch(depthFull, clampedSrc, 0).r;
    if (depth01 >= 0.999999) {
        depthVS = NOX_FP16_MAX;
        return false;
    }

    vec2 srcUV = (vec2(clampedSrc) + 0.5) / vec2(srcSize);
    depthVS = ViewDepthFromDeviceDepth(srcUV, depth01, invProj);
    return IsValidLinearDepth(depthVS);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outRadianceQuarter);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) / vec2(outSize);
    float currDepth = texelFetch(linearDepthQuarter, id, 0).r;
    if (!IsValidLinearDepth(currDepth)) {
        imageStore(outRadianceQuarter, id, vec4(0.0));
        return;
    }

    vec3 currNormal = DecodeOctNormal01(textureLod(normalFromDepthTex, uv, 0.0).rg);
    if (any(isnan(currNormal)) || any(isinf(currNormal)) || length(currNormal) < 0.5) {
        currNormal = vec3(0.0, 0.0, 1.0);
    }

    ivec2 srcSize = textureSize(bounceableDiffuseTex, 0);
    ivec2 base = id * 4;
    ivec2 anchorSrc = clamp(base + ivec2(1, 1), ivec2(0), srcSize - ivec2(1));
    float anchorDepthVS = NOX_FP16_MAX;
    float anchorDepth01 = 1.0;

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 src = clamp(base + ivec2(x, y), ivec2(0), srcSize - ivec2(1));
            float sampleDepthVS;
            float sampleDepth01;
            if (!TryLoadSourceDepthVS(src, srcSize, sampleDepthVS, sampleDepth01)) {
                continue;
            }

            if (sampleDepthVS < anchorDepthVS) {
                anchorDepthVS = sampleDepthVS;
                anchorDepth01 = sampleDepth01;
                anchorSrc = src;
            }
        }
    }

    vec3 anchorFullNormal = DecodeOctNormal01(texelFetch(gPackedNormalRM, anchorSrc, 0).rg);
    anchorFullNormal = normalize((length(anchorFullNormal) < 0.5 || any(isnan(anchorFullNormal))) ? currNormal : anchorFullNormal);
    vec3 anchorAlbedo = max(texelFetch(gAlbedoAO, anchorSrc, 0).rgb, vec3(0.0));

    vec3 sourceRadiance = vec3(0.0);
    vec3 sourceDiffuseAlbedo = vec3(0.0);
    float sourceWeight = 0.0;
    float acceptedSamples = 0.0;
    float depthWeightAccum = 0.0;
    float normalWeightAccum = 0.0;
    float albedoWeightAccum = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 src = clamp(base + ivec2(x, y), ivec2(0), srcSize - ivec2(1));
            float srcDepthVS;
            float srcDepth01;
            if (!TryLoadSourceDepthVS(src, srcSize, srcDepthVS, srcDepth01)) {
                continue;
            }
            float depthRel = abs(srcDepthVS - currDepth) / max(max(srcDepthVS, currDepth), 1e-4);
            if (depthRel > max(depthReject * 3.0, 0.12)) {
                continue;
            }

            vec3 sampleNormal = DecodeOctNormal01(texelFetch(gPackedNormalRM, src, 0).rg);
            sampleNormal = normalize((length(sampleNormal) < 0.5 || any(isnan(sampleNormal))) ? anchorFullNormal : sampleNormal);
            float normalSim = max(dot(sampleNormal, anchorFullNormal), 0.0);
            if (normalSim < sourceNormalRejectCos) {
                continue;
            }

            vec3 sampleAlbedo = max(texelFetch(gAlbedoAO, src, 0).rgb, vec3(0.0));
            float albedoDelta = length(sampleAlbedo - anchorAlbedo) / max(Luma(anchorAlbedo) + Luma(sampleAlbedo), 0.15);
            if (albedoDelta > sourceAlbedoReject) {
                continue;
            }

            vec3 bounceable = texelFetch(bounceableDiffuseTex, src, 0).rgb;
            vec3 emissive = texelFetch(emissiveTex, src, 0).rgb;
            vec3 source = max(bounceable + emissive, vec3(0.0));
            float sampleMetallic = clamp(texelFetch(gPackedNormalRM, src, 0).a, 0.0, 1.0);
            float depthW = exp(-depthRel / max(depthReject * 0.75, 1e-4));
            float normalW = smoothstep(sourceNormalRejectCos, 1.0, normalSim);
            float albedoW = exp(-albedoDelta / max(sourceAlbedoReject * 0.5, 1e-4));
            float w = depthW * normalW * albedoW;
            if (w <= 1e-6) {
                continue;
            }
            sourceRadiance += source * w;
            sourceDiffuseAlbedo += sampleAlbedo * (1.0 - sampleMetallic) * w;
            sourceWeight += w;
            acceptedSamples += 1.0;
            depthWeightAccum += depthW;
            normalWeightAccum += normalW;
            albedoWeightAccum += albedoW;
        }
    }

    vec3 currentRadiance = (sourceWeight > 1e-6)
        ? (sourceRadiance / sourceWeight)
        : max(texelFetch(bounceableDiffuseTex, anchorSrc, 0).rgb + texelFetch(emissiveTex, anchorSrc, 0).rgb, vec3(0.0));
    vec3 diffuseAlbedo = (sourceWeight > 1e-6)
        ? (sourceDiffuseAlbedo / sourceWeight)
        : anchorAlbedo * (1.0 - clamp(texelFetch(gPackedNormalRM, anchorSrc, 0).a, 0.0, 1.0));
    vec3 radiance = currentRadiance;
    float reinjectWeight = 0.0;
    vec3 reinjectionRadiance = vec3(0.0);

    if (usePreviousIndirect == 1 && previousIndirectFeedback > 0.0) {
        vec2 velocity = texture(velocityTex, uv).rg;
        vec2 prevUV = uv - velocity;
        bool validPrev =
            all(greaterThanEqual(prevUV, vec2(0.001))) &&
            all(lessThanEqual(prevUV, vec2(0.999)));

        if (validPrev) {
            float prevDepth = textureLod(previousLinearDepthQuarter, prevUV, 0.0).r;
            vec3 prevNormal = DecodeOctNormal01(textureLod(previousNormalTex, prevUV, 0.0).rg);
            vec4 prevIndirect = textureLod(previousIndirectTex, prevUV, 0.0);

            if (IsValidLinearDepth(prevDepth) && !any(isnan(prevNormal)) && !any(isinf(prevNormal))) {
                float depthRel = abs(currDepth - prevDepth) / max(max(currDepth, prevDepth), 1e-4);
                float depthW = exp(-depthRel / max(depthReject, 1e-4));

                float normalSim = max(dot(normalize(currNormal), normalize(prevNormal)), 0.0);
                float normalW = smoothstep(normalRejectCos, 1.0, normalSim);

                float frontDelta = max(prevDepth - currDepth, 0.0) / max(currDepth, 1e-4);
                float disocclusion = smoothstep(disocclusionReject * 0.25, disocclusionReject, frontDelta);

                float motionPixels = length(velocity * vec2(textureSize(linearDepthQuarter, 0)));
                float motionW = exp(-motionPixels / 64.0);

                // Multi-bounce: last frame's irradiance leaves this surface again as Lambertian
                // radiance albedo / pi * E. A feedback of 1 is physically exact; history that fails
                // the reprojection tests is dropped rather than smeared.
                reinjectWeight = clamp(previousIndirectFeedback, 0.0, 1.0);
                reinjectWeight *= depthW * normalW * motionW * (1.0 - disocclusion);

                reinjectionRadiance = diffuseAlbedo * INV_PI * max(prevIndirect.rgb, vec3(0.0)) * reinjectWeight;
                radiance += reinjectionRadiance;
            }
        }
    }

    radiance = clamp(radiance, vec3(0.0), vec3(65504.0));
    imageStore(outRadianceQuarter, id, vec4(radiance, reinjectWeight));
    imageStore(outRadianceCurrent, id, vec4(clamp(currentRadiance, vec3(0.0), vec3(65504.0)), 1.0));
    imageStore(outRadianceReinjection, id, vec4(clamp(reinjectionRadiance, vec3(0.0), vec3(65504.0)), reinjectWeight));
    float acceptRatio = acceptedSamples / 16.0;
    vec4 provenance = vec4(
        acceptRatio,
        (acceptedSamples > 0.0) ? depthWeightAccum / acceptedSamples : 0.0,
        (acceptedSamples > 0.0) ? normalWeightAccum / acceptedSamples : 0.0,
        (acceptedSamples > 0.0) ? albedoWeightAccum / acceptedSamples : 0.0
    );
    imageStore(outRadianceProvenance, id, provenance);
}
