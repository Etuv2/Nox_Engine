#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D gDepth;
layout(binding = 1) uniform sampler2D gPackedNormalRM;
layout(binding = 2, r16f) writeonly uniform image2D outLinearDepthQuarter;
layout(binding = 3, rg16f) writeonly uniform image2D outNormalQuarter;

uniform vec2 invQuarterSize;
uniform mat4 invProj;

vec2 EncodeOctNormal01(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-6);
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy + vec2(1e-6));
    }
    return n.xy * 0.5 + 0.5;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 qSize = imageSize(outLinearDepthQuarter);
    if (any(greaterThanEqual(id, qSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invQuarterSize;

    float bestDepth01 = 1.0;
    vec3 bestNormal = vec3(0.0, 0.0, 1.0);

    vec2 texel = 1.0 / vec2(textureSize(gDepth, 0));
    for (int oy = -1; oy <= 0; ++oy) {
        for (int ox = -1; ox <= 0; ++ox) {
            vec2 suv = clamp(uv + vec2(ox, oy) * texel, vec2(0.0), vec2(1.0));
            float d = textureLod(gDepth, suv, 0.0).r;
            if (d < bestDepth01) {
                bestDepth01 = d;
                bestNormal = DecodeOctNormal01(textureLod(gPackedNormalRM, suv, 0.0).rg);
            }
        }
    }

    if (bestDepth01 >= 0.9999) {
        imageStore(outLinearDepthQuarter, id, vec4(65504.0, 0.0, 0.0, 0.0));
        imageStore(outNormalQuarter, id, vec4(0.5, 0.5, 0.0, 0.0));
        return;
    }

    vec3 viewPos = ReconstructViewPosition(uv, bestDepth01, invProj);
    float linearDepth = max(-viewPos.z, 1e-4);
    vec2 packedNormal = EncodeOctNormal01(normalize(bestNormal));

    imageStore(outLinearDepthQuarter, id, vec4(linearDepth, 0.0, 0.0, 0.0));
    imageStore(outNormalQuarter, id, vec4(packedNormal, 0.0, 0.0));
}
