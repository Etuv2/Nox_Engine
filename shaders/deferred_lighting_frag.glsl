#version 460 core

// Include shared PBR functions
#include "includes/pbr_common.glsl"
#include "includes/material_common.glsl"

in vec2 vTexCoord;
out vec4 FragColor;

// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
// RT3: R8UI - Material ID (0=Standard PBR, 1=SpecGloss, 2=Transmission, etc.)
// RT4: RGBA16F - Emissive color (RGB) + unused (A)
uniform sampler2D gPackedNormalRM;  // RT0: oct normal (RG) + roughness (B) + metallic (A)
uniform sampler2D gAlbedoAO;        // RT1: albedo (RGB) + occlusion (A)
uniform sampler2D gSpecularF0;      // RT2: specular F0 (RGB) + emissive strength (A)
uniform usampler2D gMaterialID;     // RT3: material ID (uint8)
uniform sampler2D gEmissive;        // RT4: emissive color (RGB)
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
uniform float iblIntensity = 0.4;       // Overall IBL multiplier
uniform float diffuseIBLScale = 0.5;    // Diffuse irradiance scale
uniform float specularIBLScale = 0.6;   // Specular prefiltered scale

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
uniform float sssStrength = 1.0; // Contact shadow strength [0,1]

// Screen-Space Global Illumination (SSGI)
uniform sampler2D ssgiMap;
uniform float ssgiStrength = 1.0; // SSGI contribution strength [0,1]

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
uniform float shadowMinBrightness = 0.05;     // Minimum brightness in complete shadow (0.05 = 5% for subtle ambient)
uniform float shadowTransitionHardness = 1.0; // Softness of shadow boundaries [0.5=very soft, 2.0=sharp]

// Cascade split depths for view-depth blending (set from LightManager)
uniform vec4 cascadeSplits = vec4(10.0, 30.0, 100.0, 500.0);  // Far distances per cascade

// Debug visualization
uniform int debugCascadeVisualization = 0; // 0=off, 1=show cascade colors, 2=show depth

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

// Note: PI, INV_PI, DIELECTRIC_F0 are now defined in pbr_common.glsl
// Note: DecodeNormalOct, DistributionGGX, GeometrySchlickGGX, GeometrySmith, 
//       FresnelSchlick, CalculateDiffuseAlbedo, SpecularOcclusion are in pbr_common.glsl

// Reconstruct F0 - no longer needed, we store full F0 directly!
// This function is kept for compatibility but just returns the stored F0
vec3 ReconstructF0(vec3 specularF0, vec3 albedo, float metallic) {
    // F0 is already correctly computed and stored in G-buffer
    // For metals, F0 = albedo (handled in G-buffer pass)
    // For dielectrics, F0 = computed dielectric F0 with specular color
    return specularF0;
}

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

bool inUnitCube(vec3 p) {
	return all(greaterThanEqual(p, vec3(0.0))) && all(lessThanEqual(p, vec3(1.0)));
}

// More lenient bounds check for shadow sampling - allows slight depth overflow
bool inShadowBounds(vec3 p) {
	return p.x >= 0.0 && p.x <= 1.0 && 
	       p.y >= 0.0 && p.y <= 1.0 && 
	       p.z >= -0.01 && p.z <= 1.01;
}

// Cascade debug colors for visualization
vec3 GetCascadeDebugColor(int cascadeIndex) {
	if (cascadeIndex == 0) return vec3(1.0, 0.0, 0.0); // Red
	if (cascadeIndex == 1) return vec3(0.0, 1.0, 0.0); // Green
	if (cascadeIndex == 2) return vec3(0.0, 0.0, 1.0); // Blue
	if (cascadeIndex == 3) return vec3(1.0, 1.0, 0.0); // Yellow
	return vec3(1.0, 0.0, 1.0); // Magenta for invalid
}

// Global variable to store last used cascade for debug visualization
int g_lastCascadeIndex = -1;

