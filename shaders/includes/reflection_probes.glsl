#ifndef NOX_REFLECTION_PROBES_GLSL
#define NOX_REFLECTION_PROBES_GLSL

// Local reflection probes (ReflectionProbePass, docs/nfs2015/README.md M5).
// Box-projected (parallax corrected, Lagarde & Zanuttini 2012) cubemaps cross-faded by their
// influence boxes. Returns scene-referred radiance (rgb) and total coverage (a, <= 1).

struct NoxReflectionProbe {
	vec4 boxMin;   // w = blend distance (m)
	vec4 boxMax;   // w = intensity
	vec4 position; // w = cubemap array layer
};
layout(std430, binding = 44) readonly buffer ReflectionProbeBuffer { NoxReflectionProbe reflectionProbeData[]; };
uniform samplerCubeArray reflectionProbes;
uniform int reflectionProbeCount = 0;
uniform float reflectionProbeMaxLOD = 6.0;

vec4 NoxSampleReflectionProbes(vec3 worldPos, vec3 R, float perceptualRoughness) {
	vec3 accum = vec3(0.0);
	float weightSum = 0.0;
	for (int i = 0; i < reflectionProbeCount; ++i) {
		NoxReflectionProbe p = reflectionProbeData[i];
		vec3 inside3 = min(worldPos - p.boxMin.xyz, p.boxMax.xyz - worldPos);
		float inside = min(min(inside3.x, inside3.y), inside3.z);
		if (inside <= 0.0) continue;
		float w = clamp(inside / p.boxMin.w, 0.0, 1.0);
		w = w * w * (3.0 - 2.0 * w);

		// Intersect the reflection ray with the box to find the reflected point, then look it up
		// from the capture position. Rough lobes fade toward the plain direction (less parallax).
		vec3 safeR = R + vec3(equal(R, vec3(0.0))) * 1e-5;
		vec3 t1 = (p.boxMax.xyz - worldPos) / safeR;
		vec3 t2 = (p.boxMin.xyz - worldPos) / safeR;
		vec3 tFar = max(t1, t2);
		float dist = min(min(tFar.x, tFar.y), tFar.z);
		vec3 corrected = worldPos + R * dist - p.position.xyz;
		vec3 dir = normalize(mix(corrected, R, perceptualRoughness * perceptualRoughness));

		vec3 c = textureLod(reflectionProbes, vec4(dir, p.position.w), perceptualRoughness * reflectionProbeMaxLOD).rgb;
		accum += c * p.boxMax.w * w;
		weightSum += w;
	}
	if (weightSum <= 0.0) return vec4(0.0);
	return vec4(accum / weightSum, min(weightSum, 1.0));
}

#endif // NOX_REFLECTION_PROBES_GLSL
