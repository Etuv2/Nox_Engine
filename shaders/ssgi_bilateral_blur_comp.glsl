#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D inIndirect;
layout(binding = 1) uniform sampler2D inDirectional;
layout(binding = 2) uniform sampler2D linearDepthQuarter;
layout(binding = 3) uniform sampler2D normalQuarter;

layout(binding = 4, rgba16f) writeonly uniform image2D outIndirect;
layout(binding = 5, rgba16f) writeonly uniform image2D outDirectional;

uniform vec2 invQuarterSize;
uniform float depthSigma;
uniform float normalReject;

float Luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outIndirect);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invQuarterSize;
    vec4 centerI = textureLod(inIndirect, uv, 0.0);
    vec4 centerD = textureLod(inDirectional, uv, 0.0);

    float centerDepth = textureLod(linearDepthQuarter, uv, 0.0).r;
    vec3 centerN = DecodeOctNormal01(textureLod(normalQuarter, uv, 0.0).rg);

    vec3 colorAcc = vec3(0.0);
    vec3 dirAcc = vec3(0.0);
    float confAcc = 0.0;
    float anisoAcc = 0.0;
    float weightAcc = 0.0;

    float centerLuma = Luma(centerI.rgb);

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 ouv = uv + vec2(x, y) * invQuarterSize;
            if (any(lessThan(ouv, vec2(0.0))) || any(greaterThan(ouv, vec2(1.0)))) {
                continue;
            }

            vec4 sampleI = textureLod(inIndirect, ouv, 0.0);
            vec4 sampleD = textureLod(inDirectional, ouv, 0.0);
            float sampleDepth = textureLod(linearDepthQuarter, ouv, 0.0).r;
            vec3 sampleN = DecodeOctNormal01(textureLod(normalQuarter, ouv, 0.0).rg);

            float dz = abs(centerDepth - sampleDepth);
            float depthW = exp(-dz / max(depthSigma, 1e-4));

            float ndot = max(dot(centerN, sampleN), 0.0);
            float normalW = exp(-(1.0 - ndot) / max(normalReject, 1e-4));

            float spatialW = exp(-float(x * x + y * y) / 6.0);
            float confW = mix(0.2, 1.0, clamp(sampleI.a, 0.0, 1.0));

            float lumaDelta = abs(Luma(sampleI.rgb) - centerLuma);
            float varianceW = exp(-lumaDelta * 3.0);

            float w = depthW * normalW * spatialW * confW * varianceW;
            if (w <= 1e-6) {
                continue;
            }

            colorAcc += sampleI.rgb * w;
            dirAcc += (sampleD.rgb * 2.0 - 1.0) * w;
            confAcc += sampleI.a * w;
            anisoAcc += sampleD.a * w;
            weightAcc += w;
        }
    }

    if (weightAcc <= 1e-6) {
        imageStore(outIndirect, id, centerI);
        imageStore(outDirectional, id, centerD);
        return;
    }

    vec3 outColor = colorAcc / weightAcc;
    vec3 outDir = normalize(dirAcc / weightAcc + vec3(1e-5));
    float outConf = clamp(confAcc / weightAcc, 0.0, 1.0);
    float outAniso = clamp(anisoAcc / weightAcc, 0.0, 1.0);

    imageStore(outIndirect, id, vec4(outColor, outConf));
    imageStore(outDirectional, id, vec4(outDir * 0.5 + 0.5, outAniso));
}
