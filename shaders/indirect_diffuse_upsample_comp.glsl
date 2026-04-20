#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D indirectStage1;
layout(binding = 1) uniform sampler2D directionalStage1;
layout(binding = 2) uniform sampler2D indirectStage2;
layout(binding = 3) uniform sampler2D directionalStage2;
layout(binding = 4) uniform sampler2D indirectRaw;
layout(binding = 5) uniform sampler2D directionalRaw;
layout(binding = 6) uniform sampler2D depthLinearQuarter;
layout(binding = 7) uniform sampler2D normalFromDepthTex;
layout(binding = 8) uniform sampler2D radianceQuarter;
layout(binding = 9) uniform sampler2D horizonDebugQuarter;
layout(binding = 10) uniform sampler2D temporalDebugQuarter;
layout(binding = 11) uniform sampler2D depthFull;
layout(binding = 12) uniform sampler2D normalFull;
layout(binding = 13) uniform sampler2D gAlbedoAO;
layout(binding = 14) uniform sampler2D sectorDebugQuarter;
layout(binding = 15) uniform sampler2D indirectTemporal;

layout(binding = 0, rgba16f) writeonly uniform image2D outFull;

uniform vec2 invFullSize;
uniform vec2 invQuarterSize;
uniform float projScaleX;
uniform float projScaleY;
uniform float upscaleSharpness;
uniform int debugMode;
uniform mat4 view;
uniform int normalsInWorldSpace;
uniform mat4 invProj;

const float kInvalidDepth = 65000.0;
const float PI = 3.14159265359;

float Luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 DecodeNormalVS(vec2 encoded) {
    vec3 n = normalize(DecodeOctNormal01(encoded));
    return (normalsInWorldSpace == 1) ? normalize(mat3(view) * n) : n;
}

float QuarterNormalMip() {
    return max(log2(max(float(textureSize(normalFromDepthTex, 0).x) / max(float(textureSize(depthLinearQuarter, 0).x), 1.0), 1.0)), 0.0);
}

vec3 ViewPosFromLinearDepth(vec2 uv, float linearDepth) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(
        ndc.x * linearDepth / max(projScaleX, 1e-5),
        ndc.y * linearDepth / max(projScaleY, 1e-5),
        -linearDepth
    );
}

float SampleDepthQuarterNearest(vec2 uv, int mipLevel) {
    ivec2 mipSize = textureSize(depthLinearQuarter, mipLevel);
    ivec2 coord = clamp(ivec2(uv * vec2(mipSize)), ivec2(0), mipSize - ivec2(1));
    return texelFetch(depthLinearQuarter, coord, mipLevel).r;
}

float ViewDepthFromDeviceDepth(vec2 uv, float depth01) {
    vec3 viewPos = ReconstructViewPosition(uv, depth01, invProj);
    return max(-viewPos.z, 1e-4);
}

float BrdfSH(vec4 sh, vec3 normalVS) {
    vec4 basis = vec4(0.282095, 0.488603 * normalVS.y, 0.488603 * normalVS.z, 0.488603 * normalVS.x);
    return max(dot(sh, basis), 0.0);
}

vec4 SampleQuarterResolved(vec2 uv, vec3 fullNormal, float fullDepthVS, float fullDepth01, float normalMip) {
    vec4 indAcc = vec4(0.0);
    vec4 shAcc = vec4(0.0);
    float weightAcc = 0.0;

    vec3 fullViewPos = ReconstructViewPosition(uv, fullDepth01, invProj);
    vec3 centerQuarterNormal = DecodeOctNormal01(textureLod(normalFromDepthTex, uv, normalMip).rg);
    if (length(centerQuarterNormal) < 0.5 || any(isnan(centerQuarterNormal))) {
        centerQuarterNormal = fullNormal;
    }

    float sharpness = max(upscaleSharpness, 0.1);
    float depthSigma = max(0.05 * fullDepthVS * mix(1.1, 0.6, clamp((sharpness - 0.5) / 3.5, 0.0, 1.0)), 0.01);
    float planeSigma = max(0.04 * fullDepthVS * mix(1.0, 0.55, clamp((sharpness - 0.5) / 3.5, 0.0, 1.0)), 0.01);
    float normalSigma = max(0.18 / sharpness, 0.03);
    float spatialSigma = mix(1.35, 0.90, clamp((sharpness - 0.5) / 3.5, 0.0, 1.0));

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = clamp(uv + vec2(x, y) * invQuarterSize, vec2(0.0), vec2(1.0));
            float sampleDepth = SampleDepthQuarterNearest(sampleUV, 0);
            if (sampleDepth > kInvalidDepth) {
                continue;
            }

            vec4 ind = textureLod(indirectStage2, sampleUV, 0.0);
            vec4 sh = textureLod(directionalStage2, sampleUV, 0.0);
            vec3 sampleNormal = DecodeOctNormal01(textureLod(normalFromDepthTex, sampleUV, normalMip).rg);
            if (length(sampleNormal) < 0.5 || any(isnan(sampleNormal))) {
                sampleNormal = centerQuarterNormal;
            }

            vec3 sampleViewPos = ViewPosFromLinearDepth(sampleUV, sampleDepth);
            float depthDelta = abs(sampleDepth - fullDepthVS) / max(max(sampleDepth, fullDepthVS), 1e-4);
            float planeDelta = abs(dot(fullNormal, sampleViewPos - fullViewPos));
            float normalAlign = max(dot(fullNormal, sampleNormal), 0.0);
            float quarterAlign = max(dot(centerQuarterNormal, sampleNormal), 0.0);

            float depthW = exp(-depthDelta / max(depthSigma, 1e-4));
            float planeW = exp(-planeDelta / max(planeSigma, 1e-4));
            float normalW = exp(-(1.0 - normalAlign) / max(normalSigma, 1e-4));
            float quarterW = exp(-(1.0 - quarterAlign) / max(normalSigma * 0.75, 1e-4));
            float spatialW = exp(-float(x * x + y * y) / max(spatialSigma * spatialSigma, 1e-4));
            float w = depthW * planeW * normalW * quarterW * spatialW;
            if (w <= 1e-6) {
                continue;
            }

            indAcc += ind * w;
            shAcc += sh * w;
            weightAcc += w;
        }
    }

    if (weightAcc <= 1e-6) {
        vec4 ind = textureLod(indirectStage2, uv, 0.0);
        return vec4(max(ind.rgb, vec3(0.0)), clamp(ind.a, 0.0, 1.0));
    }

    vec4 ind = indAcc / weightAcc;
    ind.rgb = max(ind.rgb, vec3(0.0));
    ind.a = clamp(ind.a, 0.0, 1.0);
    return vec4(ind.rgb, ind.a);
}

