#version 450 core
out vec4 FragColor;

in VS_OUT {
    vec3 WorldPos;
    vec3 Normal;
    vec2 UV;
    vec4 TangentWS; // Tangent in world space (w = handedness)
} fs_in;

// Material properties
uniform vec4 baseColorFactor = vec4(1.0);
uniform float metallicFactor = 0.0;
uniform float roughnessFactor = 1.0;
uniform vec3 emissiveFactor = vec3(0.0);
uniform float occlusionStrength = 1.0;
uniform float specularFactor = 0.5;
uniform vec3 specularColorFactor = vec3(1.0);
uniform float normalScale = 1.0;
uniform float alphaCutoff = 0.5;

// Material textures
uniform sampler2D texture_diffuse;
uniform sampler2D texture_normal;
uniform sampler2D texture_metallic_roughness;
uniform sampler2D texture_emissive;
uniform sampler2D texture_occlusion;
uniform sampler2D texture_specular;

// IBL textures
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform float prefilteredMaxLOD = 4.0;

// Camera - Match legacy renderer uniform name
uniform vec3 viewPos;

// Enhanced lighting uniforms
uniform vec3 keyLightDir = normalize(vec3(-0.4, -1.0, -0.2));
uniform vec3 keyLightColor = vec3(1.0);
uniform float keyLightIntensity = 1.0;

// Enhanced glass material parameters
uniform float transmissionFactor = 0.9;    // High transmission for glass (0.9 = 90% transparent)
uniform float refractionIndex = 1.5;       // Typical glass IOR (1.5 for standard glass)
uniform bool useScreenSpaceRefraction = false; // Screen-space refraction toggle

// Utility functions
const float PI = 3.14159265359;

