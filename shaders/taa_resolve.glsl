#version 460 core
#include "includes/screen_space_reconstruction.glsl"

in vec2 TexCoord;

uniform sampler2D currentFrame;
uniform sampler2D historyFrame;
uniform sampler2D velocityBuffer;
uniform sampler2D depthBuffer;
uniform sampler2D normalBuffer;
uniform sampler2D historyDepthBuffer;
uniform sampler2D historyNormalBuffer;

uniform float blendFactor;
uniform float varianceThreshold;
uniform float lumaWeight;
uniform bool useYCoCg;
uniform bool historyValid;
uniform vec2 screenSize;

uniform float depthThreshold = 0.001;
uniform float normalThreshold = 0.1;
uniform float edgeThreshold = 0.05;
uniform float reactiveMaskStrength = 0.8;

// Sky pixels have no rasterised velocity; reproject their view direction with the camera rotation
// (current unjittered clip -> previous unjittered clip, translation removed).
uniform mat4 uSkyReprojection;
// Resolve in a luminance-compressed space c / (1 + luma(c)) (Karis 2014) so very bright HDR
// texels (lamp glints on wet asphalt) cannot dominate the neighbourhood statistics and flicker.
uniform bool uTonemappedResolve = true;

// 0 = legacy rejection (raw-depth/normal difference, edge-reactive). 1 = disocclusion test against
// the camera-reprojected linear depth with a 2x2 history footprint (no rejection at jittered
// silhouettes or on surfaces moving toward the camera), no edge-reactive term.
uniform int uTaaMode = 0;
uniform mat4 uInvViewProj;             // unjittered current clip -> world
uniform mat4 uPrevViewProj;            // unjittered previous world -> clip
uniform vec2 uPrevDepthParams;         // prevProj[2][2], prevProj[3][2] (linearises previous depth)
uniform float uDisocclusionTolerance = 0.03; // relative linear-depth tolerance
uniform float uMotionCurrentWeight = 0.6;    // max current-frame weight added by fast motion

layout(location = 0) out vec3 taaResult;

vec3 RGBToYCoCg(vec3 rgb) {
    float Y = 0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b;
    float Co = 0.5 * rgb.r - 0.5 * rgb.b;
    float Cg = -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b;
    return vec3(Y, Co, Cg);
}

vec3 YCoCgToRGB(vec3 ycocg) {
    float Y = ycocg.x;
    float Co = ycocg.y;
    float Cg = ycocg.z;
    return vec3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

float ComputeLuma(vec3 c) {
    return Luma(c);
}

vec3 CompressHDR(vec3 c) { return uTonemappedResolve ? c / (1.0 + Luma(c)) : c; }
vec3 DecompressHDR(vec3 c) { return uTonemappedResolve ? c / max(1.0 - Luma(c), 1e-4) : c; }

vec3 SampleHistoryCatmullRom(vec2 uv) {
    vec2 texSize = vec2(textureSize(historyFrame, 0));
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / max(w12, vec2(1e-5));

    vec2 texPos0 = (texPos1 - vec2(1.0)) / texSize;
    vec2 texPos3 = (texPos1 + vec2(2.0)) / texSize;
    vec2 texPos12 = (texPos1 + offset12) / texSize;

    vec3 result = vec3(0.0);
    result += texture(historyFrame, vec2(texPos0.x, texPos0.y)).rgb * w0.x * w0.y;
    result += texture(historyFrame, vec2(texPos12.x, texPos0.y)).rgb * w12.x * w0.y;
    result += texture(historyFrame, vec2(texPos3.x, texPos0.y)).rgb * w3.x * w0.y;
    result += texture(historyFrame, vec2(texPos0.x, texPos12.y)).rgb * w0.x * w12.y;
    result += texture(historyFrame, vec2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
    result += texture(historyFrame, vec2(texPos3.x, texPos12.y)).rgb * w3.x * w12.y;
    result += texture(historyFrame, vec2(texPos0.x, texPos3.y)).rgb * w0.x * w3.y;
    result += texture(historyFrame, vec2(texPos12.x, texPos3.y)).rgb * w12.x * w3.y;
    result += texture(historyFrame, vec2(texPos3.x, texPos3.y)).rgb * w3.x * w3.y;
    return max(result, vec3(0.0));
}

void AnalyzeNeighborhood(
    vec2 uv,
    out vec3 minColor,
    out vec3 maxColor,
    out vec3 meanColor,
    out vec3 variance,
    out float edgeMask
) {
    vec3 samples[9];
    int index = 0;

    minColor = vec3(1e6);
    maxColor = vec3(-1e6);
    meanColor = vec3(0.0);

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = clamp(uv + vec2(float(x), float(y)) / screenSize, vec2(0.0), vec2(1.0));
            vec3 color = CompressHDR(texture(currentFrame, sampleUV).rgb);
            if (useYCoCg) {
                color = RGBToYCoCg(color);
            }

            samples[index++] = color;
            minColor = min(minColor, color);
            maxColor = max(maxColor, color);
            meanColor += color;
        }
    }
    meanColor *= (1.0 / 9.0);

    variance = vec3(0.0);
    for (int i = 0; i < 9; ++i) {
        vec3 d = samples[i] - meanColor;
        variance += d * d;
    }
    variance *= (1.0 / 9.0);

    float lumaVariance = useYCoCg ? variance.x : ComputeLuma(variance);
    edgeMask = clamp(sqrt(max(lumaVariance, 0.0)) / max(edgeThreshold, 1e-4), 0.0, 1.0);
}

