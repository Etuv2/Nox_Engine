#version 460 core
#include "includes/pbr_common.glsl" // For normal encoding/decoding
in vec3 WorldPos;
in vec3 WorldNormal;
in vec2 TexCoords;
in mat3 TBN;
in vec4 RawTangent;  // Receive tangent with handedness (w component)
flat in uint TransformID;

// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
// RT2: RGBA16F - Specular F0 (RGB) + Emissive strength (A)
// RT3: R8UI - Material ID (0=Standard PBR, 1=SpecGloss, 2=Transmission, etc.)
// RT4: RGBA16F - Emissive color (RGB) + unused (A)
// RT5: R32UI - Stable TransformID for temporal/surfel/GPU tracking
layout(location = 0) out vec4 gPackedNormalRM;
layout(location = 1) out vec4 gAlbedoAO;
layout(location = 2) out vec4 gSpecularF0;
layout(location = 3) out uint gMaterialID;
layout(location = 4) out vec4 gEmissive;
layout(location = 5) out uint gTransformID;

uniform sampler2D texture_diffuse;
uniform sampler2D texture_normal;
uniform sampler2D texture_metallic_roughness; // G=roughness B=metallic
uniform sampler2D texture_emissive;
uniform sampler2D texture_occlusion;          // R channel
uniform sampler2D texture_specular;           // Specular factor texture (A channel typically)
uniform sampler2D texture_specular_color;     // Specular color texture (RGB)
uniform sampler2D texture_transmission;       // Transmission texture (R channel)

// glTF 2.0 core factors
uniform float metallicFactor  = 0.0;  // default non-metallic
uniform float roughnessFactor = 1.0;  // default fully rough
uniform vec4  baseColorFactor = vec4(1.0);
uniform vec3  emissiveFactor  = vec3(0.0);
uniform float occlusionStrength = 1.0; // multiplier
uniform float normalScale = 1.0;       // Normal map intensity

// KHR_materials_specular extension factors
uniform float specularFactor = 1.0;           // overall F0 strength multiplier
uniform vec3  specularColorFactor = vec3(1.0); // color tint to F0

// KHR_materials_transmission extension
uniform float transmissionFactor = 0.0;       // transmission factor [0,1]

// KHR_materials_ior extension
uniform float ior = 1.5;                      // index of refraction

// KHR_materials_pbrSpecularGlossiness support
uniform bool useSpecularGlossinessWorkflow = false;
uniform vec3 diffuseFactor = vec3(1.0);
uniform vec3 specularGlossinessFactor = vec3(1.0);
uniform float glossinessFactor = 1.0;

// Presence flags
uniform bool hasBaseColorTexture = false;
uniform bool hasNormalTexture = false;
uniform bool hasMetallicRoughnessTexture = false;
uniform bool hasEmissiveTexture = false;
uniform bool hasOcclusionTexture = false;
uniform bool hasSpecularTexture = false;
uniform bool hasSpecularColorTexture = false;
uniform bool hasTransmissionTexture = false;


