#version 460 core
// Volumetric fog step 1: participating media + lighting per froxel, temporally reprojected.
// Lights come from the clustered lists (LightClusterPass) and shadow maps, so fog shafts match the
// surface lighting. Output RGBA16F: rgb = in-scattered radiance per metre (pre-exposed), a = extinction.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define NOX_COMPUTE_STAGE
#include "includes/pbr_common.glsl"
#include "includes/lighting_common.glsl"
#include "includes/clustered_lighting.glsl"
#include "includes/exposure_common.glsl"
#include "includes/volumetric_common.glsl"

layout(rgba16f, binding = 0) uniform writeonly image3D uOutVolume;
uniform sampler3D uHistoryVolume;
uniform bool uHistoryValid;

uniform mat4 uInvViewProj;     // current, unjittered
uniform mat4 uPrevViewProj;    // previous, unjittered
uniform vec3 uCameraPos;
uniform vec2 uScreenSize;
uniform uint uFrame;

uniform float uFogDensity;       // extinction (1/m) at the base height
uniform float uFogBaseHeight;    // metres
uniform float uFogHeightFalloff; // metres per e-fold above the base height
uniform vec3  uFogAlbedo;
uniform float uFogAnisotropy;    // Henyey-Greenstein g
uniform float uFogNoiseStrength; // [0,1]
uniform float uFogNoiseScale;    // metres per noise cell
uniform vec3  uFogWind;          // m/s
uniform float uTime;
uniform vec3  uAmbientRadiance;  // isotropic sky light (scene units)
uniform float uTemporalBlend;    // weight of the current frame

float Hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float ValueNoise3(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	vec3 u = f * f * (3.0 - 2.0 * f);
	float n000 = Hash13(i), n100 = Hash13(i + vec3(1, 0, 0)), n010 = Hash13(i + vec3(0, 1, 0)), n110 = Hash13(i + vec3(1, 1, 0));
	float n001 = Hash13(i + vec3(0, 0, 1)), n101 = Hash13(i + vec3(1, 0, 1)), n011 = Hash13(i + vec3(0, 1, 1)), n111 = Hash13(i + vec3(1, 1, 1));
	return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

float HenyeyGreenstein(float cosTheta, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// Punctual light reaching a point in the medium (same falloff/cone/shadowing as surfaces).
vec3 LightAtPoint(int idx, vec3 worldPos, vec3 toCamera) {
	LightData Ld = lights[idx];
	int type = int(Ld.position.w);
	vec3 lightDir = normalize(Ld.direction.xyz);
	vec3 L;
	float attenuation = Ld.color.w;
	if (type == 0) {
		L = -lightDir;
	} else {
		vec3 diff = Ld.position.xyz - worldPos;
		float dist = length(diff);
		float range = Ld.attenuation.w;
		if (dist > range) return vec3(0.0);
		L = diff / max(dist, 1e-4);
		vec3 att = Ld.attenuation.xyz;
		float window = max(1.0 - pow(dist / range, 4.0), 0.0);
		attenuation *= window * window / (att.x + att.y * dist + att.z * dist * dist);
		if (type == 2) {
			float theta = dot(L, -lightDir);
			float innerCos = (Ld.spotData.x > 0.0) ? Ld.spotData.x : cos(radians(20.0));
			float outerCos = min((Ld.spotData.y > 0.0) ? Ld.spotData.y : cos(radians(30.0)), innerCos - 0.001);
			if (theta < outerCos) return vec3(0.0);
			float cone = clamp((theta - outerCos) / max(innerCos - outerCos, 0.001), 0.0, 1.0);
			attenuation *= cone * cone * (3.0 - 2.0 * cone);
		}
	}
	float shadow = 1.0;
	if (enableShadows == 1 && Ld.shadowData.z > 0.5) {
		shadow = ComputeShadowForLight(type, int(Ld.shadowData.x + 0.5), int(Ld.shadowData.y + 0.5), worldPos, L, lightDir, Ld.position.xyz);
	}
	float phase = HenyeyGreenstein(dot(-L, -toCamera), uFogAnisotropy);
	return Ld.color.rgb * attenuation * shadow * phase;
}

void main() {
	ivec3 cell = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(cell, uVolumeSize))) return;

	// Jitter the sample along the slice every frame; the temporal filter integrates the slice.
	float jitter = fract(Hash13(vec3(cell.xy, float(uFrame % 64u))) + float(uFrame % 16u) * 0.618034);
	vec2 uv = (vec2(cell.xy) + 0.5) / vec2(uVolumeSize.xy);
	float w = (float(cell.z) + jitter) / float(uVolumeSize.z);
	float depth = VolumeSliceToDepth(w);

	// Reconstruct the world position on the view ray at view-space depth `depth`.
	vec4 farPoint = uInvViewProj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
	vec3 rayDir = normalize(farPoint.xyz / farPoint.w - uCameraPos);
	vec3 viewForward = normalize(-vec3(view[0][2], view[1][2], view[2][2]));
	float t = depth / max(dot(rayDir, viewForward), 1e-3);
	vec3 worldPos = uCameraPos + rayDir * t;
	vec3 toCamera = -rayDir;

	// Media: exponential height fog with drifting low-frequency noise (mist breakup).
	float heightTerm = exp(-max(worldPos.y - uFogBaseHeight, 0.0) / max(uFogHeightFalloff, 0.1));
	float noise = ValueNoise3((worldPos - uFogWind * uTime) / max(uFogNoiseScale, 0.1));
	noise = mix(1.0, noise * 1.6, uFogNoiseStrength);
	float extinction = max(uFogDensity * heightTerm * noise, 0.0);
	vec3 scattering = extinction * uFogAlbedo;

	// Uniform sky light: the phase function integrates to 1, so in-scattered radiance equals it.
	vec3 inScatter = uAmbientRadiance;
	if (numLights > 0 && extinction > 0.0) {
		int directionalCount = int(clusterLights.y);
		for (int i = 0; i < directionalCount; ++i) {
			inScatter += LightAtPoint(i, worldPos, toCamera);
		}
		if (NoxClusteringEnabled()) {
			uint cluster = NoxClusterIndex(uv * uScreenSize, depth);
			uint count = min(clusterLightCount[cluster], NOX_CLUSTER_MAX_LIGHTS);
			uint base = cluster * NOX_CLUSTER_MAX_LIGHTS;
			for (uint k = 0u; k < count; ++k) {
				inScatter += LightAtPoint(int(clusterLightIndex[base + k]), worldPos, toCamera);
			}
		} else {
			for (int i = directionalCount; i < numLights; ++i) {
				inScatter += LightAtPoint(i, worldPos, toCamera);
			}
		}
	}
	vec4 current = vec4(scattering * inScatter * NoxPreExposure(), extinction);

	// Temporal reprojection of the froxel centre (Hillaire: ~5-10% new data per frame).
	if (uHistoryValid) {
		float centreDepth = VolumeSliceToDepth((float(cell.z) + 0.5) / float(uVolumeSize.z));
		vec3 centrePos = uCameraPos + rayDir * (centreDepth / max(dot(rayDir, viewForward), 1e-3));
		vec4 prevClip = uPrevViewProj * vec4(centrePos, 1.0);
		if (prevClip.w > 0.0) {
			vec3 prevNdc = prevClip.xyz / prevClip.w;
			vec2 prevUV = prevNdc.xy * 0.5 + 0.5;
			// Previous view depth: distance along the previous camera's forward axis.
			float prevDepth = prevClip.w;
			float prevW = VolumeDepthToSlice(prevDepth);
			if (all(greaterThanEqual(prevUV, vec2(0.0))) && all(lessThanEqual(prevUV, vec2(1.0))) && prevW <= 1.0) {
				vec4 history = texture(uHistoryVolume, vec3(prevUV, prevW));
				history.rgb *= NoxHistoryExposureScale();
				current = mix(history, current, uTemporalBlend);
			}
		}
	}
	imageStore(uOutVolume, cell, current);
}
