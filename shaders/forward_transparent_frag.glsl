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
uniform float occlusionStrength = 1.0;
uniform float normalScale = 1.0;
uniform float alphaCutoff = 0.5;

// KHR_materials_specular extension
uniform float specularFactor = 1.0;
uniform vec3 specularColorFactor = vec3(1.0);

// KHR_materials_transmission extension (from material, not hardcoded)
uniform float transmissionFactor = 0.0;    // Material transmission [0,1]
uniform sampler2D texture_transmission;    // Transmission texture

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

void main() {
    //Calculate screen UV from fragment position for depth reads
    vec2 screenUV = gl_FragCoord.xy / screenSize;
    
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
    
    // Calculate F0 using material IOR and specular extension
    vec3 albedo = baseColor.rgb;
    float baseF0 = F0FromIOR(ior);
    
    // Apply specular factor and color if using specular extension
    float specFactorSample = specularFactor;
    if (hasSpecularTexture) {
        specFactorSample *= texture(texture_specular, fs_in.UV).a;
    }
    
    vec3 specColorSample = specularColorFactor;
    if (hasSpecularColorTexture) {
        specColorSample *= texture(texture_specular_color, fs_in.UV).rgb;
    }
    
    vec3 dielectricF0 = vec3(baseF0) * specFactorSample * specColorSample;
    vec3 F0 = mix(dielectricF0, albedo, metallic);
    
    //Calculate Fresnel term for physically correct reflections
    // Transmissive materials reflect more at grazing angles (Fresnel effect)
    float fresnel = pow(1.0 - NdotV, 5.0);
    vec3 fresnelTerm = FresnelSchlickRoughness(NdotV, F0, roughness);
    
    // === TRANSMISSIVE MATERIAL RENDERING ===
    // For transparent materials, we combine:
    // 1. Environment reflections (stronger at grazing angles)
    // 2. Refracted environment (for transmission)
    // 3. Base color tinting (for colored materials)
    
    // Sample environment with both reflection and refraction using material IOR
    vec3 envColor = sampleEnvironmentWithRefraction(V, N, roughness, transmission, ior);
    
    // Apply Fresnel-modulated environment contribution
    vec3 reflectionColor = textureLod(prefilteredMap, R, roughness * prefilteredMaxLOD).rgb;
    
    // Blend reflection and transmission based on Fresnel and transmission factor
    vec3 transmissiveColor = mix(
        envColor * albedo,           // Refracted color (tinted by base color)
        reflectionColor,             // Reflected color
        fresnel * (1.0 - transmission) // Fresnel increases reflection at edges
    );
    
    // === DIRECT LIGHTING ===
    // Transmissive materials have reduced diffuse contribution
    vec3 directLighting = vec3(0.0);
    
    vec3 L = normalize(-keyLightDir);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    
    if (NdotL > 0.0) {
        // Specular highlights
        float D = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = FresnelSchlick(HdotV, F0);
        
        vec3 numerator = D * G * F;
        float denominator = 4.0 * max(NdotV, 0.0001) * max(NdotL, 0.0001);
        vec3 specular = numerator / max(denominator, 0.0001);
        
        // Diffuse contribution (reduced by transmission and metallic)
        vec3 kS = F;
        vec3 kD = (1.0 - kS) * (1.0 - metallic) * (1.0 - transmission);
        vec3 diffuse = kD * albedo / PI;
        
        // Scale direct lighting based on transmission (more transmissive = less direct lighting)
        float directScale = mix(1.0, 0.3, transmission);
        directLighting = (diffuse + specular) * keyLightColor * keyLightIntensity * NdotL * directScale;
    }
    
    // === IBL ===
    // Diffuse IBL (reduced by transmission)
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = irradiance * albedo * (1.0 - metallic) * (1.0 - transmission) * ao;
    
    // Specular IBL
    vec2 brdf = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specularIBL = reflectionColor * (fresnelTerm * brdf.x + brdf.y) * ao;
    
    // === COMBINE LIGHTING ===
    // Balance between transmissive appearance and standard PBR
    vec3 finalColor = mix(
        diffuseIBL + specularIBL + directLighting,  // Standard PBR
        transmissiveColor,                           // Transmissive appearance
        transmission * 0.7                           // Blend based on transmission
    ) + emissive;
    
    //Calculate final alpha for transmissive materials
    // Base alpha is controlled by material alpha and transmission factor
    float baseAlpha = alpha * (1.0 - transmission * 0.9);
    
    // Fresnel increases opacity at edges for realistic appearance
    float finalAlpha = mix(baseAlpha, min(baseAlpha + fresnel * 0.4, 1.0), 0.5);
    
    // For highly transmissive materials, ensure minimum visibility
    if (transmission > 0.5) {
        finalAlpha = max(finalAlpha, 0.05);
    }
    
    // Ensure alpha stays in valid range
    finalAlpha = clamp(finalAlpha, 0.0, 1.0);
    
    // Read scene depth for transparency ordering (from packed G-buffer depth)
    float sceneDepth = texture(gDepth, screenUV).r;
    
    // === OUTPUT ===
    // Output in linear color space - gamma correction happens in post-processing
    FragColor = vec4(finalColor, finalAlpha);
}
