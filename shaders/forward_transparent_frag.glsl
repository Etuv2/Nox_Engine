#version 460 core

// Include shared PBR functions
#include "includes/pbr_common.glsl"

out vec4 FragColor;

in VS_OUT {
    vec3 WorldPos;
    vec3 Normal;
    vec2 UV;
    vec4 TangentWS; // Tangent in world space (w = handedness)
} fs_in;

//G-BUFFER LAYOUT (for depth reads only - transparent pass reads scene depth)
uniform sampler2D gDepth;

//Screen-space uniforms for depth comparison
uniform vec2 screenSize;  // Screen resolution for screen UV calculation

// Material properties (glTF 2.0 core)
uniform vec4 baseColorFactor = vec4(1.0);
uniform float metallicFactor = 0.0;
uniform float roughnessFactor = 1.0;
uniform vec3 emissiveFactor = vec3(0.0);
uniform float emissiveStrength = 1.0;
uniform float occlusionStrength = 1.0;
uniform float normalScale = 1.0;
uniform float alphaCutoff = 0.5;

// KHR_materials_specular extension
uniform float specularFactor = 1.0;
uniform vec3 specularColorFactor = vec3(1.0);
uniform float clearcoatFactor = 0.0;
uniform float clearcoatRoughnessFactor = 0.0;

// KHR_materials_transmission extension (from material, not hardcoded)
uniform float transmissionFactor = 0.0;    // Material transmission [0,1]
uniform sampler2D texture_transmission;    // Transmission texture
uniform float thicknessFactor = 0.0;
uniform float attenuationDistance = 0.0;
uniform vec3 attenuationColor = vec3(1.0);

// KHR_materials_ior extension (from material, not hardcoded)
uniform float ior = 1.5;                   // Index of refraction from material

// Material textures
uniform sampler2D texture_diffuse;
uniform sampler2D texture_normal;
uniform sampler2D texture_metallic_roughness;
uniform sampler2D texture_emissive;
uniform sampler2D texture_occlusion;
uniform sampler2D texture_specular;
uniform sampler2D texture_specular_color;

// Texture presence flags
uniform bool hasBaseColorTexture = false;
uniform bool hasNormalTexture = false;
uniform bool hasMetallicRoughnessTexture = false;
uniform bool hasEmissiveTexture = false;
uniform bool hasOcclusionTexture = false;
uniform bool hasSpecularTexture = false;
uniform bool hasSpecularColorTexture = false;
uniform bool hasTransmissionTexture = false;

// IBL textures
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform float prefilteredMaxLOD = 4.0;
uniform float iblIntensity = 0.4;
uniform float diffuseIBLScale = 0.5;
uniform float specularIBLScale = 0.6;

// Camera
uniform vec3 viewPos;

// Enhanced lighting uniforms
uniform vec3 keyLightDir = normalize(vec3(-0.4, -1.0, -0.2));
uniform vec3 keyLightColor = vec3(1.0);
uniform float keyLightIntensity = 1.0;

vec3 getNormalFromMap() {
    // Start with geometric normal
    vec3 N = normalize(fs_in.Normal);
    
    // If no normal texture, return geometric normal
    if (!hasNormalTexture) {
        return N;
    }
    
    // Sample normal map in tangent space
    vec3 tangentNormal = texture(texture_normal, fs_in.UV).xyz * 2.0 - 1.0;
    
    // Apply normal scale for intensity control, then renormalize
    tangentNormal.xy *= normalScale;
    tangentNormal = normalize(tangentNormal);
    
    // Get tangent from vertex shader (already orthogonalized)
    vec3 T = normalize(fs_in.TangentWS.xyz);
    
    // Re-orthogonalize tangent per-pixel after interpolation
    // This prevents seams at triangle boundaries
    T = normalize(T - dot(T, N) * N);
    
    // Compute bitangent using handedness from tangent.w
    // The handedness preserves correct orientation across UV seams
    float handedness = fs_in.TangentWS.w;
    vec3 B = normalize(cross(N, T) * handedness);
    
    // Build TBN matrix
    mat3 TBN = mat3(T, B, N);
    
    // Transform normal from tangent space to world space
    vec3 mappedNormal = normalize(TBN * tangentNormal);
    
    // Safety check: if the normal is invalid, fall back to geometric normal
    if (length(mappedNormal) < 0.5 || any(isnan(mappedNormal)) || any(isinf(mappedNormal))) {
        return N;
    }
    
    return mappedNormal;
}

