#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D inIndirect;
layout(binding = 1) uniform sampler2D inDirectional;
layout(binding = 2) uniform sampler2D linearDepthQuarter;
layout(binding = 3) uniform sampler2D normalFromDepthTex;
layout(binding = 4) uniform sampler2D temporalDebug;

layout(binding = 5, rgba16f) writeonly uniform image2D outIndirect;
layout(binding = 6, rgba16f) writeonly uniform image2D outDirectional;
layout(binding = 7, rgba16f) writeonly uniform image2D outDenoiseDebug;

uniform vec2 invQuarterSize;
uniform vec2 fullResolution;
uniform float depthSigma;
uniform float normalReject;
uniform float denoiseStrength;
uniform int kernelRadius;
uniform float confidencePower;
uniform float lumaPhi;

const float kInvalidDepth = 65000.0;

float Luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

float QuarterNormalMip() {
    int maxMip = max(textureQueryLevels(normalFromDepthTex) - 1, 0);
    if (maxMip == 0) {
        return 0.0;
    }
    ivec2 outSize = imageSize(outIndirect);
    float desiredMip = max(log2(max(fullResolution.x / max(float(outSize.x), 1.0), 1.0)), 0.0);
    return clamp(desiredMip, 0.0, float(maxMip));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outIndirect);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invQuarterSize;
    vec4 centerI = textureLod(inIndirect, uv, 0.0);
    vec4 centerD = textureLod(inDirectional, uv, 0.0);
    float centerDepth = textureLod(linearDepthQuarter, uv, 0.0).r;
    if (centerDepth > kInvalidDepth) {
        imageStore(outIndirect, id, vec4(0.0));
        imageStore(outDirectional, id, vec4(0.0));
        imageStore(outDenoiseDebug, id, vec4(0.0));
        return;
    }

    float normalMip = QuarterNormalMip();
    vec3 centerN = DecodeOctNormal01(textureLod(normalFromDepthTex, uv, normalMip).rg);
    if (length(centerN) < 0.5 || any(isnan(centerN))) {
        centerN = vec3(0.0, 0.0, 1.0);
    }

    float centerConf = clamp(textureLod(temporalDebug, uv, 0.0).x, 0.0, 1.0);
    vec3 centerColor = max(centerI.rgb, vec3(0.0));
    float centerLuma = Luma(max(centerI.rgb, vec3(0.0)));
    float centerAO = clamp(centerI.a, 0.0, 1.0);
    vec4 centerSH = centerD;

    float stageTightness = (kernelRadius <= 1) ? 1.0 : 0.0;
    float stageScale = mix(1.0, 0.62, stageTightness);
    float confPower = max(confidencePower, 0.1);
    float stability = mix(0.78, 1.0, centerConf);

    float denoiseScale = mix(1.0, 1.15, clamp((denoiseStrength - 1.0) / 2.0, 0.0, 1.0));
    float depthSigmaEff = max(depthSigma * stageScale * mix(0.92, 1.08, centerConf) * denoiseScale, 0.003);
    float normalSigmaEff = max(max(normalReject, mix(0.14, 0.08, stageTightness)) * mix(0.82, 1.0, stageTightness) * mix(0.90, 1.05, centerConf), 0.015);
    float spatialSigma = max(float(max(kernelRadius, 1)) * mix(1.20, 0.95, stageTightness), 0.55);
    float colorSigma = max(lumaPhi, mix(0.22, 0.12, stageTightness));
    float aoSigma = mix(0.24, 0.14, stageTightness);
    float dirSigma = mix(0.28, 0.16, stageTightness);

    vec4 indirectAcc = vec4(0.0);
    vec4 directionalAcc = vec4(0.0);
    float weightAcc = 0.0;
    float acceptedSamples = 0.0;
    float visitedSamples = 0.0;

    for (int y = -kernelRadius; y <= kernelRadius; ++y) {
        for (int x = -kernelRadius; x <= kernelRadius; ++x) {
            vec2 sampleUV = uv + vec2(x, y) * invQuarterSize;
            if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
                continue;
            }

            vec4 sampleI = textureLod(inIndirect, sampleUV, 0.0);
            vec4 sampleD = textureLod(inDirectional, sampleUV, 0.0);
            float sampleDepth = textureLod(linearDepthQuarter, sampleUV, 0.0).r;
            if (sampleDepth > kInvalidDepth) {
                continue;
            }
            visitedSamples += 1.0;

            vec3 sampleN = DecodeOctNormal01(textureLod(normalFromDepthTex, sampleUV, normalMip).rg);
            if (length(sampleN) < 0.5 || any(isnan(sampleN))) {
                sampleN = centerN;
            }

            float sampleConf = clamp(textureLod(temporalDebug, sampleUV, 0.0).x, 0.0, 1.0);
            vec3 sampleColor = max(sampleI.rgb, vec3(0.0));
            float sampleLuma = Luma(max(sampleI.rgb, vec3(0.0)));
            float sampleAO = clamp(sampleI.a, 0.0, 1.0);
            vec4 sampleSH = sampleD;

            float dz = abs(centerDepth - sampleDepth) / max(max(centerDepth, sampleDepth), 1e-4);
            if (dz > max(depthSigmaEff * 3.0, 0.06)) {
                continue;
            }
            float depthW = exp(-dz / max(depthSigmaEff, 1e-4));

            float ndot = max(dot(centerN, sampleN), 0.0);
            if (ndot < mix(0.80, 0.60, stageTightness)) {
                continue;
            }
            float normalW = exp(-(1.0 - ndot) / max(normalSigmaEff, 1e-4));

            float spatialW = exp(-float(x * x + y * y) / max(spatialSigma * spatialSigma, 1e-4));
            float historyW = mix(0.20, 1.0, pow(sampleConf, confPower));
            historyW *= stability;

            float lumaDenom = max(mix(centerLuma, sampleLuma, 0.5), 0.15);
            float lumaDelta = abs(sampleLuma - centerLuma) / lumaDenom;
            float chromaDelta = length(sampleColor - centerColor) / max(centerLuma + sampleLuma, 0.15);
            if (chromaDelta > mix(1.25, 2.5, stageTightness)) {
                continue;
            }
            float colorDelta = lumaDelta + chromaDelta * 1.5;
            float colorW = exp(-colorDelta / max(colorSigma, 0.04));

            float aoW = exp(-abs(sampleAO - centerAO) / max(aoSigma, 0.02));

            float shDelta = length(sampleSH - centerSH) / max(length(sampleSH) + length(centerSH), 0.10);
            float dirW = exp(-shDelta / max(dirSigma, 0.04));

            float w = depthW * normalW * spatialW * historyW * colorW * aoW * dirW;
            if (w <= 1e-6) {
                continue;
            }

            indirectAcc += sampleI * w;
            directionalAcc += sampleD * w;
            weightAcc += w;
            acceptedSamples += 1.0;
        }
    }

    if (weightAcc <= 1e-6) {
        imageStore(outIndirect, id, centerI);
        imageStore(outDirectional, id, centerD);
        imageStore(outDenoiseDebug, id, vec4(0.0, 0.0, centerConf, 0.0));
        return;
    }

    vec4 denoisedIndirect = indirectAcc / weightAcc;
    vec4 denoisedDirectional = directionalAcc / weightAcc;

    denoisedIndirect.rgb = max(denoisedIndirect.rgb, vec3(0.0));
    denoisedIndirect.a = clamp(denoisedIndirect.a, 0.0, 1.0);
    denoisedDirectional.x = max(denoisedDirectional.x, 0.0);

    imageStore(outIndirect, id, denoisedIndirect);
    imageStore(outDirectional, id, denoisedDirectional);
    imageStore(outDenoiseDebug, id, vec4(
        clamp(weightAcc / max(visitedSamples, 1.0), 0.0, 1.0),
        clamp(acceptedSamples / max(visitedSamples, 1.0), 0.0, 1.0),
        centerConf,
        0.0));
}
