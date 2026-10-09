#version 460 core
// Rain streak shading: a drop is lit by whatever light reaches it. The froxel fog volume already
// stores in-scattered radiance (sigma_s * L_in) and extinction per froxel, so L_in ~ rgb / extinction
// (albedo-normalised) gives the local light field including lamp cones and headlights.

#include "includes/volumetric_common.glsl"
#include "includes/exposure_common.glsl"

in vec2 vCorner;
in vec3 vWorldPos;
in float vFade;
layout(location = 0) out vec4 FragColor;

uniform sampler3D uScattering;  // current froxel scattering/extinction volume
uniform bool uHasVolume;
uniform mat4 uView;
uniform mat4 uViewProj;
uniform vec3 uFogAlbedo;
uniform vec3 uAmbient;          // scene-referred fallback light
uniform float uOpacity;
uniform float uBrightness;      // drop reflectance/refraction gain

void main() {
	float across = 1.0 - abs(vCorner.x);
	float alongFade = smoothstep(0.0, 0.25, vCorner.y) * smoothstep(1.0, 0.75, vCorner.y);
	float coverage = across * across * alongFade * vFade * uOpacity;
	if (coverage <= 0.002) discard;

	vec3 light = uAmbient;
	if (uHasVolume) {
		vec4 clip = uViewProj * vec4(vWorldPos, 1.0);
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		float depth = -(uView * vec4(vWorldPos, 1.0)).z;
		vec4 se = texture(uScattering, vec3(uv, VolumeDepthToSlice(depth)));
		// se.rgb is pre-exposed sigma_s * L_in; undo albedo, extinction and pre-exposure.
		vec3 incident = se.rgb / max(se.a * uFogAlbedo, vec3(1e-6)) / max(NoxPreExposure(), 1e-12);
		light += incident;
	}
	vec3 radiance = light * uBrightness;
	// Premultiplied output; drops occlude a little and add their lit glint.
	FragColor = vec4(radiance * coverage * NoxPreExposure(), coverage * 0.35);
}
