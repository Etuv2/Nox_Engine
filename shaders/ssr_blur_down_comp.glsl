#version 460 core
// SSR glossy blur, downsample stage (dual-filter Kawase, Bjorge "Bandwidth-efficient rendering",
// SIGGRAPH 2015). Five taps: the centre (weight 4) and four diagonal taps one texel out (weight 1),
// divided by 8. Writes one level of the blur chain (ssr_composite_comp.glsl reads the chain).
// The first stage reads the temporal history (rgb radiance, a confidence) and premultiplies it, so
// the chain holds rgb*a and a; misses (a = 0) then add nothing to the blur.

layout(local_size_x = 8, local_size_y = 8) in;

layout(rgba16f, binding = 0) uniform writeonly image2D uOut;

uniform sampler2D uSource;
uniform float uSourceLod;   // chain level read (ignored for the history stage)
uniform int uPremultiply;   // 1: uSource is the history texture (rgb radiance, a confidence)

vec4 Tap(vec2 uv) {
	vec4 c = textureLod(uSource, uv, uSourceLod);
	return uPremultiply == 1 ? vec4(c.rgb * c.a, c.a) : c;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 outSize = imageSize(uOut);
	if (any(greaterThanEqual(pixel, outSize))) return;

	vec2 uv = (vec2(pixel) + 0.5) / vec2(outSize);
	vec2 texel = 1.0 / vec2(textureSize(uSource, int(uSourceLod)));
	vec4 sum = Tap(uv) * 4.0;
	sum += Tap(uv + vec2(-texel.x, -texel.y));
	sum += Tap(uv + vec2( texel.x, -texel.y));
	sum += Tap(uv + vec2(-texel.x,  texel.y));
	sum += Tap(uv + vec2( texel.x,  texel.y));
	imageStore(uOut, pixel, sum / 8.0);
}