void main() {
    // Re-orthonormalize TBN per-pixel after interpolation
    vec3 N = normalize(WorldNormal);
    
    // Reconstruct T from interpolated TBN
    vec3 T = normalize(TBN[0]);
    
    // Gram-Schmidt orthogonalization: ensure T is perpendicular to N
    T = normalize(T - dot(T, N) * N);
    
    // Recompute B using handedness from RawTangent.w
    // This preserves the correct orientation across UV seams
    float handedness = RawTangent.w;
    vec3 B = normalize(cross(N, T) * handedness);
    
    // Rebuild orthonormal TBN matrix
    mat3 orthonormalTBN = mat3(T, B, N);
    
    //  Normal mapping 
    if (hasNormalTexture) {
        // Sample normal map in tangent space (range [0,1])
        vec3 tangentNormal = texture(texture_normal, TexCoords).rgb;
        
        // Convert from [0,1] to [-1,1] range
        tangentNormal = tangentNormal * 2.0 - 1.0;
        
        // Apply normal scale for intensity control
        tangentNormal.xy *= normalScale;
        tangentNormal = normalize(tangentNormal);
    
        // Transform from tangent space to world space using orthonormal TBN
        vec3 mapped = normalize(orthonormalTBN * tangentNormal);
      
        // Safety check for valid normals
        if (length(mapped) > 0.1 && !any(isnan(mapped)) && !any(isinf(mapped))) {
            N = mapped;
        }
    }

    //  Material workflow selection 
    vec3 albedo;
    float metallic;
    float roughness;
    vec3 specularF0;

    if (useSpecularGlossinessWorkflow) {
        // KHR_materials_pbrSpecularGlossiness workflow
        albedo = diffuseFactor;
        if (hasBaseColorTexture) {
            albedo *= texture(texture_diffuse, TexCoords).rgb;
        }
        
        // In specular-glossiness, specular IS the F0
        specularF0 = specularGlossinessFactor;
        if (hasSpecularTexture) {
            vec4 sgSample = texture(texture_specular, TexCoords);
            specularF0 *= sgSample.rgb;
            // A channel contains glossiness in specular-glossiness workflow
            roughness = 1.0 - (sgSample.a * glossinessFactor);
        } else {
            roughness = 1.0 - glossinessFactor;
        }
        
        // Metallic is derived from specular in this workflow
        // High specular luminance = more metallic-like behavior
        metallic = clamp(dot(specularF0, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        
    } else {
        // Standard metallic-roughness workflow
        metallic = metallicFactor;
        roughness = roughnessFactor;
        
        if (hasMetallicRoughnessTexture) {
            vec4 mrSample = texture(texture_metallic_roughness, TexCoords);
            // glTF: G = roughness, B = metallic
            roughness = clamp(mrSample.g * roughnessFactor, 0.0, 1.0);
            metallic  = clamp(mrSample.b * metallicFactor, 0.0, 1.0);
        }
        
        //  Albedo 
        albedo = baseColorFactor.rgb;
        if (hasBaseColorTexture) {
            albedo *= texture(texture_diffuse, TexCoords).rgb;
        }
        
        //  Specular F0 calculation with KHR_materials_specular support 
        // Base dielectric F0 from IOR (default ~0.04 for IOR 1.5)
        float baseF0 = F0FromIOR(ior);
        vec3 dielectricF0 = vec3(baseF0);
        
        // Apply specular factor and color from extension
        float specFactorSample = specularFactor;
        if (hasSpecularTexture) {
            specFactorSample *= texture(texture_specular, TexCoords).a; // A channel for scalar factor
        }
        
        vec3 specColorSample = specularColorFactor;
        if (hasSpecularColorTexture) {
            specColorSample *= texture(texture_specular_color, TexCoords).rgb;
        }
        
        // Final dielectric F0 = base F0 * specular factor * specular color
        dielectricF0 = dielectricF0 * specFactorSample * specColorSample;
        dielectricF0 = clamp(dielectricF0, vec3(0.0), vec3(1.0));
        
        // Metals use albedo as F0, dielectrics use computed F0
        specularF0 = mix(dielectricF0, albedo, metallic);
    }
    
    // Minimum roughness for dielectrics to prevent numerical issues
    if (metallic < 0.1) {
        roughness = max(roughness, 0.2);
    }
    roughness = clamp(roughness, 0.04, 1.0);
    
    //  Emissive 
    vec3 emissive = emissiveFactor;
    if (hasEmissiveTexture) {
        vec3 emissiveTexSample = texture(texture_emissive, TexCoords).rgb;
        emissive *= emissiveTexSample;
    }

    //  Occlusion 
    float ao = 1.0;
    if (hasOcclusionTexture) {
        ao = mix(1.0, texture(texture_occlusion, TexCoords).r, occlusionStrength);
    }

    // Calculate emissive strength (luminance for alpha channel)
    float emissiveStrength = dot(emissive, vec3(0.299, 0.587, 0.114));

    //  Determine Material ID 
    uint materialID = 0u; // Default: Standard PBR
    
    if (useSpecularGlossinessWorkflow) {
        materialID = 1u; // Specular-Glossiness workflow
    } else if (transmissionFactor > 0.01) {
        materialID = 2u; // Transmissive/Glass material
    }
    // Future material IDs:
    // 3u = Subsurface scattering
    // 4u = Cloth/fabric
    // 5u = Clearcoat
    // 6-255u = Custom materials

    // RT0: Oct normal (RG) + roughness (B) + metallic (A)
    gPackedNormalRM = vec4(EncodeNormalOct(N), roughness, metallic);
    
    // RT1: Albedo (RGB) + occlusion (A)
    gAlbedoAO = vec4(albedo, ao);
    
    // RT2: Specular F0 (full RGB color) + emissive strength (A)
    gSpecularF0 = vec4(specularF0, emissiveStrength);
    
    // RT3: Material ID
    gMaterialID = materialID;
    
    // RT4: Emissive color (RGB)
    gEmissive = vec4(emissive, 0.0);

    // RT5: Stable transform/surface identity
    gTransformID = TransformID;
}
