#version 460 core
#include "includes/screen_space_reconstruction.glsl"

in vec2 TexCoord;
out float FragColor;

uniform sampler2D ssaoInput;
uniform sampler2D gDepth;
uniform sampler2D gPackedNormalRM;

uniform vec2 inputTexelSize;
uniform vec2 fullResTexelSize;
uniform float depthThreshold = 0.005;
uniform float normalThreshold = 0.1;

void main() {
    float centerScalar = texture(ssaoInput, TexCoord).r;
    float centerDepth = texture(gDepth, TexCoord).r;

    if (centerDepth >= 0.9999) {
        FragColor = 1.0;
        return;
    }

    vec3 centerNormal = DecodeOctNormal01(texture(gPackedNormalRM, TexCoord).rg);

    float result = 0.0;
    float weightSum = 0.0;

    const int kernelRadius = 1;
    for (int x = -kernelRadius; x <= kernelRadius; ++x) {
        for (int y = -kernelRadius; y <= kernelRadius; ++y) {
            vec2 sampleUV = TexCoord + vec2(x, y) * inputTexelSize;
            if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
                continue;
            }

            float sampleScalar = texture(ssaoInput, sampleUV).r;
            float sampleDepth = texture(gDepth, sampleUV).r;
            vec3 sampleNormal = DecodeOctNormal01(texture(gPackedNormalRM, sampleUV).rg);

            float depthDiff = abs(centerDepth - sampleDepth);
            float normalDot = max(0.0, dot(centerNormal, sampleNormal));
            float spatialWeight = exp(-float(x * x + y * y) / 4.0);
            float depthWeight = exp(-depthDiff / max(depthThreshold, 1e-4));
            float normalWeight = pow(normalDot, 16.0);
            float weight = spatialWeight * depthWeight * normalWeight;

            result += sampleScalar * weight;
            weightSum += weight;
        }
    }

    float resolved = (weightSum > 1e-5) ? (result / weightSum) : centerScalar;
    FragColor = clamp(resolved, 0.0, 1.0);
}