// Note: DistributionGGX, GeometrySchlickGGX, GeometrySmith, FresnelSchlick, 
// FresnelSchlickRoughness are now defined in pbr_common.glsl (included above)

// Simplified but physically accurate refraction calculation
vec3 calculateRefraction(vec3 I, vec3 N, float materialIOR) {
    // I is incident direction (view direction), N is surface normal
    float cosi = dot(I, N);
    float etai = 1.0; // Air
    float etat = materialIOR; // Material IOR
    vec3 n = N;
    
    // Check if we're entering or exiting the material
    if (cosi < 0.0) {
        // Entering from air into material
        cosi = -cosi;
    } else {
        // Exiting from material into air (swap IORs)
        float temp = etai;
        etai = etat;
        etat = temp;
        n = -N;
    }
    
    float eta = etai / etat;
    float k = 1.0 - eta * eta * (1.0 - cosi * cosi);
    
    // Total internal reflection check
    if (k < 0.0) {
        // Total internal reflection - return reflection instead
        return reflect(I, N);
    }
    
    // Calculate refraction direction
    return normalize(eta * I + (eta * cosi - sqrt(k)) * n);
}

// Sample environment map with optional refraction offset for transmissive materials
vec3 sampleEnvironmentWithRefraction(vec3 viewDir, vec3 normal, float roughness, float transmission, float materialIOR) {
    // Calculate both reflection and refraction directions
    vec3 reflectionDir = reflect(-viewDir, normal);
    vec3 refractionDir = calculateRefraction(-viewDir, normal, materialIOR);
    
    // Sample environment map
    float lod = roughness * prefilteredMaxLOD;
    vec3 reflectedColor = textureLod(prefilteredMap, reflectionDir, lod).rgb;
    vec3 refractedColor = textureLod(prefilteredMap, refractionDir, lod).rgb;
    
    // Mix reflection and refraction based on transmission factor
    return mix(reflectedColor, refractedColor, transmission);
}

vec3 ComputeVolumeTransmittance(vec3 baseTint, float thickness, float distanceScale, vec3 volumeColor) {
    float pathLength = max(thickness, 0.0);
    if (pathLength <= 0.0) {
        return vec3(1.0);
    }

    vec3 safeColor = max(volumeColor, vec3(1e-3));
    vec3 sigmaA = -log(safeColor) / max(distanceScale, 1e-3);
    vec3 volumeTransmittance = exp(-sigmaA * pathLength);
    return mix(vec3(1.0), volumeTransmittance * baseTint, clamp(pathLength, 0.0, 1.0));
}

