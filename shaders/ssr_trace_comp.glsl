#version 460 core
// SSR step 1: half-resolution stochastic ray trace (docs/nfs2015/README.md, M5).
// One GGX-VNDF importance-sampled ray per half-res pixel (Stachowiak 2015), traced with the
// McGuire & Mara 2014 screen-space DDA against the full-resolution depth buffer.
// Output (RGBA32F): xy = hit UV, z = ray pdf, w = 1 on hit (0 miss / skipped).

layout(local_size_x = 8, local_size_y = 8) in;

#include "includes/ssr_common.glsl"

layout(rgba32f, binding = 0) uniform writeonly image2D uTraceOut;

uniform sampler2D uDepth;        // G-buffer depth (full res)
uniform sampler2D uNormalRough;  // G-buffer RT0: oct normal (rg), roughness (b), metallic (a)
uniform sampler2D uClearCoat;    // G-buffer RT6: clearcoat factor, clearcoat roughness
uniform uint uFrame;
uniform float uMaxDistance;      // metres
uniform int uMaxSteps;
uniform float uThickness;        // metres, scaled with distance

bool TraceScreenSpaceRay(vec3 csOrig, vec3 csDir, float jitter, float stride, out vec2 hitPixel) {
	// Pixel-space projection at full resolution.
	float nearZ = -uProj[3][2] / (uProj[2][2] - 1.0); // view z of the near plane (negative)
	float rayLength = ((csOrig.z + csDir.z * uMaxDistance) > nearZ) ? (nearZ - csOrig.z) / csDir.z : uMaxDistance;
	vec3 csEnd = csOrig + csDir * rayLength;

	vec4 H0 = uProj * vec4(csOrig, 1.0);
	vec4 H1 = uProj * vec4(csEnd, 1.0);
	float k0 = 1.0 / H0.w;
	float k1 = 1.0 / H1.w;
	vec3 Q0 = csOrig * k0;
	vec3 Q1 = csEnd * k1;
	vec2 P0 = (H0.xy * k0 * 0.5 + 0.5) * uFullSize;
	vec2 P1 = (H1.xy * k1 * 0.5 + 0.5) * uFullSize;
	vec2 delta = P1 - P0;
	if (dot(delta, delta) < 1e-4) {
		P1 += vec2(0.01);
		delta = P1 - P0;
	}
	bool permute = false;
	if (abs(delta.x) < abs(delta.y)) {
		permute = true;
		delta = delta.yx;
		P0 = P0.yx;
		P1 = P1.yx;
	}
	float stepDir = sign(delta.x);
	float invdx = stepDir / delta.x;
	// Adaptive stride: spread the step budget over the whole on-screen ray so long grazing
	// reflections (distant lamps on wet asphalt) are reached; hits are refined below.
	stride = max(stride, abs(delta.x) / float(max(uMaxSteps, 1)));
	vec3 dQ = (Q1 - Q0) * invdx;
	float dk = (k1 - k0) * invdx;
	vec2 dP = vec2(stepDir, delta.y * invdx);
	dP *= stride;
	dQ *= stride;
	dk *= stride;
	P0 += dP * jitter;
	Q0 += dQ * jitter;
	k0 += dk * jitter;

	vec4 PQk = vec4(P0, Q0.z, k0);
	vec4 dPQk = vec4(dP, dQ.z, dk);
	float end = P1.x * stepDir;
	float prevZMax = csOrig.z;
	float rayZMin = prevZMax;
	float rayZMax = prevZMax;
	float sceneZ = rayZMax + 1e4;
	hitPixel = vec2(-1.0);
	for (int i = 0; i < uMaxSteps; ++i) {
		if (PQk.x * stepDir > end) break;
		rayZMin = prevZMax;
		rayZMax = (dPQk.z * 0.5 + PQk.z) / (dPQk.w * 0.5 + PQk.w);
		prevZMax = rayZMax;
		if (rayZMin > rayZMax) {
			float t = rayZMin; rayZMin = rayZMax; rayZMax = t;
		}
		vec2 pixel = permute ? PQk.yx : PQk.xy;
		if (any(lessThan(pixel, vec2(0.0))) || any(greaterThanEqual(pixel, uFullSize))) break;
		float depth = texelFetch(uDepth, ivec2(pixel), 0).r;
		if (depth >= 1.0) {
			PQk += dPQk;
			continue; // sky: keep marching, it cannot be hit
		}
		sceneZ = SsrLinearViewZ(depth);
		float thickness = uThickness * max(1.0, -sceneZ * 0.05) * max(1.0, stride * 0.25);
		if (rayZMax >= sceneZ - thickness && rayZMin <= sceneZ) {
			// Binary refinement between the previous and current step (coarse strides overshoot).
			vec4 lo = PQk - dPQk;
			vec4 hi = PQk;
			for (int r = 0; r < 5; ++r) {
				vec4 mid = (lo + hi) * 0.5;
				vec2 midPixel = permute ? mid.yx : mid.xy;
				float midDepth = texelFetch(uDepth, ivec2(clamp(midPixel, vec2(0.0), uFullSize - 1.0)), 0).r;
				float midScene = midDepth >= 1.0 ? -1e6 : SsrLinearViewZ(midDepth);
				float midRayZ = mid.z / mid.w;
				if (midRayZ <= midScene) hi = mid; else lo = mid;
			}
			hitPixel = permute ? hi.yx : hi.xy;
			return true;
		}
		PQk += dPQk;
	}
	return false;
}

