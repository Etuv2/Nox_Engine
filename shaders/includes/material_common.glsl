/**
 * @file material_common.glsl
 * @brief Shared material structures and G-buffer unpacking for deferred and RT rendering
 * 
 * This file contains material structures and G-buffer unpacking functions used by
 * both the deferred lighting pass and the ray tracing pass.
 * 
 * Include this file after pbr_common.glsl
 */

#ifndef MATERIAL_COMMON_GLSL
#define MATERIAL_COMMON_GLSL

// === Material Structure ===
// This structure holds all PBR material properties needed for lighting calculations

struct PBRMaterial {
    vec3 albedo;              // Base color (RGB)
    float metallic;           // Metallic factor [0,1]
    float roughness;          // Roughness factor [0.04,1]
    vec3 specularF0;          // Specular F0 (full RGB color)
    vec3 emissive;            // Emissive color (already scaled by strength)
    float ao;                 // Ambient occlusion from texture [0,1]
    uint materialID;          // Material type (0=PBR, 1=SpecGloss, 2=Transmissive)
    
    // Extended properties for special materials
    float transmission;       // Transmission factor for glass [0,1]
    float ior;                // Index of refraction (default 1.5)
};

// === G-Buffer Layout Reference ===
// RT0: RGBA8   - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
// RT3: R8UI    - Material ID (0=Standard PBR, 1=SpecGloss, 2=Transmission)
// RT4: RGBA16F - Emissive color (RGB) + unused (A)
// Depth buffer - Non-linear depth [0,1]

// === G-Buffer Unpacking Functions ===

// Unpack material from G-buffer textures
// samplers: gPackedNormalRM, gAlbedoAO, gSpecularF0, gMaterialID, gEmissive
PBRMaterial UnpackGBufferMaterial(
    sampler2D gPackedNormalRM,
    sampler2D gAlbedoAO,
    sampler2D gSpecularF0,
    usampler2D gMaterialID,
    sampler2D gEmissive,
    vec2 uv,
    out vec3 worldNormal
) {
    PBRMaterial mat;
    
    // RT0: Normal + Roughness + Metallic
    vec4 packedNRM = texture(gPackedNormalRM, uv);
    vec2 encNormal = packedNRM.rg;
    mat.roughness = clamp(packedNRM.b, 0.04, 1.0);  // Minimum roughness for stability
    mat.metallic = clamp(packedNRM.a, 0.0, 1.0);
    
    // Decode normal
    worldNormal = DecodeNormalOct(encNormal);
    
    // Validate normal
    if (length(worldNormal) < 0.5 || any(isnan(worldNormal))) {
        worldNormal = vec3(0.0, 1.0, 0.0);
    }
    worldNormal = normalize(worldNormal);
    
    // RT1: Albedo + Occlusion
    vec4 albedoAO = texture(gAlbedoAO, uv);
    mat.albedo = albedoAO.rgb;
    mat.ao = clamp(albedoAO.a, 0.0, 1.0);
    
    // Validate albedo
    if (any(isnan(mat.albedo)) || any(isinf(mat.albedo))) {
        mat.albedo = vec3(0.5);
    }
    
    // RT2: Specular F0 + Emissive strength
    vec4 specF0Data = texture(gSpecularF0, uv);
    mat.specularF0 = specF0Data.rgb;
    float emissiveStrength = specF0Data.a;
    
    // RT3: Material ID
    mat.materialID = texture(gMaterialID, uv).r;
    
    // RT4: Emissive color
    vec4 emissiveData = texture(gEmissive, uv);
    mat.emissive = emissiveData.rgb * emissiveStrength;
    
    // Set defaults for extended properties
    mat.transmission = (mat.materialID == 2u) ? 0.9 : 0.0;
    mat.ior = 1.5;
    
    // Material-specific adjustments
    if (mat.materialID == 2u) {
        // Transmissive materials should be smooth
        mat.roughness = min(mat.roughness, 0.1);
    }
    
    return mat;
}

// === World Position Reconstruction ===

vec3 ReconstructWorldPosition(vec2 uv, float depth, mat4 invProj, mat4 invView) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = invProj * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos = invView * viewPos;
    return worldPos.xyz;
}

// === AO Computation ===

// Compute diffuse and specular AO factors matching deferred lighting
void ComputeAOFactors(
    float textureAO,
    float ssao,
    float aoStrength,
    float NdotV,
    float roughness,
    out float diffuseAO,
    out float specularAO
) {
    // Combine texture AO with SSAO
    diffuseAO = mix(1.0, ssao, aoStrength) * textureAO;
    
    // Specular occlusion for correct specular darkening in corners
    specularAO = SpecularOcclusion(NdotV, diffuseAO, roughness);
}

// Simplified version when no SSAO is available
void ComputeAOFactorsSimple(
    float textureAO,
    float NdotV,
    float roughness,
    out float diffuseAO,
    out float specularAO
) {
    diffuseAO = textureAO;
    specularAO = SpecularOcclusion(NdotV, diffuseAO, roughness);
}

// === Direct Lighting Computation ===

// Compute direct lighting contribution from a single light with proper AO
vec3 ComputeDirectLightContribution(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    vec3 lightColor, float attenuation,
    float diffuseAO,
    float shadow
) {
    vec3 diffuse, specular;
    EvaluateBRDF_Separated(N, V, L, albedo, metallic, roughness, F0, diffuse, specular);
    
    // Apply AO to diffuse component only (specular should use specularAO in IBL)
    // For direct lighting, diffuseAO affects diffuse, specular is unaffected
    vec3 radiance = lightColor * attenuation;
    return (diffuse * diffuseAO + specular) * radiance * shadow;
}

#endif // MATERIAL_COMMON_GLSL
