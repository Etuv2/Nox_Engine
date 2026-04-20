#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D currentIndirectRaw;
layout(binding = 1) uniform sampler2D currentDirectionalRaw;
layout(binding = 2) uniform sampler2D previousIndirectRaw;
layout(binding = 3) uniform sampler2D previousDirectionalRaw;
layout(binding = 4) uniform sampler2D velocityTex;
layout(binding = 5) uniform sampler2D linearDepthQuarter;
layout(binding = 6) uniform sampler2D normalFromDepthTex;
layout(binding = 7) uniform sampler2D previousLinearDepthQuarter;
layout(binding = 8) uniform sampler2D previousNormalTex;

layout(binding = 0, rgba16f) writeonly uniform image2D outIndirect;
layout(binding = 1, rgba16f) writeonly uniform image2D outDirectional;
layout(binding = 2, rgba16f) writeonly uniform image2D outTemporalDebug;

uniform int useHistory;
uniform float historyBlend;
uniform float depthReject;
uniform float normalRejectCos;
uniform float disocclusionReject;
uniform float motionRejectPixels;
uniform float historyClampStrength;
uniform float minHistoryConfidence;

const float kInvalidDepth = 65504.0;

vec4 ClampHistoryToNeighborhood(vec2 uv, vec4 value, sampler2D tex, vec2 invSize, float clampStrength) {
    vec4 minV = vec4(1e30);
    vec4 maxV = vec4(-1e30);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = clamp(uv + vec2(x, y) * invSize, vec2(0.0), vec2(1.0));
            vec4 sampleV = textureLod(tex, sampleUV, 0.0);
            minV = min(minV, sampleV);
            maxV = max(maxV, sampleV);
        }
    }

    vec4 ext = (maxV - minV) * max(clampStrength, 0.0);
    return clamp(value, minV - ext, maxV + ext);
}

bool IsValidDepth(float depth) {
    return depth > 0.0 && depth < kInvalidDepth;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outIndirect);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 invOutSize = 1.0 / vec2(outSize);
    vec2 uv = (vec2(id) + 0.5) * invOutSize;

    vec4 curI = textureLod(currentIndirectRaw, uv, 0.0);
    vec4 curD = textureLod(currentDirectionalRaw, uv, 0.0);

    float currDepth = textureLod(linearDepthQuarter, uv, 0.0).r;
    if (!IsValidDepth(currDepth)) {
        imageStore(outIndirect, id, vec4(0.0));
        imageStore(outDirectional, id, vec4(0.0));
        imageStore(outTemporalDebug, id, vec4(0.0, 1.0, 0.0, 0.0));
        return;
    }

    vec3 currNormal = DecodeOctNormal01(textureLod(normalFromDepthTex, uv, 0.0).rg);
    if (any(isnan(currNormal)) || any(isinf(currNormal)) || length(currNormal) < 0.5) {
        currNormal = vec3(0.0, 0.0, 1.0);
    }

    vec2 velocity = texture(velocityTex, uv).rg;
    vec2 prevUV = uv - velocity;
    bool historyValid =
        all(greaterThanEqual(prevUV, vec2(0.001))) &&
        all(lessThanEqual(prevUV, vec2(0.999)));

    float currentSignal = clamp(curI.a, 0.0, 1.0);

    if (useHistory == 0) {
        imageStore(outIndirect, id, curI);
        imageStore(outDirectional, id, curD);
        imageStore(outTemporalDebug, id, vec4(0.0, 1.0, 0.0, currentSignal));
        return;
    }

    if (!historyValid) {
        imageStore(outIndirect, id, curI);
        imageStore(outDirectional, id, curD);
        imageStore(outTemporalDebug, id, vec4(0.0, 1.0, 0.0, currentSignal));
        return;
    }

    vec4 prevI = textureLod(previousIndirectRaw, prevUV, 0.0);
    vec4 prevD = textureLod(previousDirectionalRaw, prevUV, 0.0);

    prevI = ClampHistoryToNeighborhood(uv, prevI, currentIndirectRaw, invOutSize, historyClampStrength);
    prevD = ClampHistoryToNeighborhood(uv, prevD, currentDirectionalRaw, invOutSize, historyClampStrength * 0.75);

    float prevDepth = textureLod(previousLinearDepthQuarter, prevUV, 0.0).r;
    vec3 prevNormal = DecodeOctNormal01(textureLod(previousNormalTex, prevUV, 0.0).rg);
    if (any(isnan(prevNormal)) || any(isinf(prevNormal)) || length(prevNormal) < 0.5) {
        prevNormal = vec3(0.0, 0.0, 1.0);
    }

    float depthRel = abs(currDepth - prevDepth) / max(max(currDepth, prevDepth), 1e-4);
    float depthW = exp(-depthRel / max(depthReject, 1e-4));

    float normalSim = max(dot(normalize(currNormal), normalize(prevNormal)), 0.0);
    float normalW = smoothstep(normalRejectCos, 1.0, normalSim);

    float frontDelta = max(prevDepth - currDepth, 0.0) / max(currDepth, 1e-4);
    float disocclusion = smoothstep(disocclusionReject * 0.25, disocclusionReject, frontDelta);

    float motionPixels = length(velocity * vec2(textureSize(currentIndirectRaw, 0)));
    float motionW = exp(-motionPixels / max(motionRejectPixels, 1.0));
    if (motionPixels > motionRejectPixels * 1.25) {
        disocclusion = 1.0;
    }

    float historyConfidence = depthW * normalW * motionW * (1.0 - disocclusion);
    historyConfidence = clamp(historyConfidence, 0.0, 1.0);

    if (historyConfidence < minHistoryConfidence) {
        imageStore(outIndirect, id, curI);
        imageStore(outDirectional, id, curD);
        imageStore(outTemporalDebug, id, vec4(historyConfidence, disocclusion, 0.0, currentSignal));
        return;
    }

    float historyWeight = clamp(historyConfidence * historyBlend, 0.0, 0.95);
    float currentWeight = 1.0 - historyWeight;

    vec4 resolvedIndirect = curI * currentWeight + prevI * historyWeight;
    vec4 resolvedDirectional = curD * currentWeight + prevD * historyWeight;

    resolvedIndirect.rgb = max(resolvedIndirect.rgb, vec3(0.0));
    resolvedDirectional.rgb = max(resolvedDirectional.rgb, vec3(0.0));
    resolvedIndirect.a = max(resolvedIndirect.a, 0.0);
    resolvedDirectional.a = max(resolvedDirectional.a, 0.0);

    imageStore(outIndirect, id, resolvedIndirect);
    imageStore(outDirectional, id, resolvedDirectional);
    imageStore(outTemporalDebug, id, vec4(historyConfidence, disocclusion, historyWeight, currentSignal));
}
