#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D curIndirect;
layout(binding = 1) uniform sampler2D prevIndirect;
layout(binding = 2) uniform sampler2D curDirectional;
layout(binding = 3) uniform sampler2D prevDirectional;
layout(binding = 4) uniform sampler2D velocityTex;
layout(binding = 5) uniform sampler2D depthTex;
layout(binding = 6) uniform sampler2D normalTex;
layout(binding = 7) uniform sampler2D prevDepthTex;
layout(binding = 8) uniform sampler2D prevNormalTex;

layout(binding = 9, rgba16f) writeonly uniform image2D outIndirect;
layout(binding = 10, rgba16f) writeonly uniform image2D outDirectional;
layout(binding = 11, rgba16f) writeonly uniform image2D outTemporalDebug;

uniform float alpha;
uniform float depthThreshold;
uniform float normalThreshold;
uniform float temporalResponse;

float Luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outIndirect);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) / vec2(outSize);
    vec2 velocity = texture(velocityTex, uv).rg;
    vec2 prevUV = uv - velocity;

    vec4 curI = texture(curIndirect, uv);
    vec4 curD = texture(curDirectional, uv);

    bool historyValid =
        all(greaterThanEqual(prevUV, vec2(0.001))) &&
        all(lessThanEqual(prevUV, vec2(0.999)));

    if (!historyValid) {
        imageStore(outIndirect, id, curI);
        imageStore(outDirectional, id, curD);
        imageStore(outTemporalDebug, id, vec4(0.0, 1.0, 0.0, 1.0));
        return;
    }

    vec4 prevI = texture(prevIndirect, prevUV);
    vec4 prevD = texture(prevDirectional, prevUV);

    float currDepth = texture(depthTex, uv).r;
    float prevDepth = texture(prevDepthTex, prevUV).r;
    vec3 currNormal = DecodeOctNormal01(texture(normalTex, uv).rg);
    vec3 prevNormal = DecodeOctNormal01(texture(prevNormalTex, prevUV).rg);

    float depthReject = exp(-abs(currDepth - prevDepth) / max(depthThreshold, 1e-5));
    float normalSim = max(dot(normalize(currNormal), normalize(prevNormal)), 0.0);
    float normalReject = smoothstep(normalThreshold, 1.0, normalSim);

    float motionPixels = length(velocity * vec2(textureSize(depthTex, 0)));
    float motionReject = exp(-motionPixels * 0.10);

    float currConf = clamp(curI.a, 0.0, 1.0);
    float prevConf = clamp(prevI.a, 0.0, 1.0);

    float confidence = depthReject * normalReject * motionReject;
    confidence *= mix(0.35, 1.0, prevConf);
    confidence = clamp(confidence, 0.0, 1.0);

    float reactive = abs(Luma(curI.rgb) - Luma(prevI.rgb));
    reactive = clamp(reactive * 1.35, 0.0, 1.0);

    float baseAlpha = clamp(alpha, 0.02, 0.35);
    float response = clamp(temporalResponse, 0.0, 1.0);
    float currentWeight = clamp(baseAlpha + (1.0 - confidence) * mix(0.35, 0.75, response) + reactive * 0.4, 0.08, 0.98);
    float historyWeight = clamp((1.0 - currentWeight) * confidence, 0.0, 0.92);

    vec3 indirect = (curI.rgb * currentWeight + prevI.rgb * historyWeight) / max(currentWeight + historyWeight, 1e-5);
    indirect = clamp(indirect, vec3(0.0), vec3(8.0));

    vec3 curDir = normalize(curD.rgb * 2.0 - 1.0);
    vec3 prevDir = normalize(prevD.rgb * 2.0 - 1.0);
    vec3 fusedDir = normalize(curDir * currentWeight + prevDir * historyWeight + vec3(1e-5));

    float fusedAnisotropy = clamp(mix(prevD.a, curD.a, currentWeight), 0.0, 1.0);
    float outConfidence = clamp(mix(prevConf, currConf, currentWeight), 0.0, 1.0);

    imageStore(outIndirect, id, vec4(indirect, outConfidence));
    imageStore(outDirectional, id, vec4(fusedDir * 0.5 + 0.5, fusedAnisotropy));

    // x = history weight, y = disocclusion reject amount, z = motion confidence
    imageStore(outTemporalDebug, id, vec4(historyWeight, 1.0 - confidence, motionReject, 1.0));
}