void main() {
    // Sample base color and alpha
    vec4 baseColorSample = hasBaseColorTexture ? texture(texture_diffuse, fs_in.UV) : vec4(1.0);
    vec4 baseColor = baseColorSample * baseColorFactor;
    float alpha = baseColor.a;
    
    // Alpha testing for masked materials
    if (alpha < alphaCutoff) discard;
    
    // Sample material properties with proper texture presence checks
    float metallic = metallicFactor;
    float roughness = roughnessFactor;
    if (hasMetallicRoughnessTexture) {
        vec4 mrSample = texture(texture_metallic_roughness, fs_in.UV);
        metallic = clamp(mrSample.b * metallicFactor, 0.0, 1.0);
        roughness = clamp(mrSample.g * roughnessFactor, 0.04, 1.0);
    }
    
    // Sample transmission from material
    float transmission = transmissionFactor;
    if (hasTransmissionTexture) {
        transmission *= texture(texture_transmission, fs_in.UV).r;
    }
    float thickness = thicknessFactor;
    
    // Sample emissive
    vec3 emissive = emissiveFactor;
    if (hasEmissiveTexture) {
        emissive *= texture(texture_emissive, fs_in.UV).rgb;
    }
    
    // Sample occlusion
    float ao = 1.0;
    if (hasOcclusionTexture) {
        ao = mix(1.0, texture(texture_occlusion, fs_in.UV).r, occlusionStrength);
    }
    
    // Calculate vectors
    vec3 N = getNormalFromMap();
    vec3 V = normalize(viewPos - fs_in.WorldPos);
    vec3 R = reflect(-V, N);
    
    float NdotV = max(dot(N, V), 0.0);
    
    vec3 albedo = baseColor.rgb;
    float specFactorSample = 1.0;
    if (hasSpecularTexture) {
        specFactorSample *= texture(texture_specular, fs_in.UV).a;
    }
    vec3 specularColor = specularColorFactor;
    if (hasSpecularColorTexture) {
        specularColor *= texture(texture_specular_color, fs_in.UV).rgb;
    }
    vec3 F0 = ComputeSurfaceF0(albedo, metallic, ior, specularFactor * specFactorSample, specularColor);
    
    // === TRANSMISSIVE MATERIAL RENDERING ===
    // For transparent materials, we combine:
    // 1. Environment reflections (stronger at grazing angles)
    // 2. Refracted environment (for transmission)
    // 3. Base color tinting (for colored materials)
    
    // Sample environment with both reflection and refraction using material IOR
    vec3 envColor = sampleEnvironmentWithRefraction(V, N, roughness, transmission, ior);
    
    // Apply Fresnel-modulated environment contribution
    float transmissionWeight = ComputeTransmissionWeight(transmission, NdotV, F0);
    
    // === DIRECT LIGHTING ===
    vec3 directLighting = vec3(0.0);
    
    vec3 L = normalize(-keyLightDir);
    float NdotL = max(dot(N, L), 0.0);
    
    if (NdotL > 0.0) {
        vec3 directDiffuse, directSpecular;
        EvaluateCanonicalBRDFSeparated(N, V, L, albedo, metallic, roughness, F0, transmission, clearcoatFactor, clearcoatRoughnessFactor, directDiffuse, directSpecular);
        directLighting = (directDiffuse + directSpecular) * keyLightColor * keyLightIntensity;
    }
    
    // === IBL ===
    float diffuseAO = ao;
    float specularAO = SpecularOcclusion(NdotV, diffuseAO, roughness);
    vec3 pbrIBL = EvaluateCanonicalIBL(
        N, V, R, albedo, metallic, roughness, F0, transmission, clearcoatFactor, clearcoatRoughnessFactor,
        diffuseAO, specularAO,
        irradianceMap, prefilteredMap, brdfLUT,
        prefilteredMaxLOD, iblIntensity, diffuseIBLScale, specularIBLScale
    );

    // Blend the canonical PBR response with the transmission-tinted environment.
    vec3 transmittance = ComputeVolumeTransmittance(albedo, thickness, attenuationDistance, attenuationColor);
    vec3 transmissiveColor = mix(
        pbrIBL + directLighting,
        envColor * transmittance,
        transmissionWeight
    );

    // === COMBINE LIGHTING ===
    // Balance between transmissive appearance and standard PBR
    vec3 finalColor = transmissiveColor + emissive * emissiveStrength;
    
    //Calculate final alpha for transmissive materials
    // Base alpha is controlled by material alpha and transmission factor
    float mediumOpacity = 1.0 - dot(transmittance, vec3(0.3333333333));
    float baseAlpha = alpha * (1.0 - transmissionWeight * 0.9) + mediumOpacity * transmissionWeight;
    
    // Fresnel increases opacity at edges for realistic appearance
    float finalAlpha = mix(baseAlpha, min(baseAlpha + transmissionWeight * 0.4, 1.0), 0.5);
    
    // For highly transmissive materials, ensure minimum visibility
    if (transmissionWeight > 0.5) {
        finalAlpha = max(finalAlpha, 0.05);
    }
    
    // Ensure alpha stays in valid range
    finalAlpha = clamp(finalAlpha, 0.0, 1.0);
    
    // === OUTPUT ===
    // Output in linear color space - gamma correction happens in post-processing
    FragColor = vec4(finalColor, finalAlpha);
}
