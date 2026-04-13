/**
 * @file pbr_common.glsl
 * @brief Shared PBR BRDF functions for deferred and path traced rendering
 * 
 * This file contains the core PBR functions used by both the deferred lighting
 * pass and the ray tracing pass to ensure visual consistency between modes.
 * 
 */

#ifndef PBR_COMMON_GLSL
#define PBR_COMMON_GLSL

//  Constants 
const float PI = 3.14159265359;
const float TAU = 6.283185307179586;
const float INV_PI = 0.31830988618;
const float EPSILON = 1e-4;
const vec3 DIELECTRIC_F0 = vec3(0.04); // Standard dielectric baseline F0

//  Octahedral Normal Encoding/Decoding 
// https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/

// Encode world-space normal to octahedral format [0,1]
vec2 EncodeNormalOct(vec3 n) {
    n = normalize(n);
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    
    if (n.z < 0.0) {
        vec2 signN = sign(n.xy);
        signN = mix(vec2(1.0), signN, step(vec2(0.0001), abs(n.xy)));
        n.xy = (1.0 - abs(n.yx)) * signN;
    }
    
    return n.xy * 0.5 + 0.5;
}

// Decode octahedral normal from [0,1] range (RGBA8 texture)
vec3 DecodeNormalOct(vec2 e) {
    // Remap from [0,1] to [-1,1]
    e = e * 2.0 - 1.0;
    
    vec3 n;
    n.z = 1.0 - abs(e.x) - abs(e.y);
    
    if (n.z < 0.0) {
        vec2 signE = sign(e);
        signE = mix(vec2(1.0), signE, step(vec2(0.0001), abs(e)));
        n.xy = (1.0 - abs(e.yx)) * signE;
    } else {
        n.xy = e.xy;
    }
    
    return normalize(n);
}

//  Fresnel Functions 

// Schlick Fresnel approximation
vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Schlick Fresnel with roughness for IBL
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Calculate F0 from Index of Refraction
float F0FromIOR(float ior) {
    float f = (ior - 1.0) / (ior + 1.0);
    return f * f;
}

// Canonical dielectric F0 for glTF-style specular control
vec3 ComputeDielectricF0(float ior, float specularFactor, vec3 specularColorFactor) {
    float baseF0 = F0FromIOR(ior);
    vec3 dielectricF0 = vec3(baseF0);
    dielectricF0 *= max(specularFactor, 0.0);
    dielectricF0 *= max(specularColorFactor, vec3(0.0));
    return clamp(dielectricF0, vec3(0.0), vec3(1.0));
}

float ClampPerceptualRoughness(float roughness) {
    return clamp(roughness, 0.04, 1.0);
}

vec3 ComputeSurfaceF0(vec3 albedo, float metallic, float ior, float specularFactor, vec3 specularColorFactor) {
    vec3 dielectricF0 = ComputeDielectricF0(ior, specularFactor, specularColorFactor);
    return mix(dielectricF0, albedo, clamp(metallic, 0.0, 1.0));
}

float ComputeTransmissionWeight(float transmission, float NdotV, vec3 F0) {
    float fresnel = dot(FresnelSchlick(clamp(NdotV, 0.0, 1.0), F0), vec3(0.3333333333));
    return clamp(transmission * (1.0 - fresnel), 0.0, 1.0);
}

// Fresnel for dielectrics using IOR
float FresnelDielectric(float cosTheta, float ior) {
    float f0 = F0FromIOR(ior);
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

//  GGX/Trowbridge-Reitz Distribution 

// GGX Normal Distribution Function
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return a2 / max(denom, 1e-6);
}

// Schlick-GGX Geometry Function (single direction)
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-6);
}

// Smith Geometry Function (both directions)
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

//  Ambient Occlusion 

// Calculate diffuse albedo - metals have no diffuse, only specular
vec3 CalculateDiffuseAlbedo(vec3 albedo, float metallic) {
    return albedo * (1.0 - metallic);
}

// Specular occlusion - reduces specular reflections in occluded areas
float SpecularOcclusion(float NdotV, float ao, float roughness) {
    float aoInfluence = mix(0.0, 1.0, roughness * roughness);
    return clamp(pow(NdotV + ao, aoInfluence) - 1.0 + ao, 0.0, 1.0);
}

