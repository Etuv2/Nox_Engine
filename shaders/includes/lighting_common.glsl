#ifndef NOX_LIGHTING_COMMON_GLSL
#define NOX_LIGHTING_COMMON_GLSL

// Direct lighting shared by the deferred lighting pass and the forward transparent pass:
// the LightManager light/shadow-matrix buffers, cascaded / spot / point shadow lookups and the
// per-light radiance evaluation. Include after pbr_common.glsl.

uniform mat4 view; // camera view matrix; cascade selection uses view-space depth

// Shadows
uniform sampler2DArrayShadow multiLightShadowArray;
uniform int   enableShadows = 1;
// Bias strength: NOX_SHADOW_REFERENCE_BIAS (0.001) is 1x. Offsets are sized per shadow layer from
// its world-space texel footprint (see includes/shadow_bias_common.glsl).
uniform float shadowBias = 0.001;

// Cascade blend settings (set from LightManager)
uniform float cascadeBlendDistance = 5.0;  // World-space blend distance
uniform float cascadeBlendFactor = 0.15;   // Fraction of cascade range for blend

// Shadow darkness settings - control how dark shadows appear
uniform float shadowDarkness = 1.0;           // Multiplier for shadow darkness [0.0=no shadows, 1.0=full darkness]
uniform float shadowMinBrightness = 0.0;      // Minimum brightness in complete shadow (0.0 = physically dark direct shadows)
uniform float shadowTransitionHardness = 1.0; // Softness of shadow boundaries [0.5=very soft, 2.0=sharp]

// Cascade split depths for view-depth blending (set from LightManager)
uniform vec4 cascadeSplits = vec4(10.0, 30.0, 100.0, 500.0);  // Far distances per cascade

// Lights
uniform int numLights = 0; // total active lights (any type)

struct LightData {
	vec4 position;    // xyz=pos or dir origin, w=type (0 dir,1 point,2 spot)
	vec4 direction;   // xyz=direction (for spot/dir)
	vec4 color;       // rgb=color, w=intensity
	vec4 attenuation; // xyz=const,linear,quadratic, w=range
	vec4 shadowData;  // x=startSlice, y=sliceCount, z=enabled, w=pcss flag
	vec4 spotData;    // x=inner cos, y=outer cos
	vec4 areaData;    // xyz=area light size (unused for point/spot/dir), w=reserved
	vec4 sampling;    // x=PDF weight, y=solid angle, z,w=reserved
};
layout(std430, binding = 0) buffer LightDataBuffer { LightData lights[]; };
layout(std430, binding = 1) buffer ShadowMatricesBuffer { mat4 shadowMatrices[]; };

#include "shadow_common.glsl"

// Apply physically-plausible shadow visibility shaping.
// Direct shadowing should be independent of material albedo/metalness/roughness.
float ApplyRealisticShadow(float shadowVisibility) {
	float visibility = clamp(shadowVisibility, 0.0, 1.0);
	float transitionHardness = max(shadowTransitionHardness, 0.25);
	float shapedVisibility = pow(visibility, transitionHardness);

	// Keep only an explicit user floor instead of an AO-driven artificial lift.
	float ambientFloor = clamp(shadowMinBrightness, 0.0, 1.0);
	float result = mix(ambientFloor, 1.0, shapedVisibility);
	result = mix(1.0, result, clamp(shadowDarkness, 0.0, 1.0));
	return result;
}
// Cascaded shadow mapping with cascade selection and smooth blending.
// lightDir is the direction the light travels (towards the scene).
float ComputeCascadedShadow(
	int startSlice,
	int sliceCount,
	vec3 worldPos,
	vec3 N,
	vec3 lightDir,
	out int cascadeIndex,
	out vec3 projCoords,
	out float cascadeCoverage,
	out float viewDepth
) {
	vec3 viewSpacePos = (view * vec4(worldPos, 1.0)).xyz;
	viewDepth = -viewSpacePos.z;
	cascadeIndex = -1;
	projCoords = vec3(0.0);
	cascadeCoverage = 1.0;

	sliceCount = min(sliceCount, 4);
	if (sliceCount <= 0) {
		return 1.0;
	}

	float maxCascadeDepth = cascadeSplits[sliceCount - 1];
	float fadeEnd = maxCascadeDepth * 1.1;
	if (viewDepth >= fadeEnd) {
		return 1.0;
	}

	int primaryCascade = sliceCount - 1;
	for (int i = 0; i < sliceCount; ++i) {
		if (viewDepth <= cascadeSplits[i]) {
			primaryCascade = i;
			break;
		}
	}

	vec3 L = -lightDir;
	int layer0 = startSlice + primaryCascade;
	vec3 pc0;
	if (!NoxProjectToShadowMap(shadowMatrices[layer0], worldPos, pc0) || !InShadowBounds(pc0)) {
		return 1.0;
	}

	cascadeIndex = primaryCascade;
	projCoords = pc0;
	float depthSoftness = clamp(viewDepth / max(maxCascadeDepth, 0.001), 0.0, 1.0);
	float cascadeSoftness = float(primaryCascade) / float(max(sliceCount - 1, 1));
	float primaryFilterScale = mix(0.65, 1.2, max(depthSoftness, cascadeSoftness));
	bool primaryValid;
	float shadowValue = SampleShadowLayerBiased(layer0, worldPos, N, L, 0.05, primaryFilterScale, primaryValid);

	float cascadeNear = (primaryCascade == 0) ? 0.0 : cascadeSplits[primaryCascade - 1];
	float cascadeFar = cascadeSplits[primaryCascade];
	float cascadeRange = max(cascadeFar - cascadeNear, 0.001);

	if (primaryCascade < sliceCount - 1) {
		float blendZoneSize = cascadeRange * cascadeBlendFactor;
		blendZoneSize = min(blendZoneSize, cascadeBlendDistance);
		blendZoneSize = max(blendZoneSize, cascadeRange * 0.05);
		float blendStart = cascadeFar - blendZoneSize;

		if (viewDepth > blendStart) {
			int nextCascade = primaryCascade + 1;
			int layer1 = startSlice + nextCascade;
			float nextCascadeSoftness = float(nextCascade) / float(max(sliceCount - 1, 1));
			float nextFilterScale = mix(0.65, 1.2, max(depthSoftness, nextCascadeSoftness));
			bool nextValid;
			float shadow1 = SampleShadowLayerBiased(layer1, worldPos, N, L, 0.05, nextFilterScale, nextValid);
			if (nextValid) {
				float blendFactor = smoothstep(blendStart, cascadeFar, viewDepth);
				shadowValue = mix(shadowValue, shadow1, blendFactor);
				cascadeCoverage = 1.0 - blendFactor;
			}
		}
	}

	if (viewDepth > maxCascadeDepth) {
		float fadeFactor = clamp((viewDepth - maxCascadeDepth) / (fadeEnd - maxCascadeDepth), 0.0, 1.0);
		shadowValue = mix(shadowValue, 1.0, fadeFactor);
	}

	return shadowValue;
}

