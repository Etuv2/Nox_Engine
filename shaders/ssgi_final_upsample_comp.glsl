#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D inSSGI;
layout(binding = 1) uniform sampler2D depthTex;
layout(binding = 2) uniform sampler2D normalTex;
layout(binding = 3, rgba16f) writeonly uniform image2D outFullRes;

uniform vec2 invDst;
uniform float depthSigma;
uniform float normalThresh;

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dstSize = imageSize(outFullRes);
    if (any(greaterThanEqual(id, dstSize))) return;

    vec2 uvFull = (vec2(id) + 0.5) * invDst;
    float centerDepth = textureLod(depthTex, uvFull, 0.0).r;
    vec3 centerNormal = DecodeOctNormal01(textureLod(normalTex, uvFull, 0.0).rg);

    vec4 result = vec4(0.0);
    float weightSum = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(inSSGI, 0));

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = uvFull + vec2(x, y) * texelSize;
            vec4 sampleValue = textureLod(inSSGI, sampleUV, 0.0);
            float sampleDepth = textureLod(depthTex, sampleUV, 0.0).r;
            vec3 sampleNormal = DecodeOctNormal01(textureLod(normalTex, sampleUV, 0.0).rg);

            float depthWeight = exp(-abs(centerDepth - sampleDepth) / max(depthSigma, 1e-4));
            float normalWeight = exp(-(1.0 - max(dot(centerNormal, sampleNormal), 0.0)) / max(normalThresh, 1e-4));
            float spatialWeight = exp(-float(x * x + y * y) / 3.0);
            float weight = depthWeight * normalWeight * spatialWeight;

            result += sampleValue * weight;
            weightSum += weight;
        }
    }

    imageStore(outFullRes, id, result / max(weightSum, 1e-5));
}