// Apply realistic shadow darkening
// Accounts for indirect lighting, material reflectivity, and perceptual shadow intensity
float ApplyRealisticShadow(float shadowVisibility, vec3 albedo, float roughness, float metallic, float ao) {
	// Rougher surfaces receive more ambient light in shadow (better light scattering)
	float roughnessInfluence = mix(0.5, 1.0, roughness * roughness);
	
	// Metals stay mostly dark in shadow (minimal diffuse scattering)
	// Dielectrics get more ambient bounce light
	float metallicInfluence = mix(1.0, 0.3, metallic);
	
	// Darker surfaces appear darker in shadow (less light bounces back)
	// Use relative luminance
	float albedoLuminance = dot(albedo, vec3(0.299, 0.587, 0.114));
	float albedoInfluence = mix(0.4, 1.0, albedoLuminance);
	
	// Calculate ambient shadow contribution (indirect light in shadow)
	// Combine all factors for physically-plausible ambient falloff
	float ambientShadowFactor = roughnessInfluence * metallicInfluence * albedoInfluence * ao;
	
	// Shadow transition: blend between full darkness and ambient
	// shadowVisibility: 1.0 = lit, 0.0 = fully shadowed
	float shadowIntensity = 1.0 - shadowVisibility;
	
	// Apply shadow with material-dependent ambient contribution
	// In shadow: result interpolates from ambientShadowFactor to shadowMinBrightness
	// In light: result is full brightness (1.0)
	float ambientInShadow = mix(shadowMinBrightness, ambientShadowFactor, ao);
	float result = mix(ambientInShadow, 1.0, shadowVisibility);
	
	// Apply darkness multiplier
	result = mix(1.0, result, shadowDarkness);
	
	return result;
}
// Slope-scaled shadow bias calculation - physically reasonable biasing
float CalculateSlopeBias(vec3 N, vec3 L, float baseBias) {
	float NdotL = max(dot(N, L), 0.001);
	// Slope factor: tan(angle) approximated as sqrt(1 - cos^2) / cos
	float cosAngle = NdotL;
	float sinAngle = sqrt(max(1.0 - cosAngle * cosAngle, 0.0));
	float tanAngle = sinAngle / cosAngle;
	
	// Clamp slope factor to prevent extreme bias on grazing angles
	float slopeFactor = clamp(tanAngle, 0.0, 10.0);
	
	return baseBias * (1.0 + slopeFactor * 0.5);
}

// Adaptive shadow bias calculation for cascaded shadow maps
float CalculateAdaptiveShadowBias(vec3 N, vec3 Ld, int cascadeIndex, float depthComp, float distance) {
	// Use slope-based bias as the primary mechanism
	float slopeBias = CalculateSlopeBias(N, -Ld, shadowBias);
	
	// Scale by cascade - distant cascades cover larger areas and need more bias
	float cascadeScale = 1.0 + float(cascadeIndex) * 0.1;
	
	// Small distance-based component to handle depth precision issues at range
	float depthBias = shadowBias * 0.0001 * distance;
	
	float finalBias = slopeBias * cascadeScale + depthBias;
	return clamp(finalBias, shadowBias * 0.25, maxShadowBias);
}

