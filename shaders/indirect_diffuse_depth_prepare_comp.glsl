#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D gDepth;
layout(binding = 2, r16f) writeonly uniform image2D outLinearDepthQuarter;

uniform mat4 invProj;

float LinearDepthFromDepth01(vec2 uv, float depth01) {
    if (depth01 >= 0.999999) {
        return NOX_FP16_MAX;
    }
    return ViewDepthFromDeviceDepth(uv, depth01, invProj);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outLinearDepthQuarter);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    ivec2 srcSize = textureSize(gDepth, 0);
    ivec2 base = id * 4;

    float minDepth = NOX_FP16_MAX;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 src = clamp(base + ivec2(x, y), ivec2(0), srcSize - ivec2(1));
            vec2 uv = (vec2(src) + 0.5) / vec2(srcSize);
            float linearDepth = LinearDepthFromDepth01(uv, texelFetch(gDepth, src, 0).r);
            if (linearDepth < minDepth) {
                minDepth = linearDepth;
            }
        }
    }

    imageStore(outLinearDepthQuarter, id, vec4(minDepth, 0.0, 0.0, 0.0));
}
