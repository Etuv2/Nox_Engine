#version 460 core
// VehicleFX shading (VehicleFXPass). Premultiplied output into the pre-exposed HDR target.
// Light: scene ambient radiance plus the froxel fog volume's in-scattered light, the same lamp and
// headlight field RainPass uses, so smoke glows inside lamp cones. Smoke is warm-tinted (sodium and
// tungsten street light); spray droplets and mist are neutral and mostly lit by nearby lights.

#include "includes/volumetric_common.glsl"
#include "includes/exposure_common.glsl"

in vec2 vCorner;
in vec3 vWorldPos;
in float vViewDepth;
in float vOpacity;
in float vSeed;
in float vAge;
flat in int vKind;
layout(location = 0) out vec4 FragColor;

uniform sampler3D uScattering;
uniform bool uHasVolume;
uniform mat4 uProj;
uniform mat4 uViewProj;
uniform sampler2D uSceneDepth;  // G-buffer depth for the soft-particle fade
uniform vec2 uScreenSize;
uniform float uSoftRange;       // metres
uniform vec3 uFogAlbedo;
uniform vec3 uAmbient;          // scene-referred sky light (cd/m^2)
uniform vec3 uSmokeTint;
uniform float uSmokeGain;       // scales lamp/headlight light on smoke
uniform float uBrightness;

float Hash21(vec2 p) {
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

float ValueNoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = Hash21(i);
	float b = Hash21(i + vec2(1.0, 0.0));
	float c = Hash21(i + vec2(0.0, 1.0));
	float d = Hash21(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Three-octave fBm, normalised to roughly [0,1].
float Fbm(vec2 p) {
	float s = 0.0;
	float a = 0.5;
	for (int i = 0; i < 3; ++i) {
		s += a * ValueNoise(p);
		p = p * 2.03 + vec2(17.1, 9.2);
		a *= 0.5;
	}
	return s / 0.875;
}

void main() {
	float density;
	vec3 tint = vec3(1.0);
	float gain = uSmokeGain;
	float ambient = 1.0;

	if (vKind == 1) {
		// Droplet: thin streak, bright along its centre line, feathered at the ends.
		float across = clamp(1.0 - vCorner.x * vCorner.x, 0.0, 1.0);
		float along = 1.0 - smoothstep(0.55, 1.0, abs(vCorner.y));
		density = across * across * along;
		gain = uSmokeGain * 1.4;
		ambient = 0.5;
	} else {
		float r2 = dot(vCorner, vCorner);
		float shape = clamp(1.0 - r2, 0.0, 1.0);
		vec2 seedOffset = vec2(vSeed * 31.7, vSeed * 13.3);
		if (vKind == 0) {
			// Puff: noise-eroded disc that drifts and thins as it ages.
			float n = Fbm(vCorner * 1.8 + seedOffset + vec2(vAge * 0.6, -vAge * 0.3));
			density = smoothstep(0.25, 0.8, shape * 1.2 + (n - 0.5) * 0.9);
			tint = uSmokeTint;
		} else {
			// Mist: broad, soft, very low-frequency haze.
			float n = Fbm(vCorner * 1.2 + seedOffset);
			density = shape * shape * (0.6 + 0.4 * n);
			gain = uSmokeGain * 1.2;
			ambient = 0.7;
		}
	}

	// Soft particle: fade towards the scene depth so intersections with the road and bodywork blend.
	float sceneD = texture(uSceneDepth, gl_FragCoord.xy / uScreenSize).r;
	float sceneView = sceneD >= 1.0 ? 1e6 : uProj[3][2] / ((sceneD * 2.0 - 1.0) + uProj[2][2]);
	float soft = clamp((sceneView - vViewDepth) / max(uSoftRange, 1e-3), 0.0, 1.0);
	soft = soft * soft * (3.0 - 2.0 * soft);

	float coverage = vOpacity * density * soft * 0.75;
	if (coverage < 0.002) {
		discard;
	}

	// Night street: smoke is lit by headlights, tail lights and lamps around the car; the fog
	// volume carries the lamps, the ambient term is lifted so smoke reads grey instead of black.
	vec3 light = uAmbient * ambient * 8.0;
	if (uHasVolume) {
		vec4 clip = uViewProj * vec4(vWorldPos, 1.0);
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		vec4 se = texture(uScattering, vec3(uv, VolumeDepthToSlice(vViewDepth)));
		// se.rgb is pre-exposed sigma_s * L_in; undo albedo, extinction and pre-exposure.
		vec3 incident = se.rgb / max(se.a * uFogAlbedo, vec3(1e-6)) / max(NoxPreExposure(), 1e-12);
		light += incident * gain;
	}
	vec3 radiance = light * tint * uBrightness;
	// Exposure-relative floor: tyre smoke at night still reads as grey, lit by the car's own lights
	// and the street, instead of disappearing as black haze over dark asphalt.
	vec3 exposed = max(radiance * NoxPreExposure(), tint * 0.07 * ambient);
	FragColor = vec4(exposed * coverage, coverage);
}