//  BRDF Evaluation 

// Evaluate Cook-Torrance specular BRDF
// Returns specular contribution (without NdotL multiplication)
vec3 EvaluateSpecularBRDF(vec3 N, vec3 V, vec3 L, vec3 F0, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    
    if (NdotL <= 0.0) return vec3(0.0);
    
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(HdotV, F0);
    
    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    
    return numerator / denominator;
}

// Evaluate Lambertian diffuse BRDF
// Returns diffuse contribution (without NdotL multiplication)
vec3 EvaluateDiffuseBRDF(vec3 albedo, float metallic, vec3 F) {
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    return kD * albedo * INV_PI;
}

// Evaluate full metallic-roughness BRDF with separated diffuse and specular outputs
// This allows proper AO application where diffuse and specular have different occlusion
void EvaluateCanonicalBRDFSeparated(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0, float transmission,
    float clearcoat, float clearcoatRoughness,
    out vec3 diffuseOut, out vec3 specularOut
) {
    diffuseOut = vec3(0.0);
    specularOut = vec3(0.0);
    
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return;
    
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    
    // Specular BRDF
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(HdotV, F0);
    
    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    specularOut = (numerator / denominator) * NdotL;
    
    // Diffuse BRDF with energy conservation
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic) * (1.0 - clamp(transmission, 0.0, 1.0));
    diffuseOut = (kD * albedo * INV_PI) * NdotL;

    float clearcoatWeight = 0.25 * clamp(clearcoat, 0.0, 1.0);
    if (clearcoatWeight > 0.0) {
        vec3 ccF0 = vec3(0.04);
        float ccRoughness = ClampPerceptualRoughness(max(clearcoatRoughness, 0.001));
        vec3 ccF = FresnelSchlick(HdotV, ccF0);
        float ccD = DistributionGGX(N, H, ccRoughness);
        float ccG = GeometrySmith(N, V, L, ccRoughness);
        vec3 clearcoatBRDF = ((ccD * ccG * ccF) / denominator) * NdotL;
        float attenuation = 1.0 - clearcoatWeight * FresnelSchlick(NdotV, ccF0).r;
        diffuseOut *= attenuation;
        specularOut = specularOut * attenuation + clearcoatWeight * clearcoatBRDF;
    }
}

// Evaluate combined canonical BRDF (returns diffuse + specular with NdotL)
vec3 EvaluateCanonicalBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0, float transmission,
    float clearcoat, float clearcoatRoughness
) {
    vec3 diffuse, specular;
    EvaluateCanonicalBRDFSeparated(N, V, L, albedo, metallic, roughness, F0, transmission, clearcoat, clearcoatRoughness, diffuse, specular);
    return diffuse + specular;
}

//  IBL Evaluation 

// Evaluate Image-Based Lighting contribution
vec3 EvaluateCanonicalIBL(
    vec3 N, vec3 V, vec3 R,
    vec3 albedo, float metallic, float roughness, vec3 F0, float transmission,
    float clearcoat, float clearcoatRoughness,
    float diffuseAO, float specularAO,
    samplerCube irradianceMap, samplerCube prefilteredMap, sampler2D brdfLUT,
    float prefilteredMaxLOD,
    float iblIntensity, float diffuseIBLScale, float specularIBLScale
) {
    float NdotV = max(dot(N, V), 0.0);
    
    // Sample IBL textures
    vec3 irradiance = texture(irradianceMap, N).rgb;
    float lod = roughness * prefilteredMaxLOD;
    vec3 prefiltered = textureLod(prefilteredMap, R, lod).rgb;
    vec2 brdf = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    
    // Ensure valid values
    irradiance = max(irradiance, vec3(0.0));
    prefiltered = max(prefiltered, vec3(0.0));
    brdf = max(brdf, vec2(0.0));
    
    // Fresnel for IBL
    vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
    
    // Energy conservation
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic) * (1.0 - clamp(transmission, 0.0, 1.0));
    
    // Diffuse IBL
    vec3 diffuse = kD * albedo * irradiance * diffuseAO * diffuseIBLScale;
    
    // Specular IBL
    vec3 specular = prefiltered * (F * brdf.x + brdf.y) * specularAO * specularIBLScale;
    
    vec3 ibl = (diffuse + specular) * iblIntensity;
    float clearcoatWeight = 0.25 * clamp(clearcoat, 0.0, 1.0);
    if (clearcoatWeight > 0.0) {
        float ccNdotV = max(dot(N, V), 0.0);
        vec3 ccF0 = vec3(0.04);
        vec3 ccF = FresnelSchlickRoughness(ccNdotV, ccF0, ClampPerceptualRoughness(clearcoatRoughness));
        vec3 ccPrefiltered = textureLod(prefilteredMap, R, ClampPerceptualRoughness(clearcoatRoughness) * prefilteredMaxLOD).rgb;
        vec3 ccSpecular = ccPrefiltered * (ccF * brdf.x + brdf.y) * specularAO * specularIBLScale;
        float attenuation = 1.0 - clearcoatWeight * FresnelSchlick(ccNdotV, ccF0).r;
        ibl = ibl * attenuation + clearcoatWeight * ccSpecular * iblIntensity;
    }
    return ibl;
}

