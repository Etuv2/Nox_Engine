#version 460 core
// SSR step 4: composite (docs/nfs2015/README.md, M5). Chooses the glossy blur per pixel and writes the
// reflection the lighting pass reads.
// The blur level follows the cone the GGX lobe subtends at the receiving pixel: the same cone as the
// resolve's filtered fetch (2.5 alpha), scaled by the hit distance over the total path length, so a
// rough floor reflecting a nearby car is blurred more than a far wall. Mirror-smooth surfaces stay on
// the sharp temporal history. Between two levels the sharp and blurred premultiplied values are mixed,
// so the blur ramps in without visible steps.
// Output: RGBA16F rgb = reflected radiance (this frame's pre-exposure), a = confidence.

layout(local_size_x = 8, local_size_y = 8) in;

#include "includes/ssr_common.glsl"

layout(rgba16f, binding = 0) uniform writeonly image2D uOut;

uniform sampler2D uHistory;      // temporal output: rgb radiance, a confidence (sharp)
uniform sampler2D uChain;        // blur chain: rgb*a, a (level 0 = half res)
uniform sampler2D uHitDist;      // resolve's average hit distance (metres), full res
uniform sampler2D uDepth;
uniform sampler2D uNormalRough;  // G-buffer RT0: roughness in .b
uniform sampler2D uClearCoat;    // G-buffer RT6
uniform float uBlurStrength;     // scales the footprint (0 = always sharp)
uniform float uMaxBlurLevel;     // coarsest chain level allowed (0..4)
uniform int uDebugLevel;         // NOX_SSR_DEBUG_VIEW=4: colour ramp of the chosen level, 5: grey (L + 2) / 8

// Colour ramp for the chosen level L (-1 = sharp history, 0..4 = chain levels).
vec3 LevelColour(float L) {
	const vec3 ramp[6] = vec3[6](vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0),
		vec3(1.0, 1.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 0.0, 0.0));
	float t = clamp(L + 1.0, 0.0, 5.0);
	int i = min(int(t), 4);
	return mix(ramp[i], ramp[i + 1], t - float(i));
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, ivec2(uFullSize)))) return;
	vec2 uv = (vec2(pixel) + 0.5) / uFullSize;

	float depth = texelFetch(uDepth, pixel, 0).r;
	vec4 nr = texelFetch(uNormalRough, pixel, 0);
	vec2 cc = texelFetch(uClearCoat, pixel, 0).rg;
	float roughness = cc.r > 0.5 ? cc.g : nr.b;
	if (depth >= 1.0 || roughness > uMaxRoughness) {
		imageStore(uOut, pixel, vec4(0.0));
		return;
	}

	vec4 sharp = texelFetch(uHistory, pixel, 0);
	vec4 sharpPremul = vec4(sharp.rgb * sharp.a, sharp.a);

	// Footprint in full-resolution pixels, same model as the resolve's cone fetch.
	float viewDist = length(SsrViewPosFromDepth(uv, depth));
	float hitDist = texelFetch(uHitDist, pixel, 0).r;
	float alpha = max(roughness * roughness, 1e-3);
	float coneTan = max(alpha * 2.5, 0.004);
	// The hit-distance term only modulates the cone: per-pixel hit distances vary by several levels
	// inside one roughness band, which would hide the roughness response.
	float hitFactor = hitDist > 0.0 ? clamp(hitDist / (viewDist + hitDist), 0.25, 0.5) : 0.4;
	float footprintPx = coneTan * hitFactor * uFullSize.y * 0.6 * uBlurStrength;
	// Chain level L: level 0 is half resolution, so the full-resolution mip log2(footprint) is level
	// L = log2(footprint) - 1. Mirror-smooth pixels (roughness below ~0.03) get L = -1 and stay sharp;
	// the sharp history mixes into level 0 over L in [-1, 0], so there is no step at the first level.
	float L = log2(max(footprintPx, 1.0)) - 1.0;
	L = mix(-1.0, L, smoothstep(0.03, 0.15, roughness));
	L = min(L, uMaxBlurLevel);

	vec4 blurred = textureLod(uChain, uv, max(L, 0.0));
	vec4 pm = mix(sharpPremul, blurred, clamp(L + 1.0, 0.0, 1.0));
	if (uDebugLevel == 1) {
		imageStore(uOut, pixel, vec4(pm.a > 1e-3 ? LevelColour(L) : vec3(0.0), 1.0));
		return;
	}
	if (uDebugLevel == 2) {
		imageStore(uOut, pixel, vec4(pm.a > 1e-3 ? vec3((L + 2.0) / 8.0) : vec3(0.0), 1.0));
		return;
	}
	vec3 rgb = pm.a > 1e-4 ? pm.rgb / pm.a : vec3(0.0);
	imageStore(uOut, pixel, vec4(rgb, clamp(pm.a, 0.0, 1.0)));
}
