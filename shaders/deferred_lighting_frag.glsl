#version 460 core

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
uniform float normalOffsetScale = 0.1;
uniform float cascadeBiasScale = 1.0;

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

const float PI = 3.14159265359;
const vec3 DIELECTRIC_F0 = vec3(0.04); // Standard dielectric baseline F0
const float INV_PI = 0.31830988618; // 1/PI

// Octahedral normal decoding - input is [0,1] from RGBA8 texture
vec3 DecodeNormalOct8(vec2 e) {
    // Remap from [0,1] to [-1,1]
    e = e * 2.0 - 1.0;
    
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    
    if (n.z < 0.0) {
        // Handle lower hemisphere fold
        vec2 signE = sign(e);
        signE = mix(vec2(1.0), signE, step(vec2(0.0001), abs(e)));
        n.xy = (1.0 - abs(e.yx)) * signE;
    } else {
        n.xy = e.xy;
    }
    
    return normalize(n);
}

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

// Calculate diffuse albedo - metals have no diffuse, only specular
vec3 CalculateDiffuseAlbedo(vec3 albedo, float metallic) {
	return albedo * (1.0 - metallic);
}

float SpecularOcclusion(float NdotV, float ao, float roughness) {
	float aoInfluence = mix(0.0, 1.0, roughness * roughness);
	return clamp(pow(NdotV + ao, aoInfluence) - 1.0 + ao, 0.0, 1.0);
}

// Microfacet BRDF functions
float DistributionGGX(vec3 N, vec3 H, float roughness) {
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0);
	float NdotH2 = NdotH * NdotH;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = PI * denom * denom;
	return a2 / max(denom, 1e-6);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
	float r = roughness + 1.0;
	float k = (r * r) / 8.0;
	return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
	float NdotV = max(dot(N, V), 0.0);
	float NdotL = max(dot(N, L), 0.0);
	return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
	return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

bool inUnitCube(vec3 p) {
	return all(greaterThanEqual(p, vec3(0.0))) && all(lessThanEqual(p, vec3(1.0)));
}

float CalculateAdaptiveShadowBias(vec3 N, vec3 Ld, int cascadeIndex, float depthComp, float distance) {
	float NdotL = max(dot(N, -Ld), 0.0);
	float slopeFactor = sqrt(max(1.0 - NdotL * NdotL, 0.0)) / max(NdotL, 0.01);
	float baseBias = shadowBias;
	if (NdotL < 0.5) {
		baseBias *= (1.0 + slopeFactor * 1.0);
	}
	float cascadeScale = 1.0 + float(cascadeIndex) * 0.15;
	float depthBias = baseBias * (1.0 + distance * 0.0001);
	float finalBias = baseBias * cascadeScale + depthBias;
	return clamp(finalBias, shadowBias * 0.5, shadowBias * 5.0);
}

float SampleShadowArray(int layer, vec3 projCoords, float bias) {
	if (!inUnitCube(projCoords)) return 1.0;
	
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
			sum += texture(multiLightShadowArray, vec4(uv, float(layer), projCoords.z - bias));
		}
	}
	
	return sum / float(count);
}