// Backwards-compatible wrappers for older shader call sites.
vec3 EvaluateBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0
) {
    return EvaluateCanonicalBRDF(N, V, L, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0);
}

void EvaluateBRDF_Separated(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    out vec3 diffuseOut, out vec3 specularOut
) {
    EvaluateCanonicalBRDFSeparated(N, V, L, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0, diffuseOut, specularOut);
}

vec3 EvaluateMetallicRoughnessBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0
) {
    return EvaluateCanonicalBRDF(N, V, L, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0);
}

void EvaluateMetallicRoughnessBRDFSeparated(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    out vec3 diffuseOut, out vec3 specularOut
) {
    EvaluateCanonicalBRDFSeparated(N, V, L, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0, diffuseOut, specularOut);
}

vec3 EvaluateIBL(
    vec3 N, vec3 V, vec3 R,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    float diffuseAO, float specularAO,
    samplerCube irradianceMap, samplerCube prefilteredMap, sampler2D brdfLUT,
    float prefilteredMaxLOD,
    float iblIntensity, float diffuseIBLScale, float specularIBLScale
) {
    return EvaluateCanonicalIBL(
        N, V, R, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0,
        diffuseAO, specularAO,
        irradianceMap, prefilteredMap, brdfLUT,
        prefilteredMaxLOD, iblIntensity, diffuseIBLScale, specularIBLScale
    );
}

vec3 EvaluateMetallicRoughnessIBL(
    vec3 N, vec3 V, vec3 R,
    vec3 albedo, float metallic, float roughness, vec3 F0,
    float diffuseAO, float specularAO,
    samplerCube irradianceMap, samplerCube prefilteredMap, sampler2D brdfLUT,
    float prefilteredMaxLOD,
    float iblIntensity, float diffuseIBLScale, float specularIBLScale
) {
    return EvaluateCanonicalIBL(
        N, V, R, albedo, metallic, roughness, F0, 0.0, 0.0, 0.0,
        diffuseAO, specularAO,
        irradianceMap, prefilteredMap, brdfLUT,
        prefilteredMaxLOD, iblIntensity, diffuseIBLScale, specularIBLScale
    );
}

//  Sampling Utilities (for path tracing) 

// GGX importance sampling - generates a reflection direction for given roughness
// NOTE: This samples the GGX distribution in the half-vector space
vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, vec3 V, float roughness) {
    float alpha = roughness * roughness;
    
    // Sample the GGX distribution
    float phi = TAU * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (alpha * alpha - 1.0) * Xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    
    // Half vector in tangent space
    vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    
    // Build tangent space basis
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    
    // Transform H to world space
    H = normalize(tangent * H.x + bitangent * H.y + N * H.z);
    
    // Reflect view direction around half vector
    return reflect(-V, H);
}

// PDF for GGX importance sampling
float GGX_PDF(vec3 N, vec3 H, vec3 V, float roughness) {
    float alpha = roughness * roughness;
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    
    float D = DistributionGGX(N, H, roughness);
    
    // PDF = D * NdotH / (4 * HdotV)
    return D * NdotH / (4.0 * HdotV + 0.0001);
}

// Cosine-weighted hemisphere sampling PDF
float CosinePDF(float NdotL) {
    return NdotL * INV_PI;
}

#endif // PBR_COMMON_GLSL