float SampleShadowArray(int layer, vec3 projCoords, float bias) {
	// Use lenient bounds for depth, strict for XY
	if (!inShadowBounds(projCoords)) return 1.0;
	
	// Clamp depth to valid range for sampling
	float sampleDepth = clamp(projCoords.z, 0.0, 1.0);
	
	ivec3 dims = textureSize(multiLightShadowArray, 0);
	vec2 texel = 1.0 / vec2(dims.xy);
	
	const vec2 poissonDisk[16] = vec2[](
		vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
		vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
		vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
		vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
		vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
		vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
		vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
		vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
	);
	
	float sum = 0.0;
	int count = 16;
	
	for (int i = 0; i < count; ++i) {
		vec2 offset = poissonDisk[i] * texel * 1.5;
		vec2 uv = projCoords.xy + offset;
		
		if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
			sum += 1.0;
		} else {
			sum += texture(multiLightShadowArray, vec4(uv, float(layer), sampleDepth - bias));
		}
	}
	
	return sum / float(count);
}
// Cascaded shadow mapping with cascade selection and smooth blending
float ComputeCascadedShadow(int startSlice, int sliceCount, vec3 worldPos, vec3 N, vec3 lightDir) {
	vec3 viewSpacePos = (view * vec4(worldPos, 1.0)).xyz;
	float viewDepth = -viewSpacePos.z; // Linear view-space depth (positive)
	
	// Clamp sliceCount to valid range
	sliceCount = min(sliceCount, 4);
	if (sliceCount <= 0) return 1.0;
	
	// Beyond max shadow distance - fade out smoothly
	float maxCascadeDepth = cascadeSplits[sliceCount - 1];
	if (viewDepth > maxCascadeDepth) {
		// Smooth fade over 10% beyond max cascade
		float fadeStart = maxCascadeDepth;
		float fadeEnd = maxCascadeDepth * 1.1;
		float fadeFactor = clamp((viewDepth - fadeStart) / (fadeEnd - fadeStart), 0.0, 1.0);
		if (fadeFactor >= 1.0) return 1.0;
		// Continue to sample last cascade but fade result
	}
	
	// Try each cascade in order and find the first one that contains this point
	// This is more robust than calculating the cascade from depth
	int bestCascade = -1;
	vec3 bestProjCoords = vec3(0.0);
	float bestCoverage = 0.0; // How well the cascade covers this point (0 = edge, 1 = center)
	
	for (int i = 0; i < sliceCount; ++i) {
		int layer = startSlice + i;
		mat4 M = shadowMatrices[layer];
		vec4 lsp = M * vec4(worldPos, 1.0);
		vec3 pc = lsp.xyz / lsp.w;
		pc = pc * 0.5 + 0.5;
		
		// Check if this cascade covers the point
		if (inShadowBounds(pc)) {
			// Calculate how well-centered the point is in this cascade
			// Prefer cascades where the point is more centered (not at edges)
			vec2 centerDist = abs(pc.xy - 0.5);
			float coverage = 1.0 - max(centerDist.x, centerDist.y) * 2.0;
			coverage = clamp(coverage, 0.0, 1.0);
			
			// Also prefer the cascade that matches the view depth best
			float cascadeNear = (i == 0) ? 0.0 : cascadeSplits[i - 1];
			float cascadeFar = cascadeSplits[i];
			
			// Is this the "correct" cascade for this depth?
			bool isCorrectCascade = (viewDepth >= cascadeNear && viewDepth <= cascadeFar);
			
			if (isCorrectCascade) {
				// This is the ideal cascade - use it
				bestCascade = i;
				bestProjCoords = pc;
				bestCoverage = coverage;
				break; // Found the best one
			} else if (bestCascade < 0 || coverage > bestCoverage) {
				// First valid cascade or better coverage than previous
				bestCascade = i;
				bestProjCoords = pc;
				bestCoverage = coverage;
			}
		}
	}
	
	// No cascade covers this point
	if (bestCascade < 0) {
		g_lastCascadeIndex = -1;
		return 1.0;
	}
	
	// Store cascade index for debug visualization
	g_lastCascadeIndex = bestCascade;
	
	// Sample the best cascade
	int layer0 = startSlice + bestCascade;
	float bias0 = CalculateAdaptiveShadowBias(N, lightDir, bestCascade, bestProjCoords.z, viewDepth);
	float shadow0 = SampleShadowArray(layer0, bestProjCoords, bias0);
	
	// Cascade blending - blend with next cascade at boundaries
	float cascadeNear = (bestCascade == 0) ? 0.0 : cascadeSplits[bestCascade - 1];
	float cascadeFar = cascadeSplits[bestCascade];
	float cascadeRange = max(cascadeFar - cascadeNear, 0.001);
	
	// Calculate blend factor for transition to next cascade
	if (bestCascade < sliceCount - 1) {
		float blendZoneSize = cascadeRange * cascadeBlendFactor;
		blendZoneSize = min(blendZoneSize, cascadeBlendDistance);
		blendZoneSize = max(blendZoneSize, cascadeRange * 0.05); // At least 5% blend zone
		float blendStart = cascadeFar - blendZoneSize;
		
		if (viewDepth > blendStart) {
			float blendFactor = (viewDepth - blendStart) / blendZoneSize;
			blendFactor = clamp(blendFactor, 0.0, 1.0);
			blendFactor = smoothstep(0.0, 1.0, blendFactor);
			
			// Try to sample next cascade
			int nextCascade = bestCascade + 1;
			int layer1 = startSlice + nextCascade;
			mat4 M1 = shadowMatrices[layer1];
			vec4 lsp1 = M1 * vec4(worldPos, 1.0);
			vec3 pc1 = lsp1.xyz / lsp1.w;
			pc1 = pc1 * 0.5 + 0.5;
			
			if (inShadowBounds(pc1)) {
				float bias1 = CalculateAdaptiveShadowBias(N, lightDir, nextCascade, pc1.z, viewDepth);
				float shadow1 = SampleShadowArray(layer1, pc1, bias1);
				shadow0 = mix(shadow0, shadow1, blendFactor);
			}
		}
	}
	
	// Apply distance fade for last cascade
	if (viewDepth > maxCascadeDepth) {
		float fadeStart = maxCascadeDepth;
		float fadeEnd = maxCascadeDepth * 1.1;
		float fadeFactor = clamp((viewDepth - fadeStart) / (fadeEnd - fadeStart), 0.0, 1.0);
		shadow0 = mix(shadow0, 1.0, fadeFactor);
	}
	
	// Edge fade - fade shadows near cascade projection edges to avoid hard cuts
	vec2 edgeDist = min(bestProjCoords.xy, 1.0 - bestProjCoords.xy);
	float minEdgeDist = min(edgeDist.x, edgeDist.y);
	if (minEdgeDist < 0.05) {
		float edgeFade = minEdgeDist / 0.05;
		shadow0 = mix(1.0, shadow0, edgeFade);
	}
	
	return shadow0;
}

