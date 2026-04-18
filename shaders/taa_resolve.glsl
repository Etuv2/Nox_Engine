#version 460 core

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

vec3 DecodeNormal(vec2 encoded) {
    encoded = encoded * 2.0 - 1.0;
    vec3 n = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    if (n.z < 0.0) {
        vec2 signNotZero = vec2(sign(encoded.x), sign(encoded.y));
        n.xy = (1.0 - abs(n.yx)) * signNotZero;
    }
    return normalize(n);
}

float ComputeLuma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

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
            vec3 color = texture(currentFrame, sampleUV).rgb;
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

float ComputeReactiveMask(vec3 currentColor, vec3 historyColor, float edgeMask) {
    float currentLuma = useYCoCg ? currentColor.x : ComputeLuma(currentColor);
    float historyLuma = useYCoCg ? historyColor.x : ComputeLuma(historyColor);
    float lumaDelta = abs(currentLuma - historyLuma);
    float reactive = clamp(lumaDelta * max(lumaWeight, 0.0), 0.0, 1.0);
    reactive = max(reactive, edgeMask * 0.5);
    return clamp(reactive * reactiveMaskStrength, 0.0, 1.0);
}

void main() {
    vec2 uv = TexCoord;
    vec3 currentColor = texture(currentFrame, uv).rgb;

    if (!historyValid) {
        taaResult = currentColor;
        return;
    }

    vec2 velocity = texture(velocityBuffer, uv).rg;
    vec2 historyUV = uv - velocity;

    vec2 historyMargin = vec2(1.0) / screenSize;
    if (any(lessThan(historyUV, historyMargin)) || any(greaterThan(historyUV, vec2(1.0) - historyMargin))) {
        taaResult = currentColor;
        return;
    }

    vec3 historyColor = SampleHistoryCatmullRom(historyUV);

    float currentDepth = texture(depthBuffer, uv).r;
    float historyDepth = texture(historyDepthBuffer, historyUV).r;
    vec3 currentNormal = DecodeNormal(texture(normalBuffer, uv).rg);
    vec3 historyNormal = DecodeNormal(texture(historyNormalBuffer, historyUV).rg);

    float depthConfidence = exp(-abs(currentDepth - historyDepth) / max(depthThreshold, 1e-5));
    float normalSimilarity = max(dot(currentNormal, historyNormal), 0.0);
    float normalConfidence = smoothstep(normalThreshold, 1.0, normalSimilarity);
    float velocityPixels = length(velocity * screenSize);
    float motionConfidence = exp(-velocityPixels * 0.15);
    float historyConfidence = depthConfidence * normalConfidence * motionConfidence;

    if (historyConfidence < 0.15) {
        taaResult = currentColor;
        return;
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

    float currentWeight = mix(blendFactor, 1.0, 1.0 - historyConfidence);
    currentWeight = mix(currentWeight, 1.0, reactiveMask);
    currentWeight = clamp(currentWeight, blendFactor, 1.0);

    vec3 result = mix(clampedHistory, currentColor, currentWeight);

    if (useYCoCg) {
        result = YCoCgToRGB(result);
    }

    taaResult = max(vec3(0.0), result);
}
