#version 460 core
// SSR step 2: full-resolution resolve with ray reuse (Stachowiak 2015).
// Every pixel reuses the 4 nearest half-res rays, weighting each hit by this pixel's BRDF over the
// ray's pdf (ratio estimator), and samples last frame's lit colour at the reprojected hit with a
// roughness/distance-dependent mip.
// Output: RGBA16F rgb = reflected radiance (this frame's pre-exposure), a = confidence (hit fraction
// x screen-edge fade); R16F = average hit distance (metres) for temporal reprojection.

layout(local_size_x = 8, local_size_y = 8) in;

#include "includes/ssr_common.glsl"
#include "includes/exposure_common.glsl"

layout(rgba16f, binding = 0) uniform writeonly image2D uResolveOut;
layout(r16f, binding = 1) uniform writeonly image2D uHitDistOut;

uniform sampler2D uTrace;        // half-res trace results
uniform sampler2D uDepth;
uniform sampler2D uNormalRough;
uniform sampler2D uClearCoat;
uniform sampler2D uPrevColor;    // last frame's resolved HDR colour with mips
uniform sampler2D uVelocity;     // current-frame motion vectors (uv delta current - previous)
uniform float uPrevColorMaxMip;
uniform uint uFrame;
uniform int uResolveSamples;     // 4: 2x2 nearest rays (High), 9: 3x3 neighbourhood (Ultra)
uniform float uEdgeFade;         // screen-edge fade width of the hit point (fraction of the screen)

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, ivec2(uFullSize)))) return;
	vec2 uv = (vec2(pixel) + 0.5) / uFullSize;

	float depth = texelFetch(uDepth, pixel, 0).r;
	vec4 nr = texelFetch(uNormalRough, pixel, 0);
	vec2 cc = texelFetch(uClearCoat, pixel, 0).rg;
	float roughness = cc.r > 0.5 ? cc.g : nr.b;
	if (depth >= 1.0 || roughness > uMaxRoughness) {
		imageStore(uResolveOut, pixel, vec4(0.0));
		imageStore(uHitDistOut, pixel, vec4(0.0));
		return;
	}
	vec3 posVS = SsrViewPosFromDepth(uv, depth);
	vec3 N = normalize(mat3(uView) * SsrDecodeOct(nr.rg));
	vec3 V = normalize(-posVS);
	float alpha = max(roughness * roughness, 1e-3);
	float viewDist = length(posVS);

	// Reuse the 3x3 half-res neighbourhood of rays (9 rays per pixel). Neighbours traced for other
	// pixels are valid samples of this pixel's lobe once weighted by this pixel's BRDF / their pdf.
	ivec2 halfSize = textureSize(uTrace, 0);
	ivec2 h = pixel / 2;
	const int kSamples = 9;
	// 2x2 pattern toward this pixel's position inside its half-res cell.
	ivec2 o = ivec2((pixel.x & 1) == 1 ? 1 : -1, (pixel.y & 1) == 1 ? 1 : -1);
	const ivec2 quad[4] = ivec2[](ivec2(0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));

	vec3 colorSum = vec3(0.0);
	float hitWeight = 0.0;
	float validRays = 0.0;   // rays traced for a usable direction
	float hitCoverage = 0.0; // fraction of them that hit on screen (with edge fade)
	float distSum = 0.0;
	float distWeight = 0.0;
	int sampleCount = uResolveSamples >= 9 ? 9 : 4;
	for (int i = 0; i < sampleCount; ++i) {
		ivec2 offset = sampleCount == 9 ? ivec2(i % 3 - 1, i / 3 - 1) : quad[i] * o;
		ivec2 t = clamp(h + offset, ivec2(0), halfSize - 1);
		vec4 tr = texelFetch(uTrace, t, 0);
		float pdf = tr.z;
		if (pdf <= 0.0) continue; // ray not traced (sky / rough / back-facing)
		validRays += 1.0;
		if (tr.w < 0.5) continue; // miss: lowers coverage, probes/sky fill in
		vec2 hitUV = tr.xy;
		float hitDepth = texture(uDepth, hitUV).r;
		vec3 hitVS = SsrViewPosFromDepth(hitUV, hitDepth);
		vec3 L = hitVS - posVS;
		float hitDist = length(L);
		L /= max(hitDist, 1e-4);
		float w = SsrBRDFCos(N, V, L, alpha) / max(pdf, 1e-4);
		w = clamp(w, 0.0, 1e4);
		if (w <= 0.0) continue;

		vec2 velocity = texture(uVelocity, hitUV).rg;
		vec2 prevUV = hitUV - velocity;
		if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) continue;
		// Cone footprint in pixels -> mip (wider for rough surfaces and distant hits).
		// Filtered importance sampling (Stachowiak 2015): fetch with a cone matching the GGX lobe
		// width (~2.5 alpha in reflected-direction slope), so small bright sources are pre-filtered
		// instead of being hit or missed by individual rays (the main source of temporal fizzle).
		float coneTan = max(alpha * 2.5, 0.004);
		float footprintPx = coneTan * hitDist / max(viewDist + hitDist, 0.1) * uFullSize.y * 0.6;
		float mip = clamp(log2(max(footprintPx, 1.0)), 0.0, uPrevColorMaxMip);
		vec3 c = textureLod(uPrevColor, prevUV, mip).rgb;
		// Screen-edge fade of the hit point.
		vec2 edge = smoothstep(vec2(0.0), vec2(uEdgeFade), hitUV) * smoothstep(vec2(0.0), vec2(uEdgeFade), 1.0 - hitUV);
		float fade = edge.x * edge.y;
		// Luminance-weighted average (Karis): a single very bright hit cannot dominate the pixel,
		// which removes most firefly speckle on rough wet asphalt at a small bias.
		float lumaWeight = 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)) * NoxHistoryExposureScale());
		colorSum += c * w * fade * lumaWeight;
		hitWeight += w * fade * lumaWeight;
		hitCoverage += fade;
		distSum += hitDist * w;
		distWeight += w;
	}

	vec4 outColor = vec4(0.0);
	float avgDist = 0.0;
	if (hitWeight > 0.0) {
		outColor.rgb = colorSum / hitWeight * NoxHistoryExposureScale();
		outColor.a = clamp(hitCoverage / max(validRays, 1.0), 0.0, 1.0);
		avgDist = distSum / max(distWeight, 1e-6);
	}
	// Fade out toward the roughness cut-off.
	outColor.a *= 1.0 - smoothstep(uMaxRoughness * 0.75, uMaxRoughness, roughness);
	imageStore(uResolveOut, pixel, outColor);
	imageStore(uHitDistOut, pixel, vec4(avgDist));
}