// Edge-safe shadow sampling for point light cubemap faces
// Constrains filter kernel near edges to avoid sampling undefined space
float SampleShadowArrayEdgeSafe(int layer, vec3 projCoords, float bias, float edgeMargin) {
	if (!inShadowBounds(projCoords)) return 1.0;
	
	ivec3 dims = textureSize(multiLightShadowArray, 0);
	vec2 texel = 1.0 / vec2(dims.xy);
	
	// Calculate distance to nearest edge
	vec2 edgeDist = min(projCoords.xy, 1.0 - projCoords.xy);
	float minEdgeDist = min(edgeDist.x, edgeDist.y);
	
	// Determine kernel scale based on edge proximity
	// Near edges, reduce filter radius to stay within valid texels
	float maxKernelRadius = minEdgeDist / texel.x;
	float kernelScale = clamp(maxKernelRadius / 1.5, 0.0, 1.0);
	
	// Clamp depth to valid range
	float sampleDepth = clamp(projCoords.z, 0.0, 1.0);
	
	// If very close to edge, use single sample
	if (kernelScale < 0.1) {
		vec2 clampedUV = clamp(projCoords.xy, texel * 0.5, 1.0 - texel * 0.5);
		return texture(multiLightShadowArray, vec4(clampedUV, float(layer), sampleDepth - bias));
	}
	
	const vec2 poissonDisk[16] = vec2[](
		vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
		vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
		vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
		vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
		vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
		vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
		vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
		vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790)
	);
	
	float sum = 0.0;
	int count = 16;
	float filterRadius = 1.5 * kernelScale;
	
	for (int i = 0; i < count; ++i) {
		vec2 offset = poissonDisk[i] * texel * filterRadius;
		vec2 uv = projCoords.xy + offset;
		
		// Clamp to valid texture region (half-texel inset from edges)
		uv = clamp(uv, texel * 0.5, 1.0 - texel * 0.5);
		
		sum += texture(multiLightShadowArray, vec4(uv, float(layer), sampleDepth - bias));
	}
	
	return sum / float(count);
}

