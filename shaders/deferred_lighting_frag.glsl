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
uniform vec3 viewPos;

// Normal space configuration
uniform int normalsInWorldSpace = 1; // 1 if normals in G-buffer are world space, 0 if view space

// IBL
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform float prefilteredMaxLOD;

// IBL intensity controls to prevent over-bright results
uniform float iblIntensity = 0.1;        // Overall IBL multiplier
uniform float diffuseIBLScale = 0.3;    // Diffuse irradiance scale
uniform float specularIBLScale = 0.45;   // Specular prefiltered scale

// SSAO
uniform sampler2D ssaoMap;
uniform float aoStrength = 0.9;

// Screen-Space Shadows (Contact Shadows)
uniform sampler2D screenSpaceShadowMap;
uniform float sssStrength = 0.6; // Contact shadow blend strength [0,1]

const int MAX_INDIRECT_DIFFUSE_SOURCES = 4;
const float SURFEL_GI_RESPONSE_FORM_FACTOR_COMPENSATION = 22.0;
uniform sampler2D indirectDiffuseMaps[MAX_INDIRECT_DIFFUSE_SOURCES];
uniform float indirectDiffuseStrengths[MAX_INDIRECT_DIFFUSE_SOURCES];
uniform int indirectDiffuseSourceCount = 0;
uniform int indirectDiffuseCompositeMode = 0; // 0 additive, 1 modulative
uniform int lightingOutputMode = 0; // 0 full lighting, 1 bounceable radiance
uniform int uLightingCompositeDebugMode = 0; // 0 full, 1 direct, 2 IBL, 3 SSGI, 4 surfel, 5 LPV

// Debug visualization
uniform int shadowDebugVisualization = 0; // 0=off, 1=cascade index, 2=raw depth, 3=bias, 4=texel density, 5=shadow mask

#include "includes/lighting_common.glsl"

// Note: PI, INV_PI, DIELECTRIC_F0 are now defined in pbr_common.glsl
// Note: DecodeNormalOct, DistributionGGX, GeometrySchlickGGX, GeometrySmith, 
//       FresnelSchlick, CalculateDiffuseAlbedo, SpecularOcclusion are in pbr_common.glsl

vec3 worldPosFromDepth(vec2 uv, float depth) {
	return ReconstructWorldPosition(uv, depth, invProjection, invView);
}

vec3 getNormalInWorldSpace(vec3 decodedNormal) {
	if (normalsInWorldSpace == 1) {
		return decodedNormal;
	} else {
		return normalize(mat3(invView) * decodedNormal);
	}
}

vec3 EvaluateDirectionalShadowDebug(LightData Ld, vec3 worldPos, vec3 N) {
	int startSlice = int(Ld.shadowData.x + 0.5);
	int sliceCount = int(Ld.shadowData.y + 0.5);
	vec3 lightDir = normalize(Ld.direction.xyz);

	int cascadeIndex;
	vec3 projCoords;
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
		// Receiver normal offset in cascade texels: green = none, red = 4+ texels.
		int layer = startSlice + cascadeIndex;
		ivec3 dims = textureSize(multiLightShadowArray, 0);
		float texelWorld = NoxShadowTexelWorldSize(shadowMatrices[layer], worldPos, float(dims.x));
		float filterRadius = ComputeAdaptiveFilterRadiusTexels(projCoords, dims, 0.05, 1.0);
		float normalOffset = ComputeShadowNormalOffsetWorld(layer, worldPos, N, -lightDir, filterRadius);
		float normalizedBias = clamp(normalOffset / max(texelWorld * 4.0, 1e-6), 0.0, 1.0);
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
	float shadowMapShadow;
	if (!EvaluateLightRadiance(idx, worldPos, N, V, surface, diffuseOut, specularOut, shadowMapShadow)) {
		return;
	}

	float combinedShadow = shadowMapShadow;
	if (int(lights[idx].position.w) == 0 && sssStrength > 0.001) {
		// Contact shadows are view-space directional refinements, so only blend for directional lights.
		float contactShadowVisibility = texture(screenSpaceShadowMap, vTexCoord).r;
		float viewDepth = length(worldPos - viewPos);

		float contactStrength = smoothstep(18.0, 0.75, viewDepth);
		float shadowMapLitMask = smoothstep(0.35, 0.95, shadowMapShadow);
		float contactBlend = clamp(contactStrength * shadowMapLitMask * sssStrength, 0.0, 1.0);
		float contactShadow = mix(1.0, ApplyRealisticShadow(contactShadowVisibility), contactBlend);
		combinedShadow = mix(shadowMapShadow, min(shadowMapShadow, contactShadow), contactBlend);
	}

	diffuseOut *= combinedShadow;
	specularOut *= combinedShadow;
}

