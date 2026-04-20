#version 460 core

// Include shared PBR functions
#include "includes/pbr_common.glsl"
#include "includes/material_common.glsl"

in vec2 vTexCoord;
out vec4 FragColor;

// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
// RT3: R32UI - Material ID (opaque PBR or transmission)
// RT4: RGBA16F - Emissive color (RGB) + unused (A)
// RT7: RGBA16F - Principled extras: transmission (R), IOR (G), reserved (BA)
uniform sampler2D gPackedNormalRM;  // RT0: oct normal (RG) + roughness (B) + metallic (A)
uniform sampler2D gAlbedoAO;        // RT1: albedo (RGB) + occlusion (A)
uniform sampler2D gSpecularF0;      // RT2: specular F0 (RGB) + emissive strength (A)
uniform usampler2D gMaterialID;     // RT3: material ID (uint)
uniform sampler2D gEmissive;        // RT4: emissive color (RGB)
uniform sampler2D gClearCoat;       // RT6: clearcoat factor + roughness
uniform sampler2D gPrincipledParams;// RT7: principled extras
uniform sampler2D gDepth;           // depth buffer (non-linear 0..1)

// Camera - these MUST match the exact matrices used when writing G-buffer
uniform mat4 invProjection;    // Exact inverse of projection used in G-buffer pass
uniform mat4 invView;          // Exact inverse of view used in G-buffer pass
uniform mat4 view;             // View matrix for distance calculations
uniform vec3 viewPos;

// Normal space configuration
uniform int normalsInWorldSpace = 1; // 1 if normals in G-buffer are world space, 0 if view space

// IBL
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform float prefilteredMaxLOD;

// IBL intensity controls to prevent over-bright results
uniform float iblIntensity = 0.35;       // Overall IBL multiplier
uniform float diffuseIBLScale = 0.3;    // Diffuse irradiance scale
uniform float specularIBLScale = 0.45;   // Specular prefiltered scale

// LPV Global Illumination
uniform sampler3D lpvTextureR;
uniform sampler3D lpvTextureG;
uniform sampler3D lpvTextureB;
uniform vec3 lpvGridCenter;
uniform float lpvVoxelSize;
uniform int lpvGridResolution;
uniform float lpvGIStrength = 1.0;
uniform int enableLPV = 0;
uniform int lpvDebugVisualization = 0;
uniform float lpvDebugBoost = 1.0;
uniform vec4 lpvGridOrientation = vec4(0.0, 0.0, 0.0, 1.0); // Quaternion (x, y, z, w)

// SSAO
uniform sampler2D ssaoMap;
uniform float aoStrength = 0.9;

// Screen-Space Shadows (Contact Shadows)
uniform sampler2D screenSpaceShadowMap;
uniform float sssStrength = 0.6; // Contact shadow blend strength [0,1]

uniform sampler2D indirectDiffuseMap;
uniform float indirectDiffuseStrength = 1.0;
uniform int indirectDiffuseCompositeMode = 0; // 0 additive, 1 modulative
uniform int lightingOutputMode = 0; // 0 full lighting, 1 bounceable radiance

// Shadows
uniform sampler2DArrayShadow multiLightShadowArray;
uniform int   enableShadows = 1;
uniform float shadowBias = 0.001;
uniform float maxShadowBias = 0.01;
uniform float normalOffsetScale = 0.01;  // Reduced from 0.1 to minimize floating shadows
uniform float cascadeBiasScale = 1.0;

// Cascade blend settings (set from LightManager)
uniform float cascadeBlendDistance = 5.0;  // World-space blend distance
uniform float cascadeBlendFactor = 0.15;   // Fraction of cascade range for blend

// Point light shadow settings
uniform float pointLightBias = 0.002;
uniform float pointLightSlopeBias = 0.005;
uniform float pointLightNormalOffset = 0.01;

