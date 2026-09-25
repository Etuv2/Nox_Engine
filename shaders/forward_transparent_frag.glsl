#version 460 core

// Include shared PBR functions
#include "includes/pbr_common.glsl"
#include "includes/material_common.glsl"
#include "includes/lighting_common.glsl"

out vec4 FragColor;

in VS_OUT {
    vec3 WorldPos;
    vec3 Normal;
    vec2 UV;
    vec2 UV1;
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
uniform int alphaMode = 2; // 0=OPAQUE, 1=MASK, 2=BLEND

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

// glTF per-texture UV set selectors (0=TEXCOORD_0, 1=TEXCOORD_1)
uniform int baseColorUVSet = 0;
uniform int normalUVSet = 0;
uniform int metallicRoughnessUVSet = 0;
uniform int emissiveUVSet = 0;
uniform int occlusionUVSet = 0;
uniform int specularUVSet = 0;
uniform int specularColorUVSet = 0;
uniform int transmissionUVSet = 0;

// IBL textures
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform float prefilteredMaxLOD = 4.0;
uniform float iblIntensity = 0.1;
uniform float diffuseIBLScale = 0.3;
uniform float specularIBLScale = 0.45;

// Camera
uniform vec3 viewPos;


vec3 getNormalFromMap() {
    // Start with geometric normal
    vec3 N = normalize(fs_in.Normal);
    
    // If no normal texture, return geometric normal
    if (!hasNormalTexture) {
        return N;
    }
    
    // Sample normal map in tangent space
    vec3 tangentNormal = texture(texture_normal, SelectUVSet(normalUVSet, fs_in.UV, fs_in.UV1)).xyz * 2.0 - 1.0;
    
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

vec3 sampleEnvironmentReflection(vec3 viewDir, vec3 normal, float roughness) {
    vec3 reflectionDir = reflect(-viewDir, normal);
    float lod = roughness * prefilteredMaxLOD;
    return textureLod(prefilteredMap, reflectionDir, lod).rgb;
}

vec3 sampleEnvironmentRefraction(
    vec3 viewDir,
    vec3 normal,
    float roughness,
    float materialIOR,
    out bool totalInternalReflection
) {
    vec3 refractionDir = ComputeRefractionDirection(-viewDir, normal, materialIOR, totalInternalReflection);
    float lod = roughness * prefilteredMaxLOD;
    return textureLod(prefilteredMap, refractionDir, lod).rgb;
}

void main() {
    // Sample base color and alpha
    vec4 baseColorSample = hasBaseColorTexture ? texture(texture_diffuse, SelectUVSet(baseColorUVSet, fs_in.UV, fs_in.UV1)) : vec4(1.0);
    vec4 baseColor = baseColorSample * baseColorFactor;
    float alpha = baseColor.a;
    
    if ((alphaMode == 1 && alpha < alphaCutoff) || alpha <= 0.001) discard;
    
    // Sample material properties with proper texture presence checks
    float metallic = metallicFactor;
    float roughness = roughnessFactor;
    if (hasMetallicRoughnessTexture) {
        vec4 mrSample = texture(texture_metallic_roughness, SelectUVSet(metallicRoughnessUVSet, fs_in.UV, fs_in.UV1));
        metallic = clamp(mrSample.b * metallicFactor, 0.0, 1.0);
        roughness = clamp(mrSample.g * roughnessFactor, 0.04, 1.0);
    }
    
    // Sample transmission from material
    float transmission = transmissionFactor;
    if (hasTransmissionTexture) {
        transmission *= texture(texture_transmission, SelectUVSet(transmissionUVSet, fs_in.UV, fs_in.UV1)).r;
    }
    // Sample emissive
    vec3 emissive = emissiveFactor;
    if (hasEmissiveTexture) {
        emissive *= texture(texture_emissive, SelectUVSet(emissiveUVSet, fs_in.UV, fs_in.UV1)).rgb;
    }
    
    // Sample occlusion
    float ao = 1.0;
    if (hasOcclusionTexture) {
        ao = mix(1.0, texture(texture_occlusion, SelectUVSet(occlusionUVSet, fs_in.UV, fs_in.UV1)).r, occlusionStrength);
    }
    
    // Calculate vectors
    vec3 N = getNormalFromMap();
    vec3 V = normalize(viewPos - fs_in.WorldPos);
    vec3 R = reflect(-V, N);
    
    float NdotV = max(dot(N, V), 0.0);
    
    vec3 albedo = baseColor.rgb;
    float specFactorSample = 1.0;
    if (hasSpecularTexture) {
        specFactorSample *= texture(texture_specular, SelectUVSet(specularUVSet, fs_in.UV, fs_in.UV1)).a;
    }
    vec3 specularColor = specularColorFactor;
    if (hasSpecularColorTexture) {
        specularColor *= texture(texture_specular_color, SelectUVSet(specularColorUVSet, fs_in.UV, fs_in.UV1)).rgb;
    }

    PrincipledSurface surface = BuildPrincipledSurface(
        albedo,
        metallic,
        roughness,
        ior,
        specularFactor * specFactorSample,
        specularColor,
        transmission,
        clearcoatFactor,
        clearcoatRoughnessFactor,
        0.0,
        0.0,
        0.0,
        thicknessFactor,
        attenuationColor,
        attenuationDistance
    );
    
    // Fresnel-modulated transmission weighting.
    float transmissionWeight = ComputeTransmissionWeight(surface.transmission, NdotV, surface.specularF0);
    vec3 transmittance = ComputeVolumeTransmittance(
        surface.baseColor,
        surface.thickness,
        surface.attenuationDistance,
        surface.attenuationColor
    );

    bool totalInternalReflection = false;
    vec3 reflectedEnv = sampleEnvironmentReflection(V, N, surface.perceptualRoughness);
    vec3 refractedEnv = sampleEnvironmentRefraction(V, N, surface.perceptualRoughness, surface.ior, totalInternalReflection);
    if (totalInternalReflection) {
        refractedEnv = reflectedEnv;
    }
    
    // === DIRECT LIGHTING ===
    // Same light loop, attenuation and shadowing as the deferred pass (includes/lighting_common.glsl).
    vec3 directLighting = vec3(0.0);
    int lightCount = min(numLights, 64);
    for (int i = 0; i < lightCount; ++i) {
        vec3 lightDiffuse;
        vec3 lightSpecular;
        float lightVisibility;
        if (EvaluateLightRadiance(i, fs_in.WorldPos, N, V, surface, lightDiffuse, lightSpecular, lightVisibility)) {
            directLighting += (lightDiffuse + lightSpecular) * lightVisibility;
        }
    }
    
    // === IBL ===
    float diffuseAO = ao;
    float specularAO = SpecularOcclusion(NdotV, diffuseAO, surface.perceptualRoughness);
    vec3 pbrIBL = EvaluatePrincipledIBL(
        surface, N, V, R,
        diffuseAO, specularAO,
        irradianceMap, prefilteredMap, brdfLUT,
        prefilteredMaxLOD, iblIntensity, diffuseIBLScale, specularIBLScale
    );

    // Keep reflection/specular on the shared BRDF path and add a separate transmitted lobe.
    vec3 reflectiveColor = pbrIBL + directLighting;
    vec3 transmittedColor = refractedEnv * transmittance * transmissionWeight;

    // === COMBINE LIGHTING ===
    vec3 finalColor = reflectiveColor + transmittedColor + emissive * emissiveStrength;
    
    // Medium opacity comes from Beer-Lambert attenuation and blends with surface alpha.
    float mediumOpacity = 1.0 - dot(transmittance, vec3(0.3333333333));
    float surfaceOpacity = alpha * (1.0 - transmissionWeight);
    float transmissionOpacity = mediumOpacity * transmissionWeight;
    float finalAlpha = surfaceOpacity + transmissionOpacity;
    
    // Ensure alpha stays in valid range
    finalAlpha = clamp(finalAlpha, 0.0, 1.0);
    
    // === OUTPUT ===
    // Output in linear color space - gamma correction happens in post-processing
    FragColor = vec4(finalColor, finalAlpha);
}
