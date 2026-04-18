#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D indirectQuarter;
layout(binding = 1) uniform sampler2D directionalQuarter;
layout(binding = 2) uniform sampler2D depthLinearQuarter;
layout(binding = 3) uniform sampler2D horizonDebugQuarter;
layout(binding = 4) uniform sampler2D sectorDebugQuarter;
layout(binding = 5) uniform sampler2D temporalDebugQuarter;
layout(binding = 6) uniform sampler2D depthFull;
layout(binding = 7) uniform sampler2D normalFull;

layout(binding = 8, rgba16f) writeonly uniform image2D outFull;

uniform vec2 invFullSize;
uniform vec2 invQuarterSize;
uniform float projScaleX;
uniform float projScaleY;
uniform float upscaleSharpness;
uniform int debugMode;

float Luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ViewPosFromLinearDepth(vec2 uv, float linearDepth) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(
        ndc.x * linearDepth / max(projScaleX, 1e-5),
        ndc.y * linearDepth / max(projScaleY, 1e-5),
        -linearDepth
    );
}

vec4 SampleDebug(int mode, vec2 uv, vec4 finalValue) {
    if (mode == 1) {
        return textureLod(horizonDebugQuarter, uv, 0.0);
    }
    if (mode == 2) {
        return textureLod(sectorDebugQuarter, uv, 0.0);
    }
    if (mode == 3) {
        return textureLod(indirectQuarter, uv, 0.0);
    }
    if (mode == 4) {
        vec4 d = textureLod(directionalQuarter, uv, 0.0);
        return vec4(d.rgb, 1.0);
    }
    if (mode == 5) {
        float hw = textureLod(temporalDebugQuarter, uv, 0.0).x;
        return vec4(hw, hw, hw, 1.0);
    }
    if (mode == 6) {
        float rej = textureLod(temporalDebugQuarter, uv, 0.0).y;
        return vec4(rej, rej, rej, 1.0);
    }
    if (mode == 7) {
        vec3 denoised = textureLod(indirectQuarter, uv, 0.0).rgb;
        return vec4(denoised, 1.0);
    }
    if (mode == 8) {
        return finalValue;
    }
    return finalValue;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 fullSize = imageSize(outFull);
    if (any(greaterThanEqual(id, fullSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invFullSize;

    float fullDepth = textureLod(depthFull, uv, 0.0).r;
    if (fullDepth >= 0.9999) {
        imageStore(outFull, id, vec4(0.0));
        return;
    }

    vec3 fullNormal = normalize(DecodeOctNormal01(textureLod(normalFull, uv, 0.0).rg));

    vec4 colorAcc = vec4(0.0);
    vec3 dirAcc = vec3(0.0);
    float wAcc = 0.0;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 suv = clamp(uv + vec2(x, y) * invQuarterSize, vec2(0.0), vec2(1.0));

            vec4 ind = textureLod(indirectQuarter, suv, 0.0);
            vec4 dir = textureLod(directionalQuarter, suv, 0.0);

            float lowDepth = textureLod(depthLinearQuarter, suv, 0.0).r;
            vec3 lowPos = ViewPosFromLinearDepth(suv, max(lowDepth, 1e-4));
            float depthAgreement = exp(-abs((-lowPos.z) - max((-lowPos.z), 1e-4)) * 0.0);

            float fullDepthNeighbor = textureLod(depthFull, suv, 0.0).r;
            float fullDepthDelta = abs(fullDepth - fullDepthNeighbor);
            float edgeW = exp(-fullDepthDelta * 350.0 * max(upscaleSharpness, 0.1));

            vec3 nNeighbor = normalize(DecodeOctNormal01(textureLod(normalFull, suv, 0.0).rg));
            float nW = exp(-(1.0 - max(dot(fullNormal, nNeighbor), 0.0)) * (4.0 * upscaleSharpness));

            float spatialW = exp(-float(x * x + y * y) * 0.65);
            float confW = mix(0.15, 1.0, clamp(ind.a, 0.0, 1.0));
            float w = edgeW * nW * spatialW * confW * depthAgreement;

            colorAcc += ind * w;
            dirAcc += (dir.rgb * 2.0 - 1.0) * w;
            wAcc += w;
        }
    }

    if (wAcc <= 1e-6) {
        imageStore(outFull, id, vec4(0.0));
        return;
    }

    vec4 indirect = colorAcc / wAcc;
    vec3 dominantDir = normalize(dirAcc / wAcc + vec3(1e-5));

    float directionalTerm = clamp(dot(fullNormal, dominantDir), 0.0, 1.0);
    float orientedGain = mix(0.55, 1.25, directionalTerm);

    vec3 finalIndirect = clamp(indirect.rgb * orientedGain, vec3(0.0), vec3(8.0));
    vec4 finalOut = vec4(finalIndirect, clamp(indirect.a, 0.0, 1.0));

    vec4 debugOut = SampleDebug(debugMode, uv, finalOut);
    imageStore(outFull, id, vec4(max(debugOut.rgb, vec3(0.0)), finalOut.a));
}