// Shadow darkness settings - control how dark shadows appear
uniform float shadowDarkness = 1.0;           // Multiplier for shadow darkness [0.0=no shadows, 1.0=full darkness]
uniform float shadowMinBrightness = 0.0;      // Minimum brightness in complete shadow (0.0 = physically dark direct shadows)
uniform float shadowTransitionHardness = 1.0; // Softness of shadow boundaries [0.5=very soft, 2.0=sharp]

// Cascade split depths for view-depth blending (set from LightManager)
uniform vec4 cascadeSplits = vec4(10.0, 30.0, 100.0, 500.0);  // Far distances per cascade

// Debug visualization
uniform int shadowDebugVisualization = 0; // 0=off, 1=cascade index, 2=raw depth, 3=bias, 4=texel density, 5=shadow mask

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

#include "includes/shadow_common.glsl"

// Note: PI, INV_PI, DIELECTRIC_F0 are now defined in pbr_common.glsl
// Note: DecodeNormalOct, DistributionGGX, GeometrySchlickGGX, GeometrySmith, 
//       FresnelSchlick, CalculateDiffuseAlbedo, SpecularOcclusion are in pbr_common.glsl

vec3 worldPosFromDepth(vec2 uv, float depth) {
	vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	vec4 viewPos4 = invProjection * clip;
	viewPos4 /= viewPos4.w;
	vec4 world = invView * viewPos4;
	return world.xyz;
}