float ComputePointLightShadow(int startSlice, vec3 worldPos, vec3 N, vec3 lightPos) {
	vec3 toLight = worldPos - lightPos;
	float distance = length(toLight);
	vec3 lightDir = normalize(toLight);
	
	// Use world position directly - normal offset causes shadow displacement issues
	// The bias in depth comparison handles self-shadowing
	vec3 shadowPos = worldPos;
	vec3 offsetToLight = shadowPos - lightPos;
	
	// Face selection matching the render pass exactly
	// Faces: +X=0, -X=1, +Y=2, -Y=3, +Z=4, -Z=5
	vec3 absDir = abs(offsetToLight);
	int face;
	
	if (absDir.x >= absDir.y && absDir.x >= absDir.z) {
		face = (offsetToLight.x > 0.0) ? 0 : 1;
	} else if (absDir.y >= absDir.x && absDir.y >= absDir.z) {
		face = (offsetToLight.y > 0.0) ? 2 : 3;
	} else {
		face = (offsetToLight.z > 0.0) ? 4 : 5;
	}
	
	int layer = startSlice + face;
	mat4 M = shadowMatrices[layer];
	vec4 lsp = M * vec4(shadowPos, 1.0);
	
	// Perspective divide
	vec3 ndc = lsp.xyz / lsp.w;
	vec3 pc = ndc * 0.5 + 0.5;
	
	// For point lights, only check XY bounds - Z can exceed 1.0 for distant objects
	// The depth comparison will naturally handle objects beyond the light range
	if (pc.x < 0.0 || pc.x > 1.0 || pc.y < 0.0 || pc.y > 1.0) {
		return 1.0;
	}
	
	// Clamp depth to valid range for sampling
	float sampleDepth = clamp(pc.z, 0.0, 1.0);
	
	// If beyond the shadow map's far plane, object is outside light range - no shadow
	if (pc.z > 1.0) {
		return 1.0;
	}
	
	// Slope-scaled bias for point lights
	float NdotL = max(dot(N, -lightDir), 0.001);
	float slopeScale = sqrt(1.0 - NdotL * NdotL) / NdotL;
	slopeScale = clamp(slopeScale, 0.0, 3.0);
	float bias = pointLightBias * (1.0 + slopeScale * 0.5);
	
	// Use edge-safe sampling to avoid seams at cubemap face boundaries
	vec3 sampleCoords = vec3(pc.xy, sampleDepth);
	float shadow = SampleShadowArrayEdgeSafe(layer, sampleCoords, bias, 0.05);
	
	return shadow;
}
// Compute shadowing for a given light source
float ComputeShadowForLight(int lightType, int startSlice, int sliceCount, vec3 worldPos, vec3 N, vec3 lightDir, vec3 lightPos) {
	if (sliceCount <= 0 || startSlice < 0) return 1.0;

	if (lightType == 0 && sliceCount > 1) {
		return ComputeCascadedShadow(startSlice, sliceCount, worldPos, N, lightDir);
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
		if (!inUnitCube(pc)) return 1.0;
		
		float bias = CalculateAdaptiveShadowBias(N, -spotDir, 0, pc.z, distance);
		return SampleShadowArray(layer, pc, bias);
	}
}