float ComputeCascadedShadow(int startSlice, int sliceCount, vec3 worldPos, vec3 N, vec3 lightDir) {
	vec3 viewSpacePos = (view * vec4(worldPos, 1.0)).xyz;
	float viewDepth = -viewSpacePos.z;
	vec3 shadowPos = worldPos;
	
	int selectedCascade = -1;
	float cascadeBlend = 0.0;
	vec3 projCoords[2];
	float shadowSamples[2];
	int cascadeIndices[2] = int[2](-1, -1);
	
	for (int i = 0; i < sliceCount; ++i) {
		int layer = startSlice + i;
		mat4 M = shadowMatrices[layer];
		vec4 lsp = M * vec4(shadowPos, 1.0);
		lsp.xyz /= lsp.w;
		vec3 pc = lsp.xyz * 0.5 + 0.5;
		
		if (inUnitCube(pc)) {
			if (selectedCascade == -1) {
				selectedCascade = i;
				cascadeIndices[0] = i;
				projCoords[0] = pc;
				
				vec2 edgeDist = min(pc.xy, 1.0 - pc.xy);
				float minEdgeDist = min(edgeDist.x, edgeDist.y);
				
				const float blendRegion = 0.15;
				if (minEdgeDist < blendRegion && i < sliceCount - 1) {
					cascadeBlend = smoothstep(0.0, blendRegion, blendRegion - minEdgeDist);
					
					int nextLayer = startSlice + i + 1;
					mat4 nextM = shadowMatrices[nextLayer];
					vec4 nextLsp = nextM * vec4(shadowPos, 1.0);
					nextLsp.xyz /= nextLsp.w;
					projCoords[1] = nextLsp.xyz * 0.5 + 0.5;
					cascadeIndices[1] = i + 1;
				}
			}
			break;
		}
	}
	
	if (selectedCascade == -1) return 1.0;
	
	float bias0 = CalculateAdaptiveShadowBias(N, lightDir, cascadeIndices[0], projCoords[0].z, viewDepth);
	shadowSamples[0] = SampleShadowArray(startSlice + cascadeIndices[0], projCoords[0], bias0);
	
	if (cascadeBlend > 0.0 && cascadeIndices[1] >= 0) {
		float bias1 = CalculateAdaptiveShadowBias(N, lightDir, cascadeIndices[1], projCoords[1].z, viewDepth);
		shadowSamples[1] = SampleShadowArray(startSlice + cascadeIndices[1], projCoords[1], bias1);
		
		float t = cascadeBlend * cascadeBlend * (3.0 - 2.0 * cascadeBlend);
		return mix(shadowSamples[0], shadowSamples[1], t);
	}
	
	return shadowSamples[0];
}

float ComputePointLightShadow(int startSlice, vec3 worldPos, vec3 N, vec3 lightPos) {
	vec3 toLight = worldPos - lightPos;
	float distance = length(toLight);
	vec3 lightDir = toLight / distance;
	
	vec3 shadowPos = worldPos;
	vec3 offsetToLight = shadowPos - lightPos;
	
	vec3 absDir = abs(offsetToLight);
	int face = 0;
	
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
	lsp.xyz /= lsp.w;
	vec3 pc = lsp.xyz * 0.5 + 0.5;
	
	if (!inUnitCube(pc)) return 1.0;
	
	float bias = CalculateAdaptiveShadowBias(N, -lightDir, 0, pc.z, distance);
	float shadow = SampleShadowArray(layer, pc, bias);
	
	vec2 edgeDist = min(pc.xy, 1.0 - pc.xy);
	float minEdgeDist = min(edgeDist.x, edgeDist.y);
	const float seamBlendRegion = 0.05;
	
	if (minEdgeDist < seamBlendRegion) {
		float seamFade = minEdgeDist / seamBlendRegion;
		shadow = mix(shadow * 0.95, shadow, seamFade);
	}
	
	return shadow;
}

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
	vec3 F = fresnelSchlick(HdotV, F0);
	
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
	shadowMapShadow = mix(0.3, 1.0, shadowMapShadow);
	
	float shadowEnableMask = float(enableShadows == 1) * float(Ld.shadowData.z > 0.5);
	shadowMapShadow = mix(1.0, shadowMapShadow, shadowEnableMask);

	// Contact shadows
	float contactShadowVisibility = texture(screenSpaceShadowMap, vTexCoord).r;
	float viewDepth = length(worldPos - viewPos);
   
	float contactStrength = smoothstep(15.0, 1.0, viewDepth);
	contactStrength = mix(0.6, 1.0, contactStrength);
	
	float litAreaReduction = smoothstep(0.9, 1.0, shadowMapShadow);
	contactStrength *= (1.0 - litAreaReduction * 0.5);
	
	float contactShadow = mix(1.0, contactShadowVisibility, contactStrength * sssStrength);
	float combinedShadow = shadowMapShadow * contactShadow;
	combinedShadow = max(combinedShadow, 0.1);
	
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

	vec3 F = fresnelSchlick(NdotV, F0);
	
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
	vec3 decodedNormal = DecodeNormalOct8(encNormal);
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
		// TODO: Implement refraction and transmission in future update
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

	FragColor = vec4(color, 1.0);
}