vec3 getNormalInWorldSpace(vec3 decodedNormal) {
	if (normalsInWorldSpace == 1) {
		return decodedNormal;
	} else {
		return normalize(mat3(invView) * decodedNormal);
	}
}

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
// Cascaded shadow mapping with cascade selection and smooth blending
float ComputeCascadedShadow(
	int startSlice,
	int sliceCount,
	vec3 worldPos,
	vec3 N,
	vec3 lightDir,
	out int cascadeIndex,
	out vec3 projCoords,
	out float cascadeBias,
	out float cascadeCoverage,
	out float viewDepth
) {
	vec3 viewSpacePos = (view * vec4(worldPos, 1.0)).xyz;
	viewDepth = -viewSpacePos.z;
	cascadeIndex = -1;
	projCoords = vec3(0.0);
	cascadeBias = 0.0;
	cascadeCoverage = 1.0;

	sliceCount = min(sliceCount, 4);
	if (sliceCount <= 0) {
		return 1.0;
	}

	float maxCascadeDepth = cascadeSplits[sliceCount - 1];
	if (viewDepth > maxCascadeDepth) {
		float fadeStart = maxCascadeDepth;
		float fadeEnd = maxCascadeDepth * 1.1;
		float fadeFactor = clamp((viewDepth - fadeStart) / (fadeEnd - fadeStart), 0.0, 1.0);
		if (fadeFactor >= 1.0) {
			return 1.0;
		}
	}

	int primaryCascade = sliceCount - 1;
	for (int i = 0; i < sliceCount; ++i) {
		if (viewDepth <= cascadeSplits[i]) {
			primaryCascade = i;
			break;
		}
	}

	int layer0 = startSlice + primaryCascade;
	vec4 lsp0 = shadowMatrices[layer0] * vec4(worldPos, 1.0);
	vec3 pc0 = lsp0.xyz / lsp0.w;
	pc0 = pc0 * 0.5 + 0.5;
	if (!InShadowBounds(pc0)) {
		return 1.0;
	}

	cascadeIndex = primaryCascade;
	projCoords = pc0;
	cascadeBias = CalculateAdaptiveShadowBias(N, lightDir, primaryCascade, pc0.z, viewDepth);
	float depthSoftness = clamp(viewDepth / max(maxCascadeDepth, 0.001), 0.0, 1.0);
	float cascadeSoftness = float(primaryCascade) / float(max(sliceCount - 1, 1));
	float primaryFilterScale = mix(0.9, 1.8, max(depthSoftness, cascadeSoftness));
	float shadowValue = SampleShadowArrayEdgeSafe(layer0, pc0, cascadeBias, 0.05, primaryFilterScale);

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
			vec4 lsp1 = shadowMatrices[layer1] * vec4(worldPos, 1.0);
			vec3 pc1 = lsp1.xyz / lsp1.w;
			pc1 = pc1 * 0.5 + 0.5;

			if (InShadowBounds(pc1)) {
				float bias1 = CalculateAdaptiveShadowBias(N, lightDir, nextCascade, pc1.z, viewDepth);
				float nextCascadeSoftness = float(nextCascade) / float(max(sliceCount - 1, 1));
				float nextFilterScale = mix(0.9, 1.8, max(depthSoftness, nextCascadeSoftness));
				float shadow1 = SampleShadowArrayEdgeSafe(layer1, pc1, bias1, 0.05, nextFilterScale);
				float blendFactor = smoothstep(blendStart, cascadeFar, viewDepth);
				shadowValue = mix(shadowValue, shadow1, blendFactor);
				cascadeCoverage = 1.0 - blendFactor;
			}
		}
	}

	if (viewDepth > maxCascadeDepth) {
		float fadeStart = maxCascadeDepth;
		float fadeEnd = maxCascadeDepth * 1.1;
		float fadeFactor = clamp((viewDepth - fadeStart) / (fadeEnd - fadeStart), 0.0, 1.0);
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

bool ProjectPointShadowToLayer(int layer, vec3 shadowPos, out vec3 projCoords) {
	vec4 lsp = shadowMatrices[layer] * vec4(shadowPos, 1.0);
	if (abs(lsp.w) < 1e-6) {
		projCoords = vec3(0.0);
		return false;
	}

	vec3 ndc = lsp.xyz / lsp.w;
	projCoords = ndc * 0.5 + 0.5;
	if (projCoords.z > 1.0) {
		return false;
	}

	return projCoords.x >= 0.0 && projCoords.x <= 1.0 &&
		projCoords.y >= 0.0 && projCoords.y <= 1.0;
}

float SamplePointShadowFace(int startSlice, int face, vec3 shadowPos, float bias, out bool valid) {
	int layer = startSlice + face;
	vec3 projCoords;
	valid = ProjectPointShadowToLayer(layer, shadowPos, projCoords);
	if (!valid) {
		return 1.0;
	}

	vec3 sampleCoords = vec3(projCoords.xy, clamp(projCoords.z, 0.0, 1.0));
	return SampleShadowArrayEdgeSafe(layer, sampleCoords, bias, 0.02, 1.1);
}

float ComputePointLightShadow(int startSlice, vec3 worldPos, vec3 N, vec3 lightPos) {
	vec3 toSurface = worldPos - lightPos;
	float distanceToLight = length(toSurface);
	if (distanceToLight <= 1e-5) {
		return 1.0;
	}

	vec3 lightToSurfaceDir = toSurface / distanceToLight;
	vec3 surfaceToLightDir = -lightToSurfaceDir;

	// Keep normal offset extremely conservative to avoid visible shadow detachment.
	float receiverFacing = clamp(dot(N, surfaceToLightDir), 0.0, 1.0);
	float normalOffset = pointLightNormalOffset * (1.0 - receiverFacing);
	vec3 shadowPos = worldPos + N * normalOffset;

	vec3 offsetToLight = shadowPos - lightPos;
	vec3 absDir = abs(offsetToLight);

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

	int primaryFace = PointFaceFromAxisSign(primaryAxis, AxisComponent(offsetToLight, primaryAxis));
	int secondaryFace = PointFaceFromAxisSign(secondaryAxis, AxisComponent(offsetToLight, secondaryAxis));
	int tertiaryFace = PointFaceFromAxisSign(tertiaryAxis, AxisComponent(offsetToLight, tertiaryAxis));

	float NdotL = max(dot(N, surfaceToLightDir), 0.0);
	float slopeScale = sqrt(max(1.0 - NdotL * NdotL, 0.0)) / max(NdotL, 0.06);
	slopeScale = clamp(slopeScale, 0.0, 2.0);
	float bias = pointLightBias + pointLightSlopeBias * slopeScale;
	bias = clamp(bias, pointLightBias * 0.5, pointLightBias * 3.0);

	bool primaryValid = false;
	float primaryShadow = SamplePointShadowFace(startSlice, primaryFace, shadowPos, bias, primaryValid);
	if (!primaryValid) {
		return 1.0;
	}

	float axisSum = max(absDir.x + absDir.y + absDir.z, 1e-5);
	float primaryDominance = AxisComponent(absDir, primaryAxis) / axisSum;
	float seamBlend = 1.0 - smoothstep(0.72, 0.90, primaryDominance);
	if (seamBlend <= 1e-3) {
		return primaryShadow;
	}

	bool secondaryValid = false;
	bool tertiaryValid = false;
	float secondaryShadow = SamplePointShadowFace(startSlice, secondaryFace, shadowPos, bias, secondaryValid);
	float tertiaryShadow = SamplePointShadowFace(startSlice, tertiaryFace, shadowPos, bias, tertiaryValid);

	float wPrimary = 1.0;
	float wSecondary = seamBlend * (AxisComponent(absDir, secondaryAxis) / axisSum);
	float wTertiary = seamBlend * (AxisComponent(absDir, tertiaryAxis) / axisSum);

	if (!secondaryValid) {
		wSecondary = 0.0;
	}
	if (!tertiaryValid) {
		wTertiary = 0.0;
	}

	float weightSum = wPrimary + wSecondary + wTertiary;
	if (weightSum <= 1e-5) {
		return primaryShadow;
	}

	return (primaryShadow * wPrimary + secondaryShadow * wSecondary + tertiaryShadow * wTertiary) / weightSum;
}
// Compute shadowing for a given light source
float ComputeShadowForLight(int lightType, int startSlice, int sliceCount, vec3 worldPos, vec3 N, vec3 lightDir, vec3 lightPos) {
	if (sliceCount <= 0 || startSlice < 0) return 1.0;

	if (lightType == 0 && sliceCount > 1) {
		int cascadeIndex;
		vec3 projCoords;
		float cascadeBias;
		float cascadeCoverage;
		float viewDepth;
		return ComputeCascadedShadow(startSlice, sliceCount, worldPos, N, lightDir, cascadeIndex, projCoords, cascadeBias, cascadeCoverage, viewDepth);
	}
	else if (lightType == 1) {
		return ComputePointLightShadow(startSlice, worldPos, N, lightPos);
	}
	else {
		vec3 toLight = lightPos - worldPos;
		float distance = length(toLight);
		vec3 spotDir = toLight / distance;
		
		vec3 shadowPos = worldPos;
		
		int layer = startSlice;
		mat4 M = shadowMatrices[layer];
		vec4 lsp = M * vec4(shadowPos, 1.0);
		lsp.xyz /= lsp.w;
		vec3 pc = lsp.xyz * 0.5 + 0.5;
		if (!InShadowBounds(pc)) return 1.0;
		
		float bias = CalculateAdaptiveShadowBias(N, -spotDir, 0, pc.z, distance);
		float spotFilterScale = mix(0.9, 1.3, clamp(distance / 40.0, 0.0, 1.0));
		return SampleShadowArrayEdgeSafe(layer, pc, bias, 0.02, spotFilterScale);
	}
}

vec3 EvaluateDirectionalShadowDebug(LightData Ld, vec3 worldPos, vec3 N) {
	int startSlice = int(Ld.shadowData.x + 0.5);
	int sliceCount = int(Ld.shadowData.y + 0.5);
	vec3 lightDir = normalize(Ld.direction.xyz);

	int cascadeIndex;
	vec3 projCoords;
	float cascadeBias;
	float cascadeCoverage;
	float viewDepth;
	float shadow = ComputeCascadedShadow(
		startSlice,
		sliceCount,
		worldPos,
		N,
		lightDir,
		cascadeIndex,
		projCoords,
		cascadeBias,
		cascadeCoverage,
		viewDepth
	);

	if (cascadeIndex < 0) {
		return vec3(0.0);
	}

	if (shadowDebugVisualization == 1) {
		return GetCascadeDebugColor(cascadeIndex);
	}

	if (shadowDebugVisualization == 2) {
		float maxCascadeDepth = cascadeSplits[max(sliceCount - 1, 0)];
		float normalizedDepth = clamp(viewDepth / max(maxCascadeDepth, 0.001), 0.0, 1.0);
		return vec3(normalizedDepth);
	}

	if (shadowDebugVisualization == 3) {
		float normalizedBias = clamp(cascadeBias / max(maxShadowBias, 0.0001), 0.0, 1.0);
		return vec3(normalizedBias, 1.0 - normalizedBias, 0.15);
	}

	if (shadowDebugVisualization == 4) {
		float texelDensity = EstimateCascadeTexelDensity(startSlice + cascadeIndex, worldPos);
		float coverage = clamp(cascadeCoverage, 0.0, 1.0);
		vec3 lowDensityColor = vec3(0.12, 0.28, 1.0);
		vec3 highDensityColor = vec3(1.0, 0.92, 0.2);
		vec3 densityColor = mix(lowDensityColor, highDensityColor, texelDensity);
		return densityColor * mix(0.55, 1.0, coverage);
	}

	if (shadowDebugVisualization == 5) {
		return vec3(shadow);
	}

	return vec3(shadow);
}

bool EvaluateShadowDebugView(vec3 worldPos, vec3 N, out vec3 debugColor) {
	if (shadowDebugVisualization <= 0) {
		return false;
	}

	int maxLights = min(numLights, 64);
	for (int i = 0; i < maxLights; ++i) {
		LightData Ld = lights[i];
		if (int(Ld.position.w) != 0) {
			continue;
		}
		if (Ld.shadowData.z <= 0.5 || int(Ld.shadowData.y + 0.5) <= 0) {
			continue;
		}

		debugColor = EvaluateDirectionalShadowDebug(Ld, worldPos, N);
		return true;
	}

	return false;
}

// Shared direct-light evaluation so full lighting and bounceable radiance use the same
// attenuation and shadowing, while still allowing diffuse-only GI source construction.
void ComputeDirectLightSeparated(int idx, vec3 worldPos, vec3 N, vec3 V, PrincipledSurface surface, out vec3 diffuseOut, out vec3 specularOut) {
	diffuseOut = vec3(0.0);
	specularOut = vec3(0.0);

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
		if (dist > range) return;
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
			if (theta < outerCos) return;
			float eps = max(innerCos - outerCos, 0.001);
			float cone = clamp((theta - outerCos) / eps, 0.0, 1.0);
			cone = cone * cone * (3.0 - 2.0 * cone);
			attenuation *= cone;
		}
	}

	float NdotL = max(dot(N, L), 0.0);
	if (NdotL <= 0.0) return;

	vec3 diffuse, specular;
	EvaluatePrincipledBRDFSeparated(surface, N, V, L, diffuse, specular);
	
	// Shadow calculation
	int startSlice = int(Ld.shadowData.x + 0.5);
	int sliceCount = int(Ld.shadowData.y + 0.5);
	vec3 shadowLightDir = mix(normalize(lightPos - worldPos), lightDir, float(type == 0));
	
	float shadowMapShadow = ComputeShadowForLight(type, startSlice, sliceCount, worldPos, N, shadowLightDir, lightPos);
	
	// Apply realistic shadow darkening based on material properties
	// This makes shadows darker while accounting for indirect lighting and material characteristics
	float shadowEnableMask = float(enableShadows == 1) * float(Ld.shadowData.z > 0.5);
	shadowMapShadow = mix(1.0, ApplyRealisticShadow(shadowMapShadow), shadowEnableMask);

	float combinedShadow = shadowMapShadow;
	if (type == 0 && sssStrength > 0.001) {
		// Contact shadows are view-space directional refinements, so only blend for directional lights.
		float contactShadowVisibility = texture(screenSpaceShadowMap, vTexCoord).r;
		float viewDepth = length(worldPos - viewPos);

		float contactStrength = smoothstep(15.0, 1.0, viewDepth);
		contactStrength = mix(0.6, 1.0, contactStrength);

		float litAreaReduction = smoothstep(0.9, 1.0, shadowMapShadow);
		contactStrength *= (1.0 - litAreaReduction * 0.5);

		float contactBlend = clamp(contactStrength * sssStrength, 0.0, 1.0);
		float contactShadow = mix(1.0, ApplyRealisticShadow(contactShadowVisibility), contactBlend);
		combinedShadow = min(shadowMapShadow, contactShadow);
	}
	
	// Final contribution
	vec3 radiance = lightColor * attenuation;

	diffuseOut = diffuse * radiance * combinedShadow;
	specularOut = specular * radiance * combinedShadow;
}