vec3 VisualizeDepthPyramid(vec2 uv) {
    int maxMip = max(textureQueryLevels(depthLinearQuarter) - 1, 0);
    float d0 = textureLod(depthLinearQuarter, uv, 0.0).r;
    float d2 = textureLod(depthLinearQuarter, uv, float(min(2, maxMip))).r;
    float d4 = textureLod(depthLinearQuarter, uv, float(min(4, maxMip))).r;
    vec3 d = vec3(d0, d2, d4);

    if (d0 > kInvalidDepth) {
        return vec3(0.0);
    }

    d = 1.0 / (1.0 + d * 0.08);
    return clamp(d, 0.0, 1.0);
}

vec3 VisualizeSliceIntervals(vec2 uv) {
    vec4 interval = textureLod(sectorDebugQuarter, uv, 0.0);
    float startT = clamp(interval.x, 0.0, 1.0);
    float endT = clamp(interval.y, 0.0, 1.0);
    float widthT = clamp(endT - startT, 0.0, 1.0);
    return vec3(startT, endT, widthT);
}

vec3 SampleDebugMode(int mode, vec2 uv, vec3 fullNormal, vec4 upscaledOut) {
    if (mode == 1) {
        return VisualizeDepthPyramid(uv);
    }
    if (mode == 2) {
        vec3 qn = DecodeNormalVS(textureLod(normalFromDepthTex, uv, QuarterNormalMip()).rg);
        return qn * 0.5 + 0.5;
    }
    if (mode == 3) {
        return max(textureLod(radianceQuarter, uv, 0.0).rgb, vec3(0.0));
    }
    if (mode == 4) {
        return VisualizeSliceIntervals(uv);
    }
    if (mode == 5) {
        float coverage = clamp(textureLod(horizonDebugQuarter, uv, 0.0).x, 0.0, 1.0);
        return vec3(coverage);
    }
    if (mode == 6) {
        float newSector = clamp(textureLod(horizonDebugQuarter, uv, 0.0).z, 0.0, 1.0);
        return vec3(newSector);
    }
    if (mode == 7) {
        return max(textureLod(indirectRaw, uv, 0.0).rgb, vec3(0.0));
    }
    if (mode == 8) {
        float ao = clamp(textureLod(indirectRaw, uv, 0.0).a, 0.0, 1.0);
        return vec3(ao);
    }
    if (mode == 9) {
        return max(textureLod(indirectTemporal, uv, 0.0).rgb, vec3(0.0));
    }
    if (mode == 10) {
        float conf = clamp(textureLod(temporalDebugQuarter, uv, 0.0).x, 0.0, 1.0);
        return vec3(conf);
    }
    if (mode == 11) {
        float rejected = clamp(textureLod(temporalDebugQuarter, uv, 0.0).y, 0.0, 1.0);
        return vec3(rejected);
    }
    if (mode == 12) {
        float reinjection = clamp(textureLod(radianceQuarter, uv, 0.0).a * 2.0, 0.0, 1.0);
        return vec3(reinjection);
    }
    if (mode == 13) {
        return max(textureLod(indirectStage1, uv, 0.0).rgb, vec3(0.0));
    }
    if (mode == 14) {
        return max(textureLod(indirectStage2, uv, 0.0).rgb, vec3(0.0));
    }
    if (mode == 15) {
        return upscaledOut.rgb;
    }
    if (mode == 16) {
        vec3 albedo = max(textureLod(gAlbedoAO, uv, 0.0).rgb, vec3(0.0));
        return upscaledOut.rgb * albedo;
    }
    return upscaledOut.rgb;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(outFull);
    if (any(greaterThanEqual(id, fullSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invFullSize;

    float fullDepth01 = textureLod(depthFull, uv, 0.0).r;
    if (fullDepth01 >= 0.9999) {
        imageStore(outFull, id, vec4(0.0));
        return;
    }

    float fullDepthVS = ViewDepthFromDeviceDepth(uv, fullDepth01);
    vec3 fullNormal = DecodeNormalVS(textureLod(normalFull, uv, 0.0).rg);
    if (length(fullNormal) < 0.5 || any(isnan(fullNormal))) {
        fullNormal = vec3(0.0, 0.0, 1.0);
    }

    vec4 upscaledOut = SampleQuarterResolved(uv, normalize(fullNormal), fullDepthVS, fullDepth01, QuarterNormalMip());
    vec3 debugRGB = SampleDebugMode(debugMode, uv, normalize(fullNormal), upscaledOut);
    imageStore(outFull, id, vec4(max(debugRGB, vec3(0.0)), upscaledOut.a));
}