float AxisComponent(vec3 v, int axis) {
	if (axis == 0) return v.x;
	if (axis == 1) return v.y;
	return v.z;
}

int PointFaceFromAxisSign(int axis, float signedValue) {
	if (axis == 0) return (signedValue >= 0.0) ? 0 : 1;
	if (axis == 1) return (signedValue >= 0.0) ? 2 : 3;
	return (signedValue >= 0.0) ? 4 : 5;
}

float ComputePointLightShadow(int startSlice, vec3 worldPos, vec3 N, vec3 lightPos) {
	vec3 toSurface = worldPos - lightPos;
	float distanceToLight = length(toSurface);
	if (distanceToLight <= 1e-5) {
		return 1.0;
	}

	vec3 L = -toSurface / distanceToLight;
	vec3 absDir = abs(toSurface);

	int primaryAxis;
	int secondaryAxis;
	if (absDir.x >= absDir.y && absDir.x >= absDir.z) {
		primaryAxis = 0;
		secondaryAxis = (absDir.y >= absDir.z) ? 1 : 2;
	} else if (absDir.y >= absDir.x && absDir.y >= absDir.z) {
		primaryAxis = 1;
		secondaryAxis = (absDir.x >= absDir.z) ? 0 : 2;
	} else {
		primaryAxis = 2;
		secondaryAxis = (absDir.x >= absDir.y) ? 0 : 1;
	}
	int tertiaryAxis = 3 - primaryAxis - secondaryAxis;

	// Face order matches LightManager: +X, -X, +Y, -Y, +Z, -Z.
	int primaryFace = PointFaceFromAxisSign(primaryAxis, AxisComponent(toSurface, primaryAxis));
	int secondaryFace = PointFaceFromAxisSign(secondaryAxis, AxisComponent(toSurface, secondaryAxis));
	int tertiaryFace = PointFaceFromAxisSign(tertiaryAxis, AxisComponent(toSurface, tertiaryAxis));

	bool primaryValid = false;
	float primaryShadow = SampleShadowLayerBiased(startSlice + primaryFace, worldPos, N, L, 0.02, 1.1, primaryValid);
	if (!primaryValid) {
		return 1.0;
	}

	// Faces are rendered slightly wider than 90 degrees; blend towards the neighbouring faces near
	// cube edges so filtering does not show the seam.
	float axisSum = max(absDir.x + absDir.y + absDir.z, 1e-5);
	float primaryDominance = AxisComponent(absDir, primaryAxis) / axisSum;
	float seamBlend = 1.0 - smoothstep(0.72, 0.90, primaryDominance);
	if (seamBlend <= 1e-3) {
		return primaryShadow;
	}

	bool secondaryValid = false;
	bool tertiaryValid = false;
	float secondaryShadow = SampleShadowLayerBiased(startSlice + secondaryFace, worldPos, N, L, 0.02, 1.1, secondaryValid);
	float tertiaryShadow = SampleShadowLayerBiased(startSlice + tertiaryFace, worldPos, N, L, 0.02, 1.1, tertiaryValid);

	float wPrimary = 1.0;
	float wSecondary = secondaryValid ? seamBlend * (AxisComponent(absDir, secondaryAxis) / axisSum) : 0.0;
	float wTertiary = tertiaryValid ? seamBlend * (AxisComponent(absDir, tertiaryAxis) / axisSum) : 0.0;

	float weightSum = wPrimary + wSecondary + wTertiary;
	return (primaryShadow * wPrimary + secondaryShadow * wSecondary + tertiaryShadow * wTertiary) / weightSum;
}

