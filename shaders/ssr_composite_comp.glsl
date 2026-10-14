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
	float hitFactor = hitDist > 0.0 ? hitDist / (viewDist + hitDist) : 0.5;
	float footprintPx = coneTan * hitFactor * uFullSize.y * 0.6 * uBlurStrength;
	// Chain level 0 is half resolution (one doubling), so full-resolution mip m is chain level m - 1.
	// Roughness below ~0.05 keeps the footprint under a pixel and stays on the sharp history.
	float lod = log2(max(footprintPx, 1.0)) - 1.0;
	lod *= smoothstep(0.03, 0.15, roughness);
	lod = clamp(lod, 0.0, uMaxBlurLevel + 1.0);

	vec4 blurred = textureLod(uChain, uv, max(lod - 1.0, 0.0));
	vec4 pm = mix(sharpPremul, blurred, clamp(lod, 0.0, 1.0));
	vec3 rgb = pm.a > 1e-4 ? pm.rgb / pm.a : vec3(0.0);
	imageStore(uOut, pixel, vec4(rgb, clamp(pm.a, 0.0, 1.0)));
}
