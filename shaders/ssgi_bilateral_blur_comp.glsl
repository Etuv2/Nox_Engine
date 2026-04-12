#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D gDepth;
uniform sampler2D gPackedNormalRM;
layout(binding = 0) uniform sampler2D inTex;
layout(binding = 3, rgba16f) writeonly uniform image2D outTex;

uniform float depthSigma;
uniform float normalThresh;
uniform vec2 invWork;

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outTex);
    if (any(greaterThanEqual(id, size))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invWork;
    vec4 centerColor = textureLod(inTex, uv, 0.0);
    if (centerColor.a < 0.01) {
        imageStore(outTex, id, centerColor);
        return;
    }

    float centerDepth = textureLod(gDepth, uv, 0.0).r;
    vec3 centerNormal = DecodeOctNormal01(textureLod(gPackedNormalRM, uv, 0.0).rg);

    vec3 result = vec3(0.0);
    float weightSum = 0.0;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 sampleUV = uv + vec2(x, y) * invWork;
            vec4 neighborColor = textureLod(inTex, sampleUV, 0.0);
            float neighborDepth = textureLod(gDepth, sampleUV, 0.0).r;
            vec3 neighborNormal = DecodeOctNormal01(textureLod(gPackedNormalRM, sampleUV, 0.0).rg);

            float depthWeight = exp(-abs(centerDepth - neighborDepth) / max(depthSigma, 1e-4));
            float normalWeight = exp(-(1.0 - max(dot(centerNormal, neighborNormal), 0.0)) / max(normalThresh, 1e-4));
            float spatialWeight = exp(-float(x * x + y * y) / 8.0);
            float weight = depthWeight * normalWeight * spatialWeight;

            result += neighborColor.rgb * weight;
            weightSum += weight;
        }
    }

    imageStore(outTex, id, vec4(result / max(weightSum, 1e-5), centerColor.a));
}
