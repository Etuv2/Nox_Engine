#version 460 core
// SSR glossy blur, upsample stage (dual-filter Kawase, Bjorge "Bandwidth-efficient rendering",
// SIGGRAPH 2015). Eight taps around the coarser level: the four axis taps two texels out (weight 1)
// and the four diagonal taps one texel out (weight 2), divided by 12. The result is averaged with
// the same level's downsample value, which keeps the chain from drifting brighter.
// Reads and writes levels of one texture (chain): each invocation reads only its own texel of the
// level it writes, and the next level up was finished by the previous dispatch (barrier in between).

layout(local_size_x = 8, local_size_y = 8) in;

layout(rgba16f, binding = 0) uniform writeonly image2D uOut;

uniform sampler2D uChain;
uniform float uUpperLod;  // level k+1: the coarser, already upsampled level
uniform float uOwnLod;    // level k: this level's downsample value

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 outSize = imageSize(uOut);
	if (any(greaterThanEqual(pixel, outSize))) return;

	vec2 uv = (vec2(pixel) + 0.5) / vec2(outSize);
	vec2 h = 1.0 / vec2(textureSize(uChain, int(uUpperLod)));
	vec4 up = textureLod(uChain, uv + vec2(-2.0 * h.x, 0.0), uUpperLod)
	        + textureLod(uChain, uv + vec2(2.0 * h.x, 0.0), uUpperLod)
	        + textureLod(uChain, uv + vec2(0.0, -2.0 * h.y), uUpperLod)
	        + textureLod(uChain, uv + vec2(0.0, 2.0 * h.y), uUpperLod);
	up += 2.0 * (textureLod(uChain, uv + vec2(-h.x,  h.y), uUpperLod)
	           + textureLod(uChain, uv + vec2( h.x,  h.y), uUpperLod)
	           + textureLod(uChain, uv + vec2( h.x, -h.y), uUpperLod)
	           + textureLod(uChain, uv + vec2(-h.x, -h.y), uUpperLod));
	up /= 12.0;
	vec4 own = textureLod(uChain, uv, uOwnLod);
	imageStore(uOut, pixel, 0.5 * (up + own));
}
