#version 460 core

// Input from vertex shader
in VS_OUT {
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 texCoord;
} fs_in;

// Output attachments
layout(location = 0) out vec3 outPosition;  // World-space position
layout(location = 1) out vec3 outNormal;    // World-space normal
layout(location = 2) out vec3 outFlux;      // Albedo * N·L * LightColor * LightIntensity

// Material uniforms
uniform sampler2D texture_diffuse;
uniform vec4 baseColorFactor = vec4(1.0);
uniform bool hasBaseColorTexture = false;

// Light uniforms
uniform vec3 u_lightColor = vec3(1.0);
uniform float u_lightIntensity = 1.0;
uniform vec3 u_lightDirection = vec3(0.0, -1.0, 0.0); // CRITICAL: Actual light direction from CPU

void main() {
    // Output world position
    outPosition = fs_in.worldPosition;
    
    // Output world normal (normalized)
    outNormal = normalize(fs_in.worldNormal);
    
    // Sample albedo
    vec3 albedo = baseColorFactor.rgb;
    if (hasBaseColorTexture) {
        vec4 texSample = texture(texture_diffuse, fs_in.texCoord);
        albedo *= texSample.rgb;
    }
    
    // CRITICAL FIX: Calculate N·L using actual light direction passed from CPU
    // Light direction points FROM light source, so we negate it
    vec3 lightDir = normalize(u_lightDirection);
    float NdotL = max(dot(outNormal, -lightDir), 0.0);
    
    // Calculate flux: albedo * N·L * lightColor * lightIntensity
    // Flux represents the outgoing light energy (exitant radiance)
    // Scale by PI to convert from irradiance to radiance (Lambertian BRDF)
    const float PI = 3.14159265359;
    outFlux = (albedo / PI) * NdotL * u_lightColor * u_lightIntensity;
    
    // Energy boost for better visibility (can be tuned)
    outFlux *= 2.0;
}