vec3 getNormalFromMap() {
    vec3 tangentNormal = texture(texture_normal, fs_in.UV).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    
    vec3 N = normalize(fs_in.Normal);
    vec3 T = normalize(fs_in.TangentWS.xyz);
    vec3 B = cross(N, T) * fs_in.TangentWS.w;
    mat3 TBN = mat3(T, B, N);
    
    return normalize(TBN * tangentNormal);
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return a2 / max(denom, 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    
    float denom = NdotV * (1.0 - k) + k;
    
    return NdotV / max(denom, 0.0001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    
    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Simplified but physically accurate refraction calculation for glass
vec3 calculateRefraction(vec3 I, vec3 N, float ior) {
    // I is incident direction (view direction), N is surface normal
    float cosi = dot(I, N);
    float etai = 1.0; // Air
    float etat = ior; // Glass
    vec3 n = N;
    
    // Check if we're entering or exiting the material
    if (cosi < 0.0) {
        // Entering from air into glass
        cosi = -cosi;
    } else {
        // Exiting from glass into air (swap IORs)
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

// Sample environment map with optional refraction offset for glass-like materials
vec3 sampleEnvironmentWithRefraction(vec3 viewDir, vec3 normal, float roughness, float transmission) {
    // Calculate both reflection and refraction directions
    vec3 reflectionDir = reflect(-viewDir, normal);
    vec3 refractionDir = calculateRefraction(-viewDir, normal, refractionIndex);
    
    // Sample environment map
    float lod = roughness * prefilteredMaxLOD;
    vec3 reflectedColor = textureLod(prefilteredMap, reflectionDir, lod).rgb;
    vec3 refractedColor = textureLod(prefilteredMap, refractionDir, lod).rgb;
    
    // Mix reflection and refraction based on transmission factor
    return mix(reflectedColor, refractedColor, transmission);
}

void main() {
    // Sample base color and alpha
    vec4 baseColor = texture(texture_diffuse, fs_in.UV) * baseColorFactor;
    float alpha = baseColor.a;
    
    // Alpha testing for masked materials
    if (alpha < alphaCutoff) discard;
    
    // Sample material properties
    vec3 mrSample = texture(texture_metallic_roughness, fs_in.UV).rgb;
    float metallic = clamp(mrSample.b * metallicFactor, 0.0, 1.0);
    float roughness = clamp(mrSample.g * roughnessFactor, 0.04, 1.0);
    
    vec3 emissive = emissiveFactor * texture(texture_emissive, fs_in.UV).rgb;
    float ao = mix(1.0, texture(texture_occlusion, fs_in.UV).r, occlusionStrength);
    
    // Calculate vectors - Use viewPos instead of cameraPos
    vec3 N = getNormalFromMap();
    vec3 V = normalize(viewPos - fs_in.WorldPos);
    vec3 R = reflect(-V, N);
    
    float NdotV = max(dot(N, V), 0.0);
    
    // Calculate F0 for glass materials (use base color tint for colored glass)
    vec3 albedo = baseColor.rgb;
    vec3 F0 = mix(vec3(0.04), albedo, metallic); // Glass is dielectric (F0 ~0.04)
    
    // CRITICAL: Calculate Fresnel term for physically correct glass reflections
    // Glass reflects more at grazing angles (Fresnel effect)
    float fresnel = pow(1.0 - NdotV, 5.0);
    vec3 fresnelTerm = fresnelSchlickRoughness(NdotV, F0, roughness);
    
    // === GLASS MATERIAL RENDERING ===
    // For transparent glass, we combine:
    // 1. Environment reflections (stronger at grazing angles)
    // 2. Refracted environment (for transmission)
    // 3. Base color tinting (for colored glass)
    
    // Sample environment with both reflection and refraction
    vec3 envColor = sampleEnvironmentWithRefraction(V, N, roughness, transmissionFactor);
    
    // Apply Fresnel-modulated environment contribution
    vec3 reflectionColor = textureLod(prefilteredMap, R, roughness * prefilteredMaxLOD).rgb;
    
    // Blend reflection and transmission based on Fresnel and transmission factor
    vec3 glassColor = mix(
        envColor * albedo,           // Refracted color (tinted by base color)
        reflectionColor,             // Reflected color
        fresnel * (1.0 - transmissionFactor) // Fresnel increases reflection at edges
    );
    
    // === DIRECT LIGHTING (minimal for glass) ===
    // Glass materials typically have very little direct diffuse contribution
    vec3 directLighting = vec3(0.0);
    
    vec3 L = normalize(-keyLightDir);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    
    if (NdotL > 0.0) {
        // Only specular highlights for glass (no diffuse)
        float D = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(HdotV, F0);
        
        vec3 numerator = D * G * F;
        float denominator = 4.0 * max(NdotV, 0.0001) * max(NdotL, 0.0001);
        vec3 specular = numerator / max(denominator, 0.0001);
        
        directLighting = specular * keyLightColor * keyLightIntensity * NdotL * 0.5; // Reduced for glass
    }
    
    // === IBL SPECULAR (for additional reflections) ===
    vec2 brdf = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 iblSpecular = reflectionColor * (fresnelTerm * brdf.x + brdf.y);
    
    // === COMBINE LIGHTING ===
    vec3 finalColor = glassColor + directLighting * 0.5 + iblSpecular * 0.3 + emissive;
    
    // CRITICAL: Calculate final alpha for glass materials
    // Glass is more opaque at grazing angles (Fresnel effect on transparency)
    // Base alpha is controlled by material alpha and transmission factor
    float baseAlpha = alpha * (1.0 - transmissionFactor * 0.9); // Glass is mostly transparent
    
    // Fresnel increases opacity at edges for realistic glass appearance
    float finalAlpha = mix(baseAlpha, min(baseAlpha + fresnel * 0.4, 1.0), 0.7);
    
    // Ensure alpha stays in valid range
    finalAlpha = clamp(finalAlpha, 0.0, 1.0);
    
    // === OUTPUT ===
    // Output in linear color space - gamma correction happens in post-processing
    FragColor = vec4(finalColor, finalAlpha);
}