// Proper PBR direct lighting with correct albedo application
vec3 ComputeDirectLight(int idx, vec3 worldPos, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 F0, float diffuseAO) {
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
		if (dist > range) return vec3(0.0);
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
			if (theta < outerCos) return vec3(0.0);
			float eps = max(innerCos - outerCos, 0.001);
			float cone = clamp((theta - outerCos) / eps, 0.0, 1.0);
			cone = cone * cone * (3.0 - 2.0 * cone);
			attenuation *= cone;
		}
	}

	float NdotL = max(dot(N, L), 0.0);
	if (NdotL <= 0.0) return vec3(0.0);

	vec3 H = normalize(V + L);
	float NdotV = max(dot(N, V), 0.001);
	float HdotV = max(dot(H, V), 0.0);
	
	// Cook-Torrance BRDF
	float NDF = DistributionGGX(N, H, roughness);
	float G = GeometrySmith(N, V, L, roughness);
	vec3 F = FresnelSchlick(HdotV, F0);
	
	// Specular BRDF
	vec3 numerator = NDF * G * F;
	float denominator = 4.0 * NdotV * NdotL;
	vec3 specular = numerator / max(denominator, 0.001);
	
	// Energy conservation: kS is what's reflected (specular), kD is what's refracted (diffuse)
	// Metals have no diffuse, so multiply by (1 - metallic)
	vec3 kS = F;
	vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
	
	// Use raw albedo here, kD already handles the metallic factor
	// Lambertian diffuse = albedo / PI
	vec3 diffuse = kD * albedo / PI;
	
	// Shadow calculation
	int startSlice = int(Ld.shadowData.x + 0.5);
	int sliceCount = int(Ld.shadowData.y + 0.5);
	vec3 shadowLightDir = mix(normalize(lightPos - worldPos), lightDir, float(type == 0));
	
	float shadowMapShadow = ComputeShadowForLight(type, startSlice, sliceCount, worldPos, N, shadowLightDir, lightPos);
	
	// Apply realistic shadow darkening based on material properties
	// This makes shadows darker while accounting for indirect lighting and material characteristics
	float shadowEnableMask = float(enableShadows == 1) * float(Ld.shadowData.z > 0.5);
	shadowMapShadow = mix(1.0, ApplyRealisticShadow(shadowMapShadow, albedo, roughness, metallic, diffuseAO), shadowEnableMask);

	// Contact shadows - blend with shadow map
	float contactShadowVisibility = texture(screenSpaceShadowMap, vTexCoord).r;
	float viewDepth = length(worldPos - viewPos);
   
	// Contact shadow strength increases close to camera, fades out at distance
	float contactStrength = smoothstep(15.0, 1.0, viewDepth);
	contactStrength = mix(0.6, 1.0, contactStrength);
	
	// Reduce contact shadow strength where shadow map already shows shadows
	// This prevents over-darkening where both systems agree
	float litAreaReduction = smoothstep(0.9, 1.0, shadowMapShadow);
	contactStrength *= (1.0 - litAreaReduction * 0.5);
	
	// Apply contact shadows with realistic darkening
	float contactShadow = mix(ApplyRealisticShadow(contactShadowVisibility, albedo, roughness, metallic, diffuseAO), 1.0, 1.0 - contactStrength * sssStrength);
	
	// Combine shadow map and contact shadows with realistic blending
	float combinedShadow = shadowMapShadow * contactShadow;
	combinedShadow = max(combinedShadow, shadowMinBrightness);
	
	// Final contribution
	vec3 radiance = lightColor * attenuation;
	
	return (diffuse * diffuseAO + specular) * radiance * NdotL * combinedShadow;
}

