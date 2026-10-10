#version 460 core
// Surface decals (DecalPass). A decal is a material layer, not an emitter: coverage blends the decal's
// base colour and roughness into the underlying surface's G-buffer before lighting, so it is lit,
// wetted and reflected exactly like the surface. Normal, metallic, F0, emissive and AO are left
// untouched (colour masks).
//
// Texture: linear premultiplied RGBA (TextureArrayBuilder), straight colour = rgb / a.

#include "includes/pbr_common.glsl"
#include "includes/weather_common.glsl"

in vec3 vWorldPos;
in vec2 vUV;
flat in vec3 vNormal;
flat in float vLayer;
flat in vec4 vTint;

layout(location = 0) out vec4 gPackedNormalRM;  // only B (roughness) is written
layout(location = 1) out vec4 gAlbedoAO;        // only RGB (albedo) is written

uniform sampler2DArray uDecals;
uniform float uAlbedoScale = 1.0;
uniform float uRoughness = 0.5;
uniform float uWetnessResponse = 0.0;
uniform float uPuddleResponse = 0.0;

void main() {
	vec4 s = texture(uDecals, vec3(vUV, vLayer));
	float coverage = s.a * vTint.a;
	if (coverage < 1.0 / 255.0) {
		discard;
	}
	vec3 albedo = s.rgb / max(s.a, 1e-4) * vTint.rgb * uAlbedoScale;
	float roughness = uRoughness;
	vec3 N = vNormal;
	float ior = 1.5;
	NoxApplyWetness(vWorldPos, vNormal, 0.0, uWetnessResponse, uPuddleResponse, 0.0, albedo, roughness, N, ior);

	gPackedNormalRM = vec4(0.0, 0.0, ClampPerceptualRoughness(roughness), coverage);
	gAlbedoAO = vec4(albedo, coverage);
}
