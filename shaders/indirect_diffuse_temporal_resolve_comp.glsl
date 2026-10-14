#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

// Temporal accumulation with a per-pixel history length (accumulated frame count).
// The current-frame weight is 1 / length, floored at minCurrentWeight, so a stable pixel averages
// up to maxHistoryLength frames while a disoccluded or low-confidence pixel restarts at length 1.
// The history is clamped to the current 3x3 mean +/- varianceGamma * sigma box (intersected with
// the min/max box), which rejects stale colour without re-introducing the full min/max bias.

layout(binding = 0) uniform sampler2D currentIndirectRaw;
layout(binding = 1) uniform sampler2D currentDirectionalRaw;
layout(binding = 2) uniform sampler2D previousIndirectRaw;
layout(binding = 3) uniform sampler2D previousDirectionalRaw;
layout(binding = 4) uniform sampler2D velocityTex;
layout(binding = 5) uniform sampler2D linearDepthQuarter;
layout(binding = 6) uniform sampler2D normalFromDepthTex;
layout(binding = 7) uniform sampler2D previousLinearDepthQuarter;
layout(binding = 8) uniform sampler2D previousNormalTex;
layout(binding = 9) uniform sampler2D previousHistoryLength;

layout(binding = 0, rgba16f) writeonly uniform image2D outIndirect;
layout(binding = 1, rgba16f) writeonly uniform image2D outDirectional;
layout(binding = 2, rgba16f) writeonly uniform image2D outTemporalDebug;
layout(binding = 3, rgba16f) writeonly uniform image2D outHistoryRawDebug;
layout(binding = 4, rgba16f) writeonly uniform image2D outHistoryClampedDebug;
layout(binding = 5, r16f) writeonly uniform image2D outHistoryLength;

uniform int useHistory;
uniform float minCurrentWeight;
uniform float maxHistoryLength;
uniform float depthReject;
uniform float normalRejectCos;
uniform float disocclusionReject;
uniform float motionRejectPixels;
uniform float varianceGamma;
uniform float minHistoryConfidence;

vec4 ClampHistoryToNeighborhood(vec2 uv, vec4 value, sampler2D tex, vec2 invSize, float gamma) {
    vec4 minV = vec4(1e30);
    vec4 maxV = vec4(-1e30);
    vec4 m1 = vec4(0.0);
    vec4 m2 = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = clamp(uv + vec2(x, y) * invSize, vec2(0.0), vec2(1.0));
            vec4 sampleV = textureLod(tex, sampleUV, 0.0);
            minV = min(minV, sampleV);
            maxV = max(maxV, sampleV);
            m1 += sampleV;
            m2 += sampleV * sampleV;
        }
    }

    vec4 mean = m1 / 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec4(0.0)));
    float g = max(gamma, 0.0);
    vec4 lo = max(mean - sigma * g, minV);
    vec4 hi = min(mean + sigma * g, maxV);
    return clamp(value, lo, hi);
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
    if (!IsValidLinearDepth(currDepth)) {
        imageStore(outIndirect, id, vec4(0.0));
        imageStore(outDirectional, id, vec4(0.0));
        imageStore(outTemporalDebug, id, vec4(0.0, 1.0, 0.0, 0.0));
        imageStore(outHistoryRawDebug, id, vec4(0.0));
        imageStore(outHistoryClampedDebug, id, vec4(0.0));
        imageStore(outHistoryLength, id, vec4(0.0));
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
        imageStore(outHistoryRawDebug, id, vec4(0.0));
        imageStore(outHistoryClampedDebug, id, vec4(0.0));
        imageStore(outHistoryLength, id, vec4(1.0));
        return;
    }

    if (!historyValid) {
        imageStore(outIndirect, id, curI);
        imageStore(outDirectional, id, curD);
        imageStore(outTemporalDebug, id, vec4(0.0, 1.0, 0.0, currentSignal));
        imageStore(outHistoryRawDebug, id, vec4(0.0));
        imageStore(outHistoryClampedDebug, id, vec4(0.0));
        imageStore(outHistoryLength, id, vec4(1.0));
        return;
    }

    vec4 prevIUnclamped = textureLod(previousIndirectRaw, prevUV, 0.0);
    vec4 prevD = textureLod(previousDirectionalRaw, prevUV, 0.0);
    imageStore(outHistoryRawDebug, id, prevIUnclamped);

    vec4 prevI = ClampHistoryToNeighborhood(uv, prevIUnclamped, currentIndirectRaw, invOutSize, varianceGamma);
    prevD = ClampHistoryToNeighborhood(uv, prevD, currentDirectionalRaw, invOutSize, varianceGamma * 0.75);
    imageStore(outHistoryClampedDebug, id, prevI);

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
        imageStore(outTemporalDebug, id, vec4(historyConfidence, 1.0 - historyConfidence, 0.0, currentSignal));
        imageStore(outHistoryLength, id, vec4(1.0));
        return;
    }

    // Accumulated frame count: a stable pixel grows by one per frame up to the cap, scaled by how
    // much of the history survived the reprojection tests.
    float prevLength = textureLod(previousHistoryLength, prevUV, 0.0).r;
    float newLength = clamp(prevLength * historyConfidence + 1.0, 1.0, max(maxHistoryLength, 1.0));
    float currentWeight = max(1.0 / newLength, minCurrentWeight);
    float historyWeight = clamp((1.0 - currentWeight) * historyConfidence, 0.0, 0.995);
    currentWeight = 1.0 - historyWeight;

    vec4 resolvedIndirect = curI * currentWeight + prevI * historyWeight;
    vec4 resolvedDirectional = curD * currentWeight + prevD * historyWeight;

    resolvedIndirect.rgb = max(resolvedIndirect.rgb, vec3(0.0));
    resolvedDirectional.rgb = max(resolvedDirectional.rgb, vec3(0.0));
    resolvedIndirect.a = max(resolvedIndirect.a, 0.0);
    resolvedDirectional.a = max(resolvedDirectional.a, 0.0);

    imageStore(outIndirect, id, resolvedIndirect);
    imageStore(outDirectional, id, resolvedDirectional);
    imageStore(outTemporalDebug, id, vec4(historyConfidence, 1.0 - historyConfidence, historyWeight, currentSignal));
    imageStore(outHistoryLength, id, vec4(newLength));
}