// Compute shadowing for a given light source.
// lightDir: travel direction for directional lights; ignored otherwise.
float ComputeShadowForLight(int lightType, int startSlice, int sliceCount, vec3 worldPos, vec3 N, vec3 lightDir, vec3 lightPos) {
	if (sliceCount <= 0 || startSlice < 0) return 1.0;

	if (lightType == 0) {
		int cascadeIndex;
		vec3 projCoords;
		float cascadeCoverage;
		float viewDepth;
		return ComputeCascadedShadow(startSlice, sliceCount, worldPos, N, lightDir, cascadeIndex, projCoords, cascadeCoverage, viewDepth);
	}
	if (lightType == 1) {
		return ComputePointLightShadow(startSlice, worldPos, N, lightPos);
	}

	vec3 toLight = lightPos - worldPos;
	float distance = length(toLight);
	if (distance <= 1e-5) {
		return 1.0;
	}
	float spotFilterScale = mix(0.9, 1.3, clamp(distance / 40.0, 0.0, 1.0));
	bool valid;
	return SampleShadowLayerBiased(startSlice, worldPos, N, toLight / distance, 0.02, spotFilterScale, valid);
}

// Direct light `idx` at a surface point. Outputs the unshadowed diffuse and specular radiance
// (BRDF * cos * light color * intensity * distance/cone attenuation) and the shaped shadow-map
// visibility, which callers multiply in (the deferred pass first refines it with contact
// shadows). Returns false when the light does not reach the point at all.
bool EvaluateLightRadiance(
	int idx,
	vec3 worldPos,
	vec3 N,
	vec3 V,
	PrincipledSurface surface,
	out vec3 diffuseOut,
	out vec3 specularOut,
	out float shadowVisibility
) {
	diffuseOut = vec3(0.0);
	specularOut = vec3(0.0);
	shadowVisibility = 1.0;

	LightData Ld = lights[idx];
	int type = int(Ld.position.w);
	vec3 lightPos = Ld.position.xyz;
	vec3 lightDir = normalize(Ld.direction.xyz);
	vec3 L;
	float attenuation = 1.0;
	vec3 lightColor = Ld.color.rgb;
	float intensity = Ld.color.w;
	float range = Ld.attenuation.w;

	if (type == 0) {
		// Directional light
		L = -lightDir;
		attenuation = intensity;
	} else {
		// Point or spot light
		vec3 diff = lightPos - worldPos;
		float dist = length(diff);
		if (dist > range) return false;
		L = diff / dist;
		
		vec3 att = Ld.attenuation.xyz;
		float inv = 1.0 / (att.x + att.y * dist + att.z * dist * dist);
		float rf = 1.0 - pow(dist / range, 4.0);
		rf = max(rf, 0.0);
		rf *= rf;
		attenuation = inv * rf * intensity;
		
		if (type == 2) {
			// Spot light
			float theta = dot(L, -lightDir);
			float innerCos = (Ld.spotData.x > 0.0) ? Ld.spotData.x : cos(radians(20.0));
			float outerCos = (Ld.spotData.y > 0.0) ? Ld.spotData.y : cos(radians(30.0));
			outerCos = min(outerCos, innerCos - 0.001);
			if (theta < outerCos) return false;
			float eps = max(innerCos - outerCos, 0.001);
			float cone = clamp((theta - outerCos) / eps, 0.0, 1.0);
			cone = cone * cone * (3.0 - 2.0 * cone);
			attenuation *= cone;
		}
	}

	float NdotL = max(dot(N, L), 0.0);
	if (NdotL <= 0.0) return false;

	vec3 diffuse, specular;
	EvaluatePrincipledBRDFSeparated(surface, N, V, L, diffuse, specular);

	if (enableShadows == 1 && Ld.shadowData.z > 0.5) {
		int startSlice = int(Ld.shadowData.x + 0.5);
		int sliceCount = int(Ld.shadowData.y + 0.5);
		shadowVisibility = ApplyRealisticShadow(
			ComputeShadowForLight(type, startSlice, sliceCount, worldPos, N, lightDir, lightPos));
	}

	vec3 radiance = lightColor * attenuation;
	diffuseOut = diffuse * radiance;
	specularOut = specular * radiance;
	return true;
}

#endif // NOX_LIGHTING_COMMON_GLSL
