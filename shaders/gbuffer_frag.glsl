#version 460 core

in vec3 WorldPos;
in vec3 WorldNormal;
in vec2 TexCoords;
in mat3 TBN;

// RT0: RGBA8  - Oct-encoded normal (RG) + Roughness (B) + Metallic (A)
// RT1: RGBA16F - Albedo (RGB) + Occlusion (A)
// RT2: RGBA16F - Emissive (RGB) + Specular F0 luminance (A)
layout(location = 0) out vec4 gPackedNormalRM;
layout(location = 1) out vec4 gAlbedoAO;
layout(location = 2) out vec4 gEmissiveSpec;

uniform sampler2D texture_diffuse;
uniform sampler2D texture_normal;
uniform sampler2D texture_metallic_roughness; // G=roughness B=metallic
uniform sampler2D texture_emissive;
uniform sampler2D texture_occlusion;// R channel
uniform sampler2D texture_specular;// Extension (specular F0 color/factor)

// glTF 2.0 core factors
uniform float metallicFactor  = 0.0;  // default non-metallic
uniform float roughnessFactor = 1.0;  // default fully rough
uniform vec4  baseColorFactor = vec4(1.0);
uniform vec3  emissiveFactor  = vec3(0.0);
uniform float occlusionStrength = 1.0; // multiplier

// KHR_materials_specular extension factors (fallbacks)
uniform float specularFactor = 1.0;       // overall F0 strength multiplier
uniform vec3  specularColorFactor = vec3(1.0); // color tint to F0

// Presence flags
uniform bool hasBaseColorTexture = false;
uniform bool hasNormalTexture = false;
uniform bool hasMetallicRoughnessTexture = false;
uniform bool hasEmissiveTexture = false;
uniform bool hasOcclusionTexture = false;
uniform bool hasSpecularTexture = false;

vec2 EncodeNormalOct8(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign(n.xy);
    return n.xy * 0.5 + 0.5;
}

void main() {
    //Re-orthonormalize TBN per-pixel after interpolation
    vec3 N = normalize(WorldNormal);
    
    // Reconstruct T and B from interpolated TBN
    vec3 T = normalize(TBN[0]);
    vec3 B = normalize(TBN[1]);
    
    // Gram-Schmidt orthogonalization: ensure T is perpendicular to N
    T = normalize(T - dot(T, N) * N);
    
    // Recompute B to ensure right-handed coordinate system
    B = cross(N, T);
    
    // Rebuild orthonormal TBN matrix
    mat3 orthonormalTBN = mat3(T, B, N);
    
    // --- Normal mapping ---
    if (hasNormalTexture) {
        // Sample normal map in tangent space (range [0,1])
        vec3 tangentNormal = texture(texture_normal, TexCoords).rgb;
        
        // Convert from [0,1] to [-1,1] range
        tangentNormal = tangentNormal * 2.0 - 1.0;
    
        // Transform from tangent space to world space using orthonormal TBN
        vec3 mapped = normalize(orthonormalTBN * tangentNormal);
      
        // Safety check for valid normals
        if (length(mapped) > 0.1 && all(greaterThanEqual(mapped, vec3(-10.0)))) {
            N = mapped;
        }
    }

    // --- Metallic / Roughness ---
    float metallic = metallicFactor;
    float roughness = roughnessFactor;
    if (hasMetallicRoughnessTexture) {
        vec4 mrSample = texture(texture_metallic_roughness, TexCoords);
        // glTF: G = roughness, B = metallic
        roughness = clamp(mrSample.g * roughnessFactor, 0.0, 1.0);
        metallic  = clamp(mrSample.b * metallicFactor, 0.0, 1.0);
  }
    // Minimum roughness for dielectrics to prevent numerical issues
    if (metallic < 0.1) {
        roughness = max(roughness, 0.2);
    }
    roughness = clamp(roughness, 0.04, 1.0);

    // --- Albedo ---
    vec3 albedo = baseColorFactor.rgb;
    if (hasBaseColorTexture) {
        albedo *= texture(texture_diffuse, TexCoords).rgb;
    }
    if (length(albedo) < 0.01) albedo = vec3(0.7);

    // --- Emissive ---
    vec3 emissive = emissiveFactor;
    if (hasEmissiveTexture) {
  vec3 emissiveTexSample = texture(texture_emissive, TexCoords).rgb;
      emissive *= emissiveTexSample;
    }

    // --- Occlusion ---
    float ao = 1.0;
    if (hasOcclusionTexture) {
        ao = mix(1.0, texture(texture_occlusion, TexCoords).r, occlusionStrength);
    }

    // --- Specular F0 ---
    vec3 dielectricF0 = vec3(0.04) * specularFactor * specularColorFactor;
    if (hasSpecularTexture) {
        vec3 specSample = texture(texture_specular, TexCoords).rgb;
        dielectricF0 *= specSample;
 }
    dielectricF0 = clamp(dielectricF0, vec3(0.0), vec3(1.0));
    vec3 F0 = mix(dielectricF0, albedo, metallic);
    
    // Convert F0 to luminance for packing (we'll reconstruct full F0 in lighting pass)
    float specLuminance = dot(F0, vec3(0.299, 0.587, 0.114));

    // RT0: Oct normal (RG) + roughness (B) + metallic (A)
    gPackedNormalRM = vec4(EncodeNormalOct8(N), roughness, metallic);
    
    // RT1: Albedo (RGB) + occlusion (A)
    gAlbedoAO = vec4(albedo, ao);
    
    // RT2: Emissive (RGB) + specular luminance (A)
    gEmissiveSpec = vec4(emissive, specLuminance);
}