// Proper PBR direct lighting with principled surface layering
vec3 ComputeDirectLight(int idx, vec3 worldPos, vec3 N, vec3 V, PrincipledSurface surface) {
	vec3 diffuse;
	vec3 specular;
	ComputeDirectLightSeparated(idx, worldPos, N, V, surface, diffuse, specular);
	return diffuse + specular;
}

vec3 ComputeIBL(vec3 N, vec3 V, PrincipledSurface surface, float diffuseAO, float specularAO) {
	vec3 iblResult = EvaluatePrincipledIBL(
		surface, N, V, reflect(-V, N),
		diffuseAO, specularAO,
		irradianceMap, prefilteredMap, brdfLUT,
		prefilteredMaxLOD, iblIntensity, diffuseIBLScale, specularIBLScale
	);
	
	if (any(isnan(iblResult)) || any(isinf(iblResult))) {
		return vec3(0.0);
	}
	
	return max(iblResult, vec3(0.0));
}

// LPV Helper functions
vec3 rotateVector(vec3 v, vec4 q) {
	vec3 qxyz = q.xyz;
	float qw = q.w;
	vec3 t = 2.0 * cross(qxyz, v);
	return v + qw * t + cross(qxyz, t);
}

vec3 rotateVectorInverse(vec3 v, vec4 q) {
	vec4 qConj = vec4(-q.x, -q.y, -q.z, q.w);
	return rotateVector(v, qConj);
}

