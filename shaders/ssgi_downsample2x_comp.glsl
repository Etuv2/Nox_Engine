#version 460 core

// 2x downsample with simple 4-tap box filter (operates on half-res to produce quarter-res)
// Input: sampler2D inTex (half-res)
// Output: image2D outTex (quarter-res)

layout (local_size_x = 8, local_size_y = 8) in;

layout (binding = 0) uniform sampler2D inTex;          // half-res input
layout (binding = 1, rgba16f) writeonly uniform image2D outTex; // quarter-res output

uniform vec2 invSrc; // 1.0 / source resolution (half-res)

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dstSize = imageSize(outTex);
    if (any(greaterThanEqual(gid, dstSize))) return;

    // Map destination pixel to normalized UV
    vec2 uv = (vec2(gid) + 0.5) / vec2(dstSize);

    // 4-tap box on source (offsets half a texel to gather 2x2)
    vec2 o = 0.5 * invSrc;
    vec3 c = vec3(0.0);
    c += textureLod(inTex, uv + vec2(-o.x, -o.y), 0.0).rgb;
    c += textureLod(inTex, uv + vec2( o.x, -o.y), 0.0).rgb;
    c += textureLod(inTex, uv + vec2(-o.x,  o.y), 0.0).rgb;
    c += textureLod(inTex, uv + vec2( o.x,  o.y), 0.0).rgb;
    c *= 0.25;

    imageStore(outTex, gid, vec4(c, 1.0));
}

