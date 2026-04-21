#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D gDepth;
layout(binding = 1, rg16f) writeonly uniform image2D outNormalQuarter;

uniform mat4 invProj;

bool ReconstructViewPos(ivec2 src, ivec2 srcSize, out vec3 viewPos, out float depth01) {
    src = clamp(src, ivec2(0), srcSize - ivec2(1));
    depth01 = texelFetch(gDepth, src, 0).r;
    if (depth01 >= 0.999999) {
        viewPos = vec3(0.0);
        return false;
    }

    vec2 uv = (vec2(src) + 0.5) / vec2(srcSize);
    viewPos = ReconstructViewPosition(uv, depth01, invProj);
    return true;
}

vec3 ReconstructOrFallback(ivec2 src, ivec2 srcSize, vec3 fallback) {
    vec3 viewPos;
    float depth01;
    return ReconstructViewPos(src, srcSize, viewPos, depth01) ? viewPos : fallback;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outNormalQuarter);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    ivec2 srcSize = textureSize(gDepth, 0);
    ivec2 base = id * 4;

    ivec2 anchor = clamp(base + ivec2(1, 1), ivec2(0), srcSize - ivec2(1));
    float bestDepth = NOX_FP16_MAX;
    float anchorDepth01 = 1.0;

    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 src = clamp(base + ivec2(x, y), ivec2(0), srcSize - ivec2(1));
            vec3 viewPos;
            float depth01;
            if (!ReconstructViewPos(src, srcSize, viewPos, depth01)) {
                continue;
            }

            float linearDepth = max(-viewPos.z, 1e-4);
            if (linearDepth < bestDepth) {
                bestDepth = linearDepth;
                anchor = src;
                anchorDepth01 = depth01;
            }
        }
    }

    if (bestDepth >= NOX_FP16_MAX) {
        imageStore(outNormalQuarter, id, vec4(0.5, 0.5, 0.0, 0.0));
        return;
    }

    vec2 anchorUV = (vec2(anchor) + 0.5) / vec2(srcSize);
    vec3 centerVS = ReconstructViewPosition(anchorUV, anchorDepth01, invProj);
    if (any(isnan(centerVS)) || any(isinf(centerVS))) {
        imageStore(outNormalQuarter, id, vec4(0.5, 0.5, 0.0, 0.0));
        return;
    }

    vec3 vx0 = centerVS - ReconstructOrFallback(anchor + ivec2(1, 0), srcSize, centerVS);
    vec3 vy0 = centerVS - ReconstructOrFallback(anchor + ivec2(0, 1), srcSize, centerVS);
    vec3 vx1 = ReconstructOrFallback(anchor - ivec2(1, 0), srcSize, centerVS) - centerVS;
    vec3 vy1 = ReconstructOrFallback(anchor - ivec2(0, 1), srcSize, centerVS) - centerVS;

    vec3 vx = (abs(vx0.z) < abs(vx1.z)) ? vx0 : vx1;
    vec3 vy = (abs(vy0.z) < abs(vy1.z)) ? vy0 : vy1;

    vec3 normalVS = normalize(cross(vy, vx));
    if (any(isnan(normalVS)) || any(isinf(normalVS)) || length(normalVS) < 0.5) {
        normalVS = vec3(0.0, 0.0, 1.0);
    }

    vec3 viewDir = normalize(-centerVS);
    if (dot(normalVS, viewDir) < 0.0) {
        normalVS = -normalVS;
    }

    imageStore(outNormalQuarter, id, vec4(EncodeOctNormal01(normalVS), 0.0, 0.0));
}
