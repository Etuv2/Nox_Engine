#version 460 core
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

layout(binding = 0, rgba16f) writeonly uniform image2D outRadianceQuarter;

uniform int usePreviousIndirect;
uniform float previousIndirectFeedback;
uniform float depthReject;
uniform float normalRejectCos;
uniform float disocclusionReject;
uniform mat4 invProj;

const float kInvalidDepth = 65504.0;

float Luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

bool IsValidDepth(float depth) {
    return depth > 0.0 && depth < kInvalidDepth;
}

float ViewDepthFromDeviceDepth(vec2 uv, float depth01) {
    vec3 viewPos = ReconstructViewPosition(uv, depth01, invProj);
    return max(-viewPos.z, 1e-4);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outRadianceQuarter);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) / vec2(outSize);
    float currDepth = texelFetch(linearDepthQuarter, id, 0).r;
    if (!IsValidDepth(currDepth)) {
        imageStore(outRadianceQuarter, id, vec4(0.0));
        return;
    }

    vec3 currNormal = DecodeOctNormal01(textureLod(normalFromDepthTex, uv, 0.0).rg);
    if (any(isnan(currNormal)) || any(isinf(currNormal)) || length(currNormal) < 0.5) {
        currNormal = vec3(0.0, 0.0, 1.0);
    }

    ivec2 srcSize = textureSize(bounceableDiffuseTex, 0);
    ivec2 base = id * 4;

    vec3 sourceRadiance = vec3(0.0);
    float sourceWeight = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 src = clamp(base + ivec2(x, y), ivec2(0), srcSize - ivec2(1));
            vec2 srcUV = (vec2(src) + 0.5) / vec2(srcSize);
            float srcDepth01 = texelFetch(depthFull, src, 0).r;
            if (srcDepth01 >= 0.999999) {
                continue;
            }

            float srcDepthVS = ViewDepthFromDeviceDepth(srcUV, srcDepth01);
            float depthRel = abs(srcDepthVS - currDepth) / max(max(srcDepthVS, currDepth), 1e-4);
            if (depthRel > max(depthReject * 3.0, 0.12)) {
                continue;
            }

            vec3 bounceable = texelFetch(bounceableDiffuseTex, src, 0).rgb;
            vec3 emissive = texelFetch(emissiveTex, src, 0).rgb;
            vec3 source = max(bounceable + emissive, vec3(0.0));
            float depthW = exp(-depthRel / max(depthReject * 0.75, 1e-4));
            float w = depthW;
            if (w <= 1e-6) {
                continue;
            }
            sourceRadiance += source * w;
            sourceWeight += w;
        }
    }

    vec3 radiance = (sourceWeight > 1e-6)
        ? (sourceRadiance / sourceWeight)
        : max(texelFetch(bounceableDiffuseTex, clamp(base + ivec2(1, 1), ivec2(0), srcSize - ivec2(1)), 0).rgb, vec3(0.0));
    float reinjectWeight = 0.0;

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

            if (IsValidDepth(prevDepth) && !any(isnan(prevNormal)) && !any(isinf(prevNormal))) {
                float depthRel = abs(currDepth - prevDepth) / max(max(currDepth, prevDepth), 1e-4);
                float depthW = exp(-depthRel / max(depthReject, 1e-4));

                float normalSim = max(dot(normalize(currNormal), normalize(prevNormal)), 0.0);
                float normalW = smoothstep(normalRejectCos, 1.0, normalSim);

                float frontDelta = max(prevDepth - currDepth, 0.0) / max(currDepth, 1e-4);
                float disocclusion = smoothstep(disocclusionReject * 0.25, disocclusionReject, frontDelta);

                float motionPixels = length(velocity * vec2(textureSize(linearDepthQuarter, 0)));
                float motionW = exp(-motionPixels / 64.0);

                float historySignal = Luma(max(prevIndirect.rgb, vec3(0.0)));
                float historySignalWeight = 1.0 - exp(-historySignal * 0.75);
                reinjectWeight = clamp(previousIndirectFeedback, 0.0, 0.75) * historySignalWeight;
                reinjectWeight *= depthW * normalW * motionW * (1.0 - disocclusion);
                reinjectWeight = clamp(reinjectWeight, 0.0, 0.65);

                radiance += max(prevIndirect.rgb, vec3(0.0)) * reinjectWeight;
            }
        }
    }

    radiance = clamp(radiance, vec3(0.0), vec3(65504.0));
    imageStore(outRadianceQuarter, id, vec4(radiance, reinjectWeight));
}
