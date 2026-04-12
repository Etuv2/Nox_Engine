#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D curSSGI;
layout(binding = 1) uniform sampler2D prevSSGI;
layout(binding = 2) uniform sampler2D velocityTex;
layout(binding = 3) uniform sampler2D depthTex;
layout(binding = 4) uniform sampler2D normalTex;
layout(binding = 5, rgba16f) writeonly uniform image2D outSSGI;

uniform float alpha;
uniform float depthThreshold;
uniform float normalThreshold;
uniform bool useYCoCg = false;

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

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(outSSGI);
    if (any(greaterThanEqual(id, sz))) return;

    vec2 uvWork = (vec2(id) + 0.5) / vec2(sz);
    vec2 velocity = texture(velocityTex, uvWork).rg;
    vec2 prevUV = uvWork - velocity;

    vec4 current = texture(curSSGI, uvWork);
    bool historyValid =
        all(greaterThanEqual(prevUV, vec2(0.005))) &&
        all(lessThan(prevUV, vec2(0.995)));

    vec4 history = historyValid ? texture(prevSSGI, prevUV) : vec4(0.0);

    if (useYCoCg) {
        current.rgb = RGBToYCoCg(current.rgb);
        history.rgb = RGBToYCoCg(history.rgb);
    }

    float confidence = historyValid ? 1.0 : 0.0;
    if (historyValid) {
        float currDepth = texture(depthTex, uvWork).r;
        float prevDepth = texture(depthTex, prevUV).r;
        vec3 currNormal = DecodeOctNormal01(texture(normalTex, uvWork).rg);
        vec3 prevNormal = DecodeOctNormal01(texture(normalTex, prevUV).rg);

        confidence *= exp(-abs(currDepth - prevDepth) / max(depthThreshold, 1e-4));
        confidence *= exp(-(1.0 - max(dot(currNormal, prevNormal), 0.0)) / max(normalThreshold, 1e-4));
    }

    vec3 minColor = vec3(1e9);
    vec3 maxColor = vec3(-1e9);
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 sampleUV = clamp(uvWork + vec2(i, j) / vec2(sz), vec2(0.0), vec2(1.0));
            vec3 sampleColor = texture(curSSGI, sampleUV).rgb;
            if (useYCoCg) sampleColor = RGBToYCoCg(sampleColor);
            minColor = min(minColor, sampleColor);
            maxColor = max(maxColor, sampleColor);
        }
    }

    vec3 historyClamped = clamp(history.rgb, minColor, maxColor);
    float currentWeight = mix(alpha, 0.75, 1.0 - confidence);
    currentWeight = clamp(currentWeight, 0.05, 0.95);

    vec3 result = mix(historyClamped, current.rgb, currentWeight);
    float outMask = mix(history.a, current.a, currentWeight);

    if (useYCoCg) {
        result = YCoCgToRGB(result);
    }

    imageStore(outSSGI, id, vec4(max(result, vec3(0.0)), clamp(outMask, 0.0, 1.0)));
}
