#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

// Temporal accumulation with a per-pixel history length (accumulated frame count).
// The current-frame weight is 1 / length, floored at minCurrentWeight, so a stable pixel averages
// up to maxHistoryLength frames while a disoccluded or low-confidence pixel restarts at length 1.
// The history is clamped to the traced neighbourhood box (mean +/- varianceGamma * sigma, intersected with
// the min/max box), which rejects stale colour without re-introducing the full min/max bias.
//
// Interleaved trace: the gather traces half of the pixels each frame and marks the others with alpha < 0
// in the raw indirect texture. An untraced pixel takes no new sample: its value is the reprojected history
// blended with a depth-weighted fill from traced neighbours, so every pixel leaves this pass filled.

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

// Min/max/mean/sigma over the traced pixels of a (2r+1)^2 window, for the indirect (I) and directional (D)
// textures, plus the depth-weighted mean of the traced pixels (the spatial fill for untraced pixels).
// Clamp boxes are mean +/- gamma * sigma intersected with the min/max box.
void TracedNeighborhood(ivec2 id, ivec2 size, float centerDepth, int radius, float gammaI, float gammaD,
                        out vec4 loI, out vec4 hiI, out vec4 fillI, out vec4 loD, out vec4 hiD, out vec4 fillD) {
    vec4 minI = vec4(1e30);
    vec4 maxI = vec4(-1e30);
    vec4 m1I = vec4(0.0);
    vec4 m2I = vec4(0.0);
    vec4 accI = vec4(0.0);
    vec4 minD = vec4(1e30);
    vec4 maxD = vec4(-1e30);
    vec4 m1D = vec4(0.0);
    vec4 m2D = vec4(0.0);
    vec4 accD = vec4(0.0);
    float count = 0.0;
    float weightSum = 0.0;
    float sigmaDepth = max(depthReject, 0.02);
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            ivec2 c = id + ivec2(x, y);
            if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, size))) {
                continue;
            }
            vec4 si = texelFetch(currentIndirectRaw, c, 0);
            if (si.a < 0.0) {
                continue;
            }
            vec4 sd = texelFetch(currentDirectionalRaw, c, 0);
            float sampleDepth = texelFetch(linearDepthQuarter, c, 0).r;
            float dz = abs(sampleDepth - centerDepth) / max(max(sampleDepth, centerDepth), 1e-4);
            float w = exp(-dz / sigmaDepth) * exp(-float(x * x + y * y) / (2.0 * float(radius * radius) + 1.0));
            if (!IsValidLinearDepth(sampleDepth)) {
                w = 0.0;
            }
            count += 1.0;
            minI = min(minI, si);
            maxI = max(maxI, si);
            m1I += si;
            m2I += si * si;
            minD = min(minD, sd);
            maxD = max(maxD, sd);
            m1D += sd;
            m2D += sd * sd;
            accI += si * w;
            accD += sd * w;
            weightSum += w;
        }
    }
    float inv = 1.0 / max(count, 1.0);
    vec4 meanI = m1I * inv;
    vec4 sigI = sqrt(max(m2I * inv - meanI * meanI, vec4(0.0)));
    vec4 meanD = m1D * inv;
    vec4 sigD = sqrt(max(m2D * inv - meanD * meanD, vec4(0.0)));
    loI = max(meanI - sigI * max(gammaI, 0.0), minI);
    hiI = min(meanI + sigI * max(gammaI, 0.0), maxI);
    loD = max(meanD - sigD * max(gammaD, 0.0), minD);
    hiD = min(meanD + sigD * max(gammaD, 0.0), maxD);
    float invW = weightSum > 1e-6 ? 1.0 / weightSum : 0.0;
    fillI = accI * invW;
    fillD = accD * invW;
    if (count < 0.5) {
        loI = vec4(0.0);
        hiI = vec4(0.0);
        fillI = vec4(0.0);
        loD = vec4(0.0);
        hiD = vec4(0.0);
        fillD = vec4(0.0);
    }
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outIndirect);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 invOutSize = 1.0 / vec2(outSize);
    vec2 uv = (vec2(id) + 0.5) * invOutSize;

    vec4 curI = texelFetch(currentIndirectRaw, id, 0);
    vec4 curD = texelFetch(currentDirectionalRaw, id, 0);
    bool traced = curI.a >= 0.0;

    float currDepth = texelFetch(linearDepthQuarter, id, 0).r;
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

    // Traced pixels clamp to a 3x3 box of traced neighbours (always contains itself). Untraced pixels use a 5x5
    // window, which always reaches a traced row of the 4-row band pattern.
    vec4 loI;
    vec4 hiI;
    vec4 fillI;
    vec4 loD;
    vec4 hiD;
    vec4 fillD;
    TracedNeighborhood(id, outSize, currDepth, traced ? 1 : 2, varianceGamma, varianceGamma * 0.75,
        loI, hiI, fillI, loD, hiD, fillD);
    vec4 fallbackI = traced ? curI : fillI;
    vec4 fallbackD = traced ? curD : fillD;

    if (useHistory == 0 || !historyValid) {
        imageStore(outIndirect, id, fallbackI);
        imageStore(outDirectional, id, fallbackD);
        imageStore(outTemporalDebug, id, vec4(0.0, 1.0, 0.0, currentSignal));
        imageStore(outHistoryRawDebug, id, vec4(0.0));
        imageStore(outHistoryClampedDebug, id, vec4(0.0));
        imageStore(outHistoryLength, id, vec4(1.0));
        return;
    }

    vec4 prevIUnclamped = textureLod(previousIndirectRaw, prevUV, 0.0);
    vec4 prevD = textureLod(previousDirectionalRaw, prevUV, 0.0);
    imageStore(outHistoryRawDebug, id, prevIUnclamped);

    vec4 prevI = clamp(prevIUnclamped, loI, hiI);
    prevD = clamp(prevD, loD, hiD);
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
        imageStore(outIndirect, id, fallbackI);
        imageStore(outDirectional, id, fallbackD);
        imageStore(outTemporalDebug, id, vec4(historyConfidence, 1.0 - historyConfidence, 0.0, currentSignal));
        imageStore(outHistoryLength, id, vec4(1.0));
        return;
    }

    float prevLength = textureLod(previousHistoryLength, prevUV, 0.0).r;
    float maxLength = max(maxHistoryLength, 1.0);
    vec4 resolvedIndirect;
    vec4 resolvedDirectional;
    float historyWeight;
    float newLength;
    if (traced) {
        // Accumulated frame count: a stable pixel grows by one per frame up to the cap, scaled by how
        // much of the history survived the reprojection tests.
        newLength = clamp(prevLength * historyConfidence + 1.0, 1.0, maxLength);
        float currentWeight = max(1.0 / newLength, minCurrentWeight);
        historyWeight = clamp((1.0 - currentWeight) * historyConfidence, 0.0, 0.995);
        currentWeight = 1.0 - historyWeight;
        resolvedIndirect = curI * currentWeight + prevI * historyWeight;
        resolvedDirectional = curD * currentWeight + prevD * historyWeight;
    } else {
        // Untraced: no new sample this frame. Keep the accumulated history and fill the rest from neighbours.
        newLength = clamp(prevLength * historyConfidence, 1.0, maxLength);
        historyWeight = clamp(historyConfidence, 0.0, 0.995);
        resolvedIndirect = prevI * historyWeight + fillI * (1.0 - historyWeight);
        resolvedDirectional = prevD * historyWeight + fillD * (1.0 - historyWeight);
    }

    resolvedIndirect.rgb = max(resolvedIndirect.rgb, vec3(0.0));
    resolvedDirectional.rgb = max(resolvedDirectional.rgb, vec3(0.0));
    resolvedIndirect.a = max(resolvedIndirect.a, 0.0);
    resolvedDirectional.a = max(resolvedDirectional.a, 0.0);

    imageStore(outIndirect, id, resolvedIndirect);
    imageStore(outDirectional, id, resolvedDirectional);
    imageStore(outTemporalDebug, id, vec4(historyConfidence, 1.0 - historyConfidence, historyWeight, currentSignal));
    imageStore(outHistoryLength, id, vec4(newLength));
}