vec3 ComputeIBL(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 F0, float diffuseAO, float specularAO) {
	vec3 R = reflect(-V, N);
	roughness = max(roughness, 0.04);
  
	vec3 irradiance = texture(irradianceMap, N).rgb;
	
	float lod = roughness * prefilteredMaxLOD;
	vec3 prefiltered = textureLod(prefilteredMap, R, lod).rgb;
	
	irradiance = max(irradiance, vec3(0.0));
	prefiltered = max(prefiltered, vec3(0.0));
	
	float NdotV = max(dot(N, V), 0.0);
	vec2 brdf = texture(brdfLUT, vec2(NdotV, roughness)).rg;
	brdf = max(brdf, vec2(0.0));

	vec3 F = FresnelSchlick(NdotV, F0);
	
	// Energy conservation
	vec3 kS = F;
	vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
	
	// Diffuse IBL uses raw albedo, kD handles metallic
	vec3 diffuse = kD * albedo * irradiance * diffuseAO * diffuseIBLScale;
	
	// Specular IBL
	vec3 specular = prefiltered * (F * brdf.x + brdf.y) * specularAO * specularIBLScale;
	
	diffuse = max(diffuse, vec3(0.0));
	specular = max(specular, vec3(0.0));
	
	vec3 iblResult = (diffuse + specular) * iblIntensity;
	
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
	if (depth >= 0.9999) { FragColor = vec4(0.0); return; }

	// Read material ID
	uint materialID = texture(gMaterialID, uv).r;

	// Unpack G-buffer
	vec4 packedNRM = texture(gPackedNormalRM, uv);
	vec4 albedoAO = texture(gAlbedoAO, uv);
	vec4 specF0Data = texture(gSpecularF0, uv);
	vec4 emissiveData = texture(gEmissive, uv);
	
	// Extract material properties
	vec2 encNormal = packedNRM.rg;
	float roughness = clamp(packedNRM.b, 0.04, 1.0);
	float metallic = clamp(packedNRM.a, 0.0, 1.0);
	
	// Extract albedo (base color) directly from G-buffer
	vec3 albedo = albedoAO.rgb;
	
	// Only validate for NaN/Inf, NOT for dark colors
	// Black materials (like tires) are perfectly valid and should not be overridden
	if (any(isnan(albedo)) || any(isinf(albedo))) {
		albedo = vec3(0.5); // Fallback only for invalid data
	}
	
	float aoTex = clamp(albedoAO.a, 0.0, 1.0);
	
	// Extract full specular F0 color (RGB) - no more luminance compression!
	vec3 specularF0 = specF0Data.rgb;
	float emissiveStrength = specF0Data.a;
	
	// Extract emissive color
	vec3 emissive = emissiveData.rgb * emissiveStrength;

	// SSAO
	float ssao = clamp(texture(ssaoMap, uv).r, 0.0, 1.0);

	// Decode normal
	vec3 decodedNormal = DecodeNormalOct(encNormal);
	vec3 N = getNormalInWorldSpace(decodedNormal);
	
	// Validate normal
	if (length(N) < 0.5 || any(isnan(N))) {
		N = vec3(0.0, 1.0, 0.0);
	}
	N = normalize(N);
	
	vec3 worldPos = worldPosFromDepth(uv, depth);
	vec3 V = normalize(viewPos - worldPos);
	float NdotV = max(dot(N, V), 0.0);

	// Use stored F0 directly - no reconstruction needed!
	vec3 F0 = ReconstructF0(specularF0, albedo, metallic);

	// AO factors
	float diffuseAO = mix(1.0, ssao, aoStrength) * aoTex;
	float specularAO = SpecularOcclusion(NdotV, diffuseAO, roughness);

	// Start with emissive
	vec3 color = emissive;

	// Material routing allows different BRDF models per surface
	if (materialID == 2u) {
		// Transmissive/Glass material (ID 2)
		// TODO: Implement refraction and transmission in future update(s)
		// For now, use standard PBR with high specular
		roughness = min(roughness, 0.1); // Force smooth for glass-like appearance
	}

	// Direct lighting - pass raw albedo, metallic factor is applied inside
	if (numLights > 0) {
		int maxLights = min(numLights, 64);
		for (int i = 0; i < maxLights; ++i) {
			color += ComputeDirectLight(i, worldPos, N, V, albedo, metallic, roughness, F0, diffuseAO);
		}
	} else {
		// Fallback ambient lighting
		vec3 Ld = normalize(vec3(0.2, -0.8, -0.3));
		float NdotL = max(dot(N, Ld), 0.0);
		// Apply metallic factor for fallback too
		vec3 diffuseColor = albedo * (1.0 - metallic);
		color += (diffuseColor / PI) * NdotL * 0.3 * diffuseAO;
	}

	// IBL - pass raw albedo
	color += ComputeIBL(N, V, albedo, metallic, roughness, F0, diffuseAO, specularAO);

	// LPV GI - pass raw albedo
	if (enableLPV == 1) {
		vec3 lpvContribution = SampleLPV(worldPos, N, albedo, metallic, diffuseAO);
		
		if (lpvDebugVisualization == 1) {
			FragColor = vec4(lpvContribution, 1.0);
			return;
		}
		
		color += lpvContribution;
	}

	// SSGI - ssgiIndirect is already irradiance, don't multiply by albedo
	// The sampled hit color contains final radiance with albedo baked in
	// We apply metallic factor and diffuse BRDF (1/PI) for energy conservation
	vec3 ssgiIndirect = texture(ssgiMap, vTexCoord).rgb;
	vec3 ssgiContribution = ssgiIndirect * (1.0 - metallic) * INV_PI * ssgiStrength * diffuseAO;
	color += ssgiContribution;

	// Safety fallback
	if (length(color) < 0.0005) {
		color += albedo * (1.0 - metallic) * 0.1 * diffuseAO;
	}

	color = max(color, vec3(0.0));

	// Debug cascade visualization
	if (debugCascadeVisualization == 1 && g_lastCascadeIndex >= 0) {
		// Overlay cascade colors
		vec3 cascadeColor = GetCascadeDebugColor(g_lastCascadeIndex);
		color = mix(color, cascadeColor, 0.5);
	} else if (debugCascadeVisualization == 2) {
		// Show view depth as grayscale
		vec3 viewSpacePos = (view * vec4(worldPos, 1.0)).xyz;
		float viewDepth = -viewSpacePos.z;
		float normalizedDepth = viewDepth / cascadeSplits[3];
		color = vec3(normalizedDepth);
	}

	FragColor = vec4(color, 1.0);
}