vec3 WorldToVoxelUVW(vec3 worldPos) {
	vec3 localPos = worldPos - lpvGridCenter;
	localPos = rotateVectorInverse(localPos, lpvGridOrientation);
	vec3 voxelPos = (localPos / lpvVoxelSize) + vec3(lpvGridResolution * 0.5);
	return voxelPos / float(lpvGridResolution);
}

vec3 EvaluateSH(vec4 shR, vec4 shG, vec4 shB, vec3 normal) {
	float Y00 = 0.282095;
	float Y1_1 = 0.488603 * normal.x;
	float Y10 = 0.488603 * normal.y;
	float Y11 = 0.488603 * normal.z;
	
	vec4 shBasis = vec4(Y00, Y1_1, Y10, Y11);
	
	float irradianceR = dot(shR, shBasis);
	float irradianceG = dot(shG, shBasis);
	float irradianceB = dot(shB, shBasis);
	
	return max(vec3(irradianceR, irradianceG, irradianceB), vec3(0.0));
}

vec3 SampleLPV(vec3 worldPos, vec3 normal, vec3 albedo, float metallic, float diffuseAO) {
	if (enableLPV == 0) return vec3(0.0);
	
	vec3 uvw = WorldToVoxelUVW(worldPos);
	if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0)))) {
		return vec3(0.0);
	}
	
	vec4 shR = texture(lpvTextureR, uvw);
	vec4 shG = texture(lpvTextureG, uvw);
	vec4 shB = texture(lpvTextureB, uvw);
	
	vec3 localNormal = rotateVectorInverse(normal, lpvGridOrientation);
	vec3 irradiance = EvaluateSH(shR, shG, shB, localNormal);
	
	// Apply metallic factor here, use raw albedo
	vec3 kD = vec3(1.0 - metallic);
	vec3 giContribution = (albedo / PI) * irradiance * kD;
	giContribution *= lpvGIStrength * lpvDebugBoost * diffuseAO;
	
	return giContribution;
}

