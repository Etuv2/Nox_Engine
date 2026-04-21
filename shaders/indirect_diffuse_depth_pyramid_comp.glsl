#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D depthPyramid;
layout(binding = 1, r16f) writeonly uniform image2D outDepthMip;

uniform int srcMip;

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outDepthMip);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    ivec2 srcSize = textureSize(depthPyramid, srcMip);
    ivec2 base = id * 2;

    float minDepth = NOX_FP16_MAX;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            ivec2 src = clamp(base + ivec2(x, y), ivec2(0), srcSize - ivec2(1));
            float depth = texelFetch(depthPyramid, src, srcMip).r;
            if (depth < minDepth) {
                minDepth = depth;
            }
        }
    }

    imageStore(outDepthMip, id, vec4(minDepth, 0.0, 0.0, 0.0));
}