// Proper PBR direct lighting with principled surface layering
vec3 ComputeDirectLight(int idx, vec3 worldPos, vec3 N, vec3 V, PrincipledSurface surface) {
	vec3 diffuse;
	vec3 specular;
	ComputeDirectLightSeparated(idx, worldPos, N, V, surface, diffuse, specular);
	return diffuse + specular;
}

vec3 ComputeIBL(vec3 N, vec3 V, PrincipledSurface surface, float diffuseAO, float specularAO) {
	if (iblIntensity <= 0.0001 || (diffuseIBLScale <= 0.0001 && specularIBLScale <= 0.0001)) {
		return vec3(0.0);
	}

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

vec3 ComputeDiffuseIBLSource(vec3 N, vec3 V, PrincipledSurface surface, float diffuseAO) {
	if (iblIntensity <= 0.0001 || diffuseIBLScale <= 0.0001) {
		return vec3(0.0);
	}

	float NdotV = Saturate(dot(N, V));
	vec3 irradiance = max(texture(irradianceMap, N).rgb, vec3(0.0));
	vec2 brdf = SampleBRDFLUT(brdfLUT, NdotV, surface.perceptualRoughness);
	vec3 FssEss;
	vec3 FmsEms;
	ComputeIBLSpecularEnergy(surface.specularF0, NdotV, brdf, FssEss, FmsEms);

	float transmissionWeight = ComputeTransmissionWeight(surface.transmission, NdotV, surface.specularF0);
	float diffuseTerm = mix(1.0, 1.0 + 0.5 * surface.perceptualRoughness, surface.subsurface);
	vec3 kD = max(vec3(1.0) - (FssEss + FmsEms), vec3(0.0)) * (1.0 - transmissionWeight);
	vec3 diffuse = kD * surface.diffuseColor * irradiance * diffuseAO * diffuseIBLScale * diffuseTerm;
	return max(diffuse * iblIntensity, vec3(0.0));
}

vec3 ComputeBounceableIBLSource(vec3 N, vec3 V, PrincipledSurface surface, float diffuseAO, float specularAO) {
	vec3 diffuseSource = ComputeDiffuseIBLSource(N, V, surface, diffuseAO);
	if (iblIntensity <= 0.0001 || specularIBLScale <= 0.0001) {
		return diffuseSource;
	}

	float NdotV = Saturate(dot(N, V));
	vec3 R = reflect(-V, N);
	vec2 brdf = SampleBRDFLUT(brdfLUT, NdotV, surface.perceptualRoughness);
	vec3 FssEss;
	vec3 FmsEms;
	ComputeIBLSpecularEnergy(surface.specularF0, NdotV, brdf, FssEss, FmsEms);

	float baseGloss = pow(1.0 - surface.perceptualRoughness, 2.0);
	vec3 prefiltered = max(textureLod(prefilteredMap, R, surface.perceptualRoughness * prefilteredMaxLOD).rgb, vec3(0.0));
	vec3 glossySource = prefiltered * (FssEss + FmsEms) * baseGloss * mix(specularAO, 1.0, 0.25) * specularIBLScale;

	float baseAttenuation = ComputeBaseLayerAttenuation(surface, NdotV);
	vec3 source = diffuseSource + glossySource * baseAttenuation * iblIntensity * 0.35;
	return max(source, vec3(0.0));
}

vec3 ComputeIndirectGIResponse(vec3 indirectIrradiance, vec3 N, vec3 V, PrincipledSurface surface, float diffuseAO, float specularAO) {
	float NdotV = Saturate(dot(N, V));

	vec3 F = FresnelSchlickRoughness(NdotV, surface.specularF0, surface.perceptualRoughness);
	vec3 kD = clamp(vec3(1.0) - F, vec3(0.0), vec3(1.0));
	float transmissionWeight = ComputeTransmissionWeight(surface.transmission, NdotV, surface.specularF0);
	vec3 diffuseAlbedo = surface.baseColor * (1.0 - surface.metallic) * (1.0 - transmissionWeight);
	float diffuseTerm = mix(1.0, 1.0 + 0.35 * surface.perceptualRoughness, surface.subsurface);
	float aoPolicy = mix(0.55, 1.0, clamp(diffuseAO, 0.0, 1.0));
	float baseAttenuation = ComputeBaseLayerAttenuation(surface, NdotV);
	vec3 materialResponse = kD * diffuseAlbedo * INV_PI * diffuseTerm * aoPolicy * baseAttenuation;
	return max(indirectIrradiance * materialResponse, vec3(0.0));
}

vec3 ComputeSurfelGIResponse(vec3 indirectIrradiance, float indirectConfidence, vec3 N, vec3 V, PrincipledSurface surface, float diffuseAO, float specularAO) {
	float NdotV = Saturate(dot(N, V));

	vec3 F = FresnelSchlickRoughness(NdotV, surface.specularF0, surface.perceptualRoughness);
	vec3 kD = clamp(vec3(1.0) - F, vec3(0.0), vec3(1.0));
	float transmissionWeight = ComputeTransmissionWeight(surface.transmission, NdotV, surface.specularF0);
	vec3 diffuseAlbedo = surface.baseColor * (1.0 - surface.metallic) * (1.0 - transmissionWeight);
	float diffuseTerm = mix(1.0, 1.0 + 0.35 * surface.perceptualRoughness, surface.subsurface);
	float indirectLum = Luminance(indirectIrradiance);
	float indirectChroma = length(indirectIrradiance - vec3(indirectLum));
	float indirectChromaSignal = smoothstep(0.012, 0.18, indirectChroma / max(indirectLum, 0.06));

	float aoPolicy = mix(0.55, 1.0, clamp(diffuseAO, 0.0, 1.0));
	float baseAttenuation = ComputeBaseLayerAttenuation(surface, NdotV);
	float confidenceLift = smoothstep(0.025, 0.55, indirectConfidence);
	float chromaVisibility = mix(0.92, 1.0, indirectChromaSignal * confidenceLift);
	float energyCompensation = mix(1.0, SURFEL_GI_RESPONSE_FORM_FACTOR_COMPENSATION, confidenceLift);
	vec3 materialResponse = kD * diffuseAlbedo * INV_PI * diffuseTerm * aoPolicy * baseAttenuation * confidenceLift * chromaVisibility * energyCompensation;
	vec3 standardResponse = indirectIrradiance * materialResponse;

	float receiverAlbedoLum = Luminance(diffuseAlbedo);
	float chromaResponseBlend = indirectChromaSignal * confidenceLift * smoothstep(0.006, 0.10, indirectLum);
	vec3 receiverChromaResponseAlbedo = mix(
		diffuseAlbedo,
		max(diffuseAlbedo, vec3(max(receiverAlbedoLum * 0.90, 0.055))),
		chromaResponseBlend * 0.46);
	vec3 receiverChromaResponse = kD * receiverChromaResponseAlbedo * INV_PI * diffuseTerm * aoPolicy * baseAttenuation * confidenceLift * chromaVisibility * energyCompensation;
	vec3 chromaPreservedResponse = indirectIrradiance * receiverChromaResponse;
	vec3 response = mix(standardResponse, chromaPreservedResponse, chromaResponseBlend * 0.46);

	float standardLum = Luminance(max(standardResponse, vec3(0.0)));
	float responseLum = Luminance(max(response, vec3(0.0)));
	float maxChromaPreservedLum = max(
		standardLum * mix(1.06, 1.28, chromaResponseBlend),
		standardLum + 0.024 * chromaResponseBlend);
	if (responseLum > maxChromaPreservedLum) {
		response *= maxChromaPreservedLum / max(responseLum, 1e-5);
	}

	return max(response, vec3(0.0));
}

vec3 PreserveSurfelBleedAgainstDirect(vec3 surfelContribution, vec3 directAndIBLContribution) {
	vec3 safeSurfel = max(surfelContribution, vec3(0.0));
	float surfelLum = Luminance(safeSurfel);
	if (surfelLum <= 1e-5) {
		return vec3(0.0);
	}

	float directLum = Luminance(max(directAndIBLContribution, vec3(0.0)));
	vec3 neutralSurfel = vec3(surfelLum);
	vec3 surfelChroma = safeSurfel - neutralSurfel;
	float chromaRatio = length(surfelChroma) / max(surfelLum, 0.025);
	float chromaSignal = smoothstep(0.08, 0.46, chromaRatio);
	float directWashout = smoothstep(0.20, 1.40, directLum);
	float protection = chromaSignal * directWashout;

	vec3 positiveChroma = max(safeSurfel - neutralSurfel * 0.58, vec3(0.0));
	vec3 protectedSurfel = safeSurfel * (1.0 + 0.18 * protection);
	protectedSurfel += positiveChroma * (0.30 * protection);

	float protectedLum = Luminance(protectedSurfel);
	float maxProtectedLum = max(
		surfelLum * mix(1.04, 1.22, protection),
		surfelLum + 0.018 * protection);
	if (protectedLum > maxProtectedLum) {
		protectedSurfel *= maxProtectedLum / max(protectedLum, 1e-5);
	}

	return max(protectedSurfel, vec3(0.0));
}

vec3 CompressIndirectContribution(vec3 indirectContribution) {
	float luma = Luminance(indirectContribution);
	if (luma <= 1e-5) {
		return vec3(0.0);
	}

	float softKnee = 1.0 / (1.0 + max(luma - 2.2, 0.0) * 0.28);
	return min(indirectContribution * softKnee, vec3(8.0));
}

vec3 EvaluateIndirectDiffuseMap(int sourceIndex, vec3 N, vec3 V, PrincipledSurface surface, float diffuseAO, float specularAO) {
	float strength = indirectDiffuseStrengths[sourceIndex];
	if (strength <= 0.0001) {
		return vec3(0.0);
	}

	vec4 indirectSample = texture(indirectDiffuseMaps[sourceIndex], vTexCoord);
	vec3 indirectIrradiance = max(indirectSample.rgb, vec3(0.0));
	float indirectAO = clamp(indirectSample.a, 0.0, 1.0);
	float indirectAttenuation = sourceIndex == 2 ? 1.0 : mix(0.35, 1.0, indirectAO);
	float sourceScale = 1.0;
	vec3 response = sourceIndex == 2
		? ComputeSurfelGIResponse(indirectIrradiance, indirectAO, N, V, surface, diffuseAO, specularAO)
		: ComputeIndirectGIResponse(indirectIrradiance, N, V, surface, diffuseAO, specularAO);
	vec3 contribution = response * strength * indirectAttenuation * sourceScale;
	return CompressIndirectContribution(contribution);
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
	float ssao = (aoStrength > 0.0001) ? clamp(texture(ssaoMap, uv).r, 0.0, 1.0) : 1.0;

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
		vec3 bounceableIBL = ComputeBounceableIBLSource(N, V, surface, diffuseAO, specularAO);
		FragColor = vec4(max(directDiffuseLighting + bounceableIBL, vec3(0.0)), 1.0);
		return;
	}

	vec3 iblContribution = ComputeIBL(N, V, surface, diffuseAO, specularAO);

	vec3 ssgiContribution = vec3(0.0);
	vec3 lpvContribution = vec3(0.0);
	vec3 surfelContribution = vec3(0.0);
	int indirectCount = clamp(indirectDiffuseSourceCount, 0, MAX_INDIRECT_DIFFUSE_SOURCES);
	for (int i = 0; i < indirectCount; ++i) {
		vec3 sourceContribution = EvaluateIndirectDiffuseMap(i, N, V, surface, diffuseAO, specularAO);
		if (i == 0) {
			ssgiContribution += sourceContribution;
		} else if (i == 1) {
			lpvContribution += sourceContribution;
		} else if (i == 2) {
			surfelContribution += sourceContribution;
		} else {
			lpvContribution += sourceContribution;
		}
	}
	vec3 fullSurfelContribution = PreserveSurfelBleedAgainstDirect(surfelContribution, directLighting + iblContribution);
	vec3 color = (uLightingCompositeDebugMode == 0) ? emissive : vec3(0.0);
	if (uLightingCompositeDebugMode == 1) {
		color += directLighting;
	} else if (uLightingCompositeDebugMode == 2) {
		color += iblContribution;
	} else if (uLightingCompositeDebugMode == 3) {
		color += ssgiContribution;
	} else if (uLightingCompositeDebugMode == 4) {
		color += surfelContribution;
	} else if (uLightingCompositeDebugMode == 5) {
		color += lpvContribution;
	} else {
		color += directLighting;
		color += iblContribution;
		if (indirectDiffuseCompositeMode == 1) {
			vec3 modulativeIndirectContribution = ssgiContribution + lpvContribution + fullSurfelContribution;
			color *= vec3(1.0) + modulativeIndirectContribution;
		}
		else {
			color += ssgiContribution + lpvContribution + fullSurfelContribution;
		}
	}

	color = max(color, vec3(0.0));

	FragColor = vec4(color, 1.0);
}
