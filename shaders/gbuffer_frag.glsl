#version 460 core

in vec3 WorldPos;
in vec3 WorldNormal;
in vec2 TexCoords;
in mat3 TBN;
in vec4 RawTangent;  // CRITICAL: Receive tangent with handedness (w component)

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

// Improved octahedral normal encoding with proper sign handling
vec2 EncodeNormalOct8(vec3 n) {
    // Normalize to ensure unit length
    n = normalize(n);
    
    // Project onto octahedron
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    
    // Handle the lower hemisphere (z < 0)
    if (n.z < 0.0) {
        vec2 signN = sign(n.xy);
        // Ensure sign is never zero to avoid discontinuities
        signN = mix(vec2(1.0), signN, step(vec2(0.0001), abs(n.xy)));
        n.xy = (1.0 - abs(n.yx)) * signN;
    }
    
    // Map from [-1, 1] to [0, 1]
    return n.xy * 0.5 + 0.5;
}

// Calculate F0 from IOR (Schlick approximation)
float F0FromIOR(float ior) {
    float f = (ior - 1.0) / (ior + 1.0);
    return f * f;
}

void main() {
    // Re-orthonormalize TBN per-pixel after interpolation
    vec3 N = normalize(WorldNormal);
    
    // Reconstruct T from interpolated TBN
    vec3 T = normalize(TBN[0]);
    
    // Gram-Schmidt orthogonalization: ensure T is perpendicular to N
    T = normalize(T - dot(T, N) * N);
    
    // CRITICAL FIX: Recompute B using handedness from RawTangent.w
    // This preserves the correct orientation across UV seams
    float handedness = RawTangent.w;
    vec3 B = normalize(cross(N, T) * handedness);
    
    // Rebuild orthonormal TBN matrix
    mat3 orthonormalTBN = mat3(T, B, N);
    
    // --- Normal mapping ---
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

    // --- Material workflow selection ---
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
        
        // --- Albedo ---
        albedo = baseColorFactor.rgb;
        if (hasBaseColorTexture) {
            albedo *= texture(texture_diffuse, TexCoords).rgb;
        }
        
        // --- Specular F0 calculation with KHR_materials_specular support ---
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

    // Convert F0 to luminance for packing (we'll reconstruct full F0 in lighting pass)
    float specLuminance = dot(specularF0, vec3(0.299, 0.587, 0.114));

    // RT0: Oct normal (RG) + roughness (B) + metallic (A)
    gPackedNormalRM = vec4(EncodeNormalOct8(N), roughness, metallic);
    
    // RT1: Albedo (RGB) + occlusion (A)
    gAlbedoAO = vec4(albedo, ao);
    
    // RT2: Emissive (RGB) + specular luminance (A)
    gEmissiveSpec = vec4(emissive, specLuminance);
}