void main() {
	vec2 uv = vTexCoord;
	float depth = texture(gDepth, uv).r;
	if (depth >= 0.999999) { FragColor = vec4(0.0); return; }

	// Read material contract from the G-buffer using the shared unpack path.
	vec3 decodedNormal;
	PBRMaterial material = UnpackGBufferMaterial(
		gPackedNormalRM,
		gAlbedoAO,
		gSpecularF0,
		gMaterialID,
		gEmissive,
		gClearCoat,
		gPrincipledParams,
		uv,
		decodedNormal
	);

	float aoTex = material.ao;
	vec3 emissive = material.emissive;
	PrincipledSurface surface = BuildPrincipledSurfaceFromMaterial(material);

	// SSAO
	float ssao = clamp(texture(ssaoMap, uv).r, 0.0, 1.0);

	vec3 N = getNormalInWorldSpace(decodedNormal);
	
	// Validate normal
	if (length(N) < 0.5 || any(isnan(N))) {
		N = vec3(0.0, 1.0, 0.0);
	}
	N = normalize(N);
	
	vec3 worldPos = worldPosFromDepth(uv, depth);
	vec3 V = normalize(viewPos - worldPos);
	float NdotV = max(dot(N, V), 0.0);

	if (shadowDebugVisualization > 0) {
		vec3 shadowDebugColor;
		if (EvaluateShadowDebugView(worldPos, N, shadowDebugColor)) {
			FragColor = vec4(shadowDebugColor, 1.0);
			return;
		}
	}

	// AO factors
	float diffuseAO = mix(1.0, ssao, aoStrength) * aoTex;
	float specularAO = SpecularOcclusion(NdotV, diffuseAO, surface.perceptualRoughness);

	// Evaluate direct and indirect terms separately so indirect diffuse stays local and debuggable.
	vec3 directLighting = vec3(0.0);
	vec3 directDiffuseLighting = vec3(0.0);
	if (numLights > 0) {
		int maxLights = min(numLights, 64);
		for (int i = 0; i < maxLights; ++i) {
			vec3 diffuseLight;
			vec3 specularLight;
			ComputeDirectLightSeparated(i, worldPos, N, V, surface, diffuseLight, specularLight);
			directDiffuseLighting += diffuseLight;
			directLighting += diffuseLight + specularLight;
		}
	}

	if (lightingOutputMode == 1) {
		FragColor = vec4(max(directDiffuseLighting, vec3(0.0)), 1.0);
		return;
	}

	vec3 iblContribution = ComputeIBL(N, V, surface, diffuseAO, specularAO);
	vec3 lpvContribution = vec3(0.0);

	// LPV GI still uses the legacy diffuse-only path until volumetric GI is upgraded
	if (enableLPV == 1) {
		lpvContribution = SampleLPV(worldPos, N, material.albedo, material.metallic, diffuseAO);
		
		if (lpvDebugVisualization == 1) {
			FragColor = vec4(lpvContribution, 1.0);
			return;
		}
	}

	vec4 indirectSample = texture(indirectDiffuseMap, vTexCoord);
	vec3 indirectIrradiance = max(indirectSample.rgb, vec3(0.0));
	float indirectAO = clamp(indirectSample.a, 0.0, 1.0);
	float indirectAttenuation = mix(0.35, 1.0, indirectAO);
	vec3 indirectContribution = indirectIrradiance * surface.diffuseColor * indirectDiffuseStrength * indirectAttenuation * 2.5;
	indirectContribution = clamp(indirectContribution, vec3(0.0), vec3(6.0));

	vec3 color = emissive;
	color += directLighting;
	color += iblContribution + lpvContribution;
	if (indirectDiffuseCompositeMode == 1) {
		color *= vec3(1.0) + indirectContribution;
	}
	else {
		color += indirectContribution;
	}

	color = max(color, vec3(0.0));

	FragColor = vec4(color, 1.0);
}