vec3 ClampHistoryToNeighborhood(
    vec3 historyColor,
    vec3 minColor,
    vec3 maxColor,
    vec3 meanColor,
    vec3 variance
) {
    vec3 sigma = sqrt(max(variance, vec3(1e-5)));
    float gamma = mix(1.2, 1.0, clamp(varianceThreshold, 0.0, 1.0));
    vec3 statMin = max(minColor, meanColor - gamma * sigma);
    vec3 statMax = min(maxColor, meanColor + gamma * sigma);
    return clamp(historyColor, statMin, statMax);
}

float PrevLinearDepth(float d) {
    // glm perspective: view depth = P32 / (ndc + P22), both terms negative.
    return uPrevDepthParams.y / min(d * 2.0 - 1.0 + uPrevDepthParams.x, -1e-7);
}

// Mode 1 history confidence. Static surfaces: the expected previous view depth (camera reprojection
// of this pixel) must lie inside the depth range of the history texels around historyUV. The
// footprint covers both sides of a jittered silhouette, so edges keep their history; a real
// disocclusion (expected depth far from every texel) is rejected. Pixels whose motion vector differs
// from the camera-only reprojection belong to moving objects: their depth change is unknown, so
// only the neighbourhood clamp applies.
float DisocclusionConfidence(vec2 uv, float depth, vec2 historyUV) {
    vec4 world = uInvViewProj * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 prevClip = uPrevViewProj * vec4(world.xyz / world.w, 1.0);
    if (prevClip.w <= 1e-4) return 0.0;
    vec2 cameraPrevUV = prevClip.xy / prevClip.w * 0.5 + 0.5;
    if (length((cameraPrevUV - historyUV) * screenSize) > 1.0) return 1.0;
    // Two diagonal 2x2 gathers cover the 3x3 history texels around historyUV (minus two corners),
    // so both sides of a silhouette are in the footprint whichever side the jitter lands on.
    vec2 halfTexel = 0.5 / screenSize;
    vec4 ha = textureGather(historyDepthBuffer, historyUV - halfTexel, 0);
    vec4 hb = textureGather(historyDepthBuffer, historyUV + halfTexel, 0);
    float dMin = min(min(min(ha.x, ha.y), min(ha.z, ha.w)), min(min(hb.x, hb.y), min(hb.z, hb.w)));
    float dMax = max(max(max(ha.x, ha.y), max(ha.z, ha.w)), max(max(hb.x, hb.y), max(hb.z, hb.w)));
    float zMin = PrevLinearDepth(dMin);
    float zMax = PrevLinearDepth(dMax);
    float expected = prevClip.w;
    float outside = max(zMin - expected, expected - zMax) / max(expected, 1e-4);
    return 1.0 - smoothstep(uDisocclusionTolerance, 2.0 * uDisocclusionTolerance, outside);
}

float ComputeReactiveMask(vec3 currentColor, vec3 historyColor, float edgeMask) {
    float currentLuma = useYCoCg ? currentColor.x : ComputeLuma(currentColor);
    float historyLuma = useYCoCg ? historyColor.x : ComputeLuma(historyColor);
    float lumaDelta = abs(currentLuma - historyLuma);
    float reactive = clamp(lumaDelta * max(lumaWeight, 0.0), 0.0, 1.0);
    if (uTaaMode == 0) {
        reactive = max(reactive, edgeMask * 0.5);
    }
    return clamp(reactive * reactiveMaskStrength, 0.0, 1.0);
}

