#version 460 core
// SSR step 3: temporal accumulation (docs/nfs2015/README.md, M5).
// Reprojects through the reflected (virtual) point: a mirror reflection moves with the reflected
// object, not with the reflecting surface (Stachowiak 2015 "hit-depth reprojection"). The history is
// clamped to the 3x3 neighbourhood of the current resolve in a luminance-compressed space.

layout(local_size_x = 8, local_size_y = 8) in;

#include "includes/ssr_common.glsl"
#include "includes/exposure_common.glsl"

layout(rgba16f, binding = 0) uniform writeonly image2D uOut;

uniform sampler2D uCurrent;   // resolve output
uniform sampler2D uHitDist;
uniform sampler2D uHistory;   // previous temporal output (previous pre-exposure)
uniform sampler2D uDepth;
uniform sampler2D uVelocity;
uniform sampler2D uNormalRough;
uniform sampler2D uClearCoat;
uniform mat4 uPrevViewProj;   // unjittered previous view-projection
uniform bool uHistoryValid;
uniform float uBlend;         // weight of the current frame

vec4 Compress(vec4 c) { return vec4(c.rgb / (1.0 + dot(c.rgb, vec3(0.2126, 0.7152, 0.0722))), c.a); }
vec4 Decompress(vec4 c) { return vec4(c.rgb / max(1.0 - dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-4), c.a); }

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, ivec2(uFullSize)))) return;
	vec2 uv = (vec2(pixel) + 0.5) / uFullSize;

	vec4 current = texelFetch(uCurrent, pixel, 0);
	float depth = texelFetch(uDepth, pixel, 0).r;
	if (!uHistoryValid || depth >= 1.0) {
		imageStore(uOut, pixel, current);
		return;
	}

	// Variance clipping (Salvi 2016) against the current resolve: mean +- 1.5 sigma in compressed
	// space. A hard min/max clamp would follow the per-pixel noise of the stochastic resolve.
	vec4 m1 = vec4(0.0);
	vec4 m2 = vec4(0.0);
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			vec4 s = Compress(texelFetch(uCurrent, clamp(pixel + ivec2(x, y), ivec2(0), ivec2(uFullSize) - 1), 0));
			m1 += s;
			m2 += s * s;
		}
	}
	m1 /= 9.0;
	vec4 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec4(0.0)));
	vec4 cMin = m1 - 1.5 * sigma;
	vec4 cMax = m1 + 1.5 * sigma;

	float hitDist = texelFetch(uHitDist, pixel, 0).r;
	vec4 nr = texelFetch(uNormalRough, pixel, 0);
	vec2 cc = texelFetch(uClearCoat, pixel, 0).rg;
	float roughness = cc.r > 0.5 ? cc.g : nr.b;
	vec2 surfaceVelocity = texelFetch(uVelocity, pixel, 0).rg;
	vec2 prevUV;
	// Mirror-like reflections move with the reflected object (virtual point); glossy ones are
	// dominated by the surface itself, where the noisy average hit distance would only add jitter.
	if (hitDist > 0.0 && roughness < 0.2) {
		vec3 posVS = SsrViewPosFromDepth(uv, depth);
		vec3 virtualVS = posVS + normalize(posVS) * hitDist;
		vec4 prevClip = uPrevViewProj * (uInvView * vec4(virtualVS, 1.0));
		prevUV = prevClip.xy / prevClip.w * 0.5 + 0.5;
	} else {
		prevUV = uv - surfaceVelocity;
	}
	if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
		imageStore(uOut, pixel, current);
		return;
	}
	vec4 history = texture(uHistory, prevUV);
	history.rgb *= NoxHistoryExposureScale();
	vec4 h = clamp(Compress(history), cMin, cMax);
	// Accumulate longer when the view is stable; respond faster under motion.
	float motionPixels = length(surfaceVelocity * uFullSize);
	float blend = clamp(uBlend * mix(0.35, 2.0, clamp(motionPixels / 16.0, 0.0, 1.0)), 0.03, 1.0);
	vec4 result = mix(h, Compress(current), blend);
	imageStore(uOut, pixel, Decompress(result));
}
