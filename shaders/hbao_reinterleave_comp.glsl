#version 460 core
// HBAO+ step 3: reinterleave the 16 quarter-res AO layers into the output image and pack the view
// depth next to the AO (rg = ao, view depth) so the blur never re-reads the depth layers.
// The output may be a scaled version of the frame (ssaoResolutionScale); each output texel maps to
// the full-res pixel under its centre.
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rg16f, binding = 0) uniform writeonly image2D uOut;

uniform sampler2DArray uAO;            // step 2 output
uniform sampler2DArray uLinearDepth;   // step 1 output
uniform vec2 uFullSize;                // frame size in pixels

void main() {
    ivec2 q = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(uOut);
    if (any(greaterThanEqual(q, outSize))) {
        return;
    }

    ivec2 f = ivec2(floor((vec2(q) + 0.5) * uFullSize / vec2(outSize)));
    f = clamp(f, ivec2(0), ivec2(uFullSize) - 1);
    ivec2 p = f >> 2;
    int layer = ((f.y & 3) << 2) | (f.x & 3);

    float ao = texelFetch(uAO, ivec3(p, layer), 0).r;
    float viewZ = min(texelFetch(uLinearDepth, ivec3(p, layer), 0).r, 65000.0);
    imageStore(uOut, q, vec4(ao, viewZ, 0.0, 0.0));
}