void main() {
	ivec2 halfPixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 halfSize = imageSize(uTraceOut);
	if (any(greaterThanEqual(halfPixel, halfSize))) return;

	// Each half-res pixel traces for one full-res pixel of its 2x2 quad, rotating every frame.
	const ivec2 quad[4] = ivec2[](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
	ivec2 fullPixel = min(halfPixel * 2 + quad[(uFrame + uint(halfPixel.x + halfPixel.y * 3)) & 3u], ivec2(uFullSize) - 1);
	vec2 uv = (vec2(fullPixel) + 0.5) / uFullSize;

	float depth = texelFetch(uDepth, fullPixel, 0).r;
	vec4 nr = texelFetch(uNormalRough, fullPixel, 0);
	vec2 cc = texelFetch(uClearCoat, fullPixel, 0).rg;
	// Clear-coated surfaces reflect mostly through the smooth coat: trace with the coat lobe.
	float roughness = cc.r > 0.5 ? cc.g : nr.b;
	if (depth >= 1.0 || roughness > uMaxRoughness) {
		imageStore(uTraceOut, halfPixel, vec4(0.0));
		return;
	}

	vec3 posVS = SsrViewPosFromDepth(uv, depth);
	vec3 N = normalize(mat3(uView) * SsrDecodeOct(nr.rg));
	vec3 V = normalize(-posVS);
	float alpha = max(roughness * roughness, 1e-3);

	// Tangent frame around N.
	vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 T = normalize(cross(up, N));
	vec3 B = cross(N, T);
	vec3 Ve = vec3(dot(V, T), dot(V, B), dot(V, N));
	if (Ve.z <= 0.0) {
		imageStore(uTraceOut, halfPixel, vec4(0.0));
		return;
	}
	float seed = float(uFrame % 64u);
	// R2 low-discrepancy sequence over frames with a per-pixel Cranley-Patterson rotation: successive
	// frames cover the lobe evenly, so the temporal filter converges much faster than with white noise.
	vec2 rotation = vec2(SsrHash(vec2(fullPixel), 0.0), SsrHash(vec2(fullPixel) + 17.3, 3.1));
	vec2 u = fract(rotation + vec2(0.7548776662, 0.5698402910) * float(uFrame % 256u));
	// Bias samples toward the mirror direction (Stachowiak 2015): less noise, slight sharpening.
	u.x *= 0.6;
	vec3 He = SsrSampleGGXVNDF(Ve, alpha, u);
	vec3 H = T * He.x + B * He.y + N * He.z;
	vec3 R = reflect(-V, H);
	if (dot(R, N) <= 0.0) {
		imageStore(uTraceOut, halfPixel, vec4(0.0));
		return;
	}
	float pdf = SsrPdfVNDF(Ve.z, He.z, alpha);

	// Start slightly off the surface; stride grows with roughness (blurry reflections need fewer steps).
	vec3 origin = posVS + N * max(0.01, -posVS.z * 0.002);
	float stride = mix(1.0, 3.0, clamp(roughness * 2.0, 0.0, 1.0)) * 2.0;
	float jitter = SsrHash(vec2(fullPixel) + 5.7, seed + 7.7);
	vec2 hitPixel;
	bool hit = TraceScreenSpaceRay(origin, R, jitter, stride, hitPixel);
	if (!hit) {
		imageStore(uTraceOut, halfPixel, vec4(0.0, 0.0, pdf, 0.0));
		return;
	}
	// Reject hits on surfaces facing away from the ray (back faces / thin geometry).
	vec2 hitUV = (hitPixel + 0.5) / uFullSize;
	vec3 hitN = normalize(mat3(uView) * SsrDecodeOct(texelFetch(uNormalRough, ivec2(hitPixel), 0).rg));
	if (dot(hitN, R) > 0.2) {
		imageStore(uTraceOut, halfPixel, vec4(0.0, 0.0, pdf, 0.0));
		return;
	}
	imageStore(uTraceOut, halfPixel, vec4(hitUV, pdf, 1.0));
}
