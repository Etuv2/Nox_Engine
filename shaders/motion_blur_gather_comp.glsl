#version 460 core
// Motion blur step 2 (McGuire et al. 2012 reconstruction, Jimenez 2014 refinements): gather along the
// neighbourhood's dominant velocity with soft depth / velocity classification so foreground objects
// blur over the background but sharp background never smears over a sharp foreground.

layout(local_size_x = 8, local_size_y = 8) in;

layout(rgba16f, binding = 0) uniform writeonly image2D uOut;
uniform sampler2D uColor;        // TAA-resolved HDR
uniform sampler2D uVelocity;     // motion vectors (uv units)
uniform sampler2D uDepth;        // window depth
uniform sampler2D uNeighborMax;  // dominant velocity per tile (pixels)
uniform vec2 uFullSize;
uniform int uTileSize;
uniform float uVelocityScale;
uniform float uMaxBlurPixels;
uniform int uSampleCount;
uniform mat4 uProj;
uniform uint uFrame;

float LinearDepth(float d) {
	return d >= 1.0 ? 1e6 : uProj[3][2] / ((d * 2.0 - 1.0) + uProj[2][2]);
}

float Cone(float dist, float velLen) { return clamp(1.0 - dist / max(velLen, 1e-3), 0.0, 1.0); }
float Cylinder(float dist, float velLen) { return 1.0 - smoothstep(0.95 * velLen, 1.05 * velLen, dist); }
float SoftDepthCompare(float za, float zb) { return clamp(1.0 - (za - zb) / max(0.02 * min(za, zb), 0.05), 0.0, 1.0); }

vec2 PixelVelocity(ivec2 p) {
	vec2 v = texelFetch(uVelocity, p, 0).rg * uFullSize * uVelocityScale;
	float len = length(v);
	return len > uMaxBlurPixels ? v * (uMaxBlurPixels / len) : v;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, ivec2(uFullSize)))) return;
	vec3 centerColor = texelFetch(uColor, pixel, 0).rgb;
	vec2 vn = texelFetch(uNeighborMax, pixel / uTileSize, 0).rg;
	float vnLen = length(vn);
	if (vnLen < 0.5) {
		imageStore(uOut, pixel, vec4(centerColor, 1.0));
		return;
	}

	vec2 vc = PixelVelocity(pixel);
	float vcLen = max(length(vc), 0.5);
	float zc = LinearDepth(texelFetch(uDepth, pixel, 0).r);
	// Per-pixel sample offset breaks up banding. This pass runs after TAA, so nothing filters the
	// offset over time: a per-frame random value showed up as speckle on bright blurred edges.
	// Interleaved gradient noise (Jimenez 2014), constant over time, interleaves the samples of
	// neighbouring pixels without flicker (a 2x2 ordered offset left visible stripes).
	float jitter = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715)))) - 0.5;

	float weight = 1.0 / vcLen;
	vec3 sum = centerColor * weight;
	int n = max(uSampleCount, 2);
	for (int i = 0; i < n; ++i) {
		// n samples strictly inside (-1, 1); the centre is already counted above.
		float t = mix(-1.0, 1.0, (float(i) + 0.5 + jitter) / float(n));
		vec2 offset = vn * t * 0.5;
		vec2 samplePos = clamp(vec2(pixel) + 0.5 + offset, vec2(0.5), uFullSize - 0.5);
		ivec2 sp = ivec2(samplePos);
		float dist = length(offset);
		float zs = LinearDepth(texelFetch(uDepth, sp, 0).r);
		vec2 vs = PixelVelocity(sp);
		float vsLen = max(length(vs), 0.5);
		float f = SoftDepthCompare(zc, zs);  // sample in front of the centre
		float b = SoftDepthCompare(zs, zc);  // sample behind the centre
		float w = f * Cone(dist, vsLen) + b * Cone(dist, vcLen) + Cylinder(dist, vsLen) * Cylinder(dist, vcLen) * 2.0;
		// Bilinear colour fetch: sub-pixel sample positions avoid stair-stepped copies of thin lights.
		sum += textureLod(uColor, samplePos / uFullSize, 0.0).rgb * w;
		weight += w;
	}
	imageStore(uOut, pixel, vec4(sum / max(weight, 1e-4), 1.0));
}