void main() {
    vec2 uv = TexCoord;
    vec3 currentColor = texture(currentFrame, uv).rgb;

    if (!historyValid) {
        taaResult = currentColor;
        return;
    }

    // Velocity of the closest surface in a 3x3 cross so silhouettes of moving objects carry their
    // motion onto the background pixels next to them (reduces trailing at speed).
    vec2 texel = 1.0 / screenSize;
    vec2 closestUV = uv;
    float closestDepth = texture(depthBuffer, uv).r;
    const vec2 crossOffsets[4] = vec2[](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1));
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUV = uv + crossOffsets[i] * texel;
        float d = texture(depthBuffer, sampleUV).r;
        if (d < closestDepth) {
            closestDepth = d;
            closestUV = sampleUV;
        }
    }
    vec2 velocity = texture(velocityBuffer, closestUV).rg;
    if (closestDepth >= 0.999999) {
        vec4 prevClip = uSkyReprojection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
        vec2 prevUV = (prevClip.xy / max(abs(prevClip.w), 1e-6) * sign(prevClip.w)) * 0.5 + 0.5;
        velocity = uv - prevUV;
    }
    vec2 historyUV = uv - velocity;

    vec2 historyMargin = vec2(1.0) / screenSize;
    if (any(lessThan(historyUV, historyMargin)) || any(greaterThan(historyUV, vec2(1.0) - historyMargin))) {
        taaResult = currentColor;
        return;
    }

    vec3 historyColor = CompressHDR(SampleHistoryCatmullRom(historyUV));
    currentColor = CompressHDR(currentColor);

    float velocityPixels = length(velocity * screenSize);
    float historyConfidence;
    if (uTaaMode == 1) {
        historyConfidence = DisocclusionConfidence(uv, texture(depthBuffer, uv).r, historyUV);
        if (historyConfidence < 0.04) {
            taaResult = DecompressHDR(currentColor);
            return;
        }
    } else {
        float currentDepth = texture(depthBuffer, uv).r;
        float historyDepth = texture(historyDepthBuffer, historyUV).r;
        vec3 currentNormal = DecodeOctNormal01(texture(normalBuffer, uv).rg);
        vec3 historyNormal = DecodeOctNormal01(texture(historyNormalBuffer, historyUV).rg);

        float depthConfidence = exp(-abs(currentDepth - historyDepth) / max(depthThreshold, 1e-5));
        float normalSimilarity = max(dot(currentNormal, historyNormal), 0.0);
        float normalConfidence = smoothstep(normalThreshold, 1.0, normalSimilarity);
        float motionConfidence = exp(-velocityPixels * 0.045);
        historyConfidence = depthConfidence * normalConfidence * mix(0.55, 1.0, motionConfidence);

        if (depthConfidence * normalConfidence < 0.08 || historyConfidence < 0.04) {
            taaResult = DecompressHDR(currentColor);
            return;
        }
    }

    if (useYCoCg) {
        currentColor = RGBToYCoCg(currentColor);
        historyColor = RGBToYCoCg(historyColor);
    }

    vec3 minColor;
    vec3 maxColor;
    vec3 meanColor;
    vec3 variance;
    float edgeMask;
    AnalyzeNeighborhood(uv, minColor, maxColor, meanColor, variance, edgeMask);

    vec3 clampedHistory = ClampHistoryToNeighborhood(historyColor, minColor, maxColor, meanColor, variance);
    float reactiveMask = ComputeReactiveMask(currentColor, clampedHistory, edgeMask);

    float motionResponsiveWeight = clamp(velocityPixels / 42.0, 0.0, uTaaMode == 1 ? uMotionCurrentWeight : 0.60);
    float currentWeight = mix(blendFactor, 0.90, 1.0 - historyConfidence);
    currentWeight = max(currentWeight, motionResponsiveWeight);
    currentWeight = mix(currentWeight, 1.0, reactiveMask);
    currentWeight = clamp(currentWeight, blendFactor, 1.0);

    vec3 result = mix(clampedHistory, currentColor, currentWeight);

    if (useYCoCg) {
        result = YCoCgToRGB(result);
    }

    taaResult = max(vec3(0.0), DecompressHDR(max(result, vec3(0.0))));
}
