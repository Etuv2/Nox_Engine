#version 460 core
// HBAO+ step 1: linearize the full-resolution gbuffer depth and deinterleave it into 4x4 = 16
// quarter-resolution layers (Bavoil & Sainz; NVIDIA HBAO+). Layer l = 4 * oy + ox holds the
// full-res pixels whose coordinates are (ox, oy) modulo 4, so each layer is a small image that
// spans the whole screen and the horizon search reads it with good cache locality.
// Output: R32F view-space depth (positive, metres along the view axis; 0 = sky / no geometry).
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(r32f, binding = 0) uniform writeonly image2DArray uLinearDepthOut;

uniform sampler2D uDepth;   // gbuffer depth (standard 0..1)
uniform mat4 uProj;

void main() {
    ivec2 q = ivec2(gl_GlobalInvocationID.xy);
    ivec2 quarterSize = imageSize(uLinearDepthOut).xy;
    if (any(greaterThanEqual(q, quarterSize))) {
        return;
    }
    ivec2 fullSize = textureSize(uDepth, 0);

    for (int layer = 0; layer < 16; ++layer) {
        ivec2 offset = ivec2(layer & 3, layer >> 2);
        ivec2 fp = min(q * 4 + offset, fullSize - 1);
        float depth01 = texelFetch(uDepth, fp, 0).r;
        float linearZ = 0.0;
        if (depth01 < 0.99999) {
            linearZ = max(-LinearizeDepthFromProjection(depth01, uProj), 1e-4);
        }
        imageStore(uLinearDepthOut, ivec3(q, layer), vec4(linearZ, 0.0, 0.0, 0.0));
    }
}
