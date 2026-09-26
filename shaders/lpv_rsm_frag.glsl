#version 460 core

// Reflective shadow map (Dachsbacher & Stamminger 2005) for the LPV: every texel is a surface
// patch lit by the directional light, which the injection pass turns into a virtual point light.

in VS_OUT {
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 texCoord;
} fs_in;

layout(location = 0) out vec3 outPosition;  // World-space position
layout(location = 1) out vec3 outNormal;    // World-space normal, facing the light
layout(location = 2) out vec3 outFlux;      // Reflected flux per unit light-perpendicular area (W/m^2)

// Material uniforms
uniform sampler2D texture_diffuse;
uniform vec4 baseColorFactor = vec4(1.0);
uniform bool hasBaseColorTexture = false;
uniform float metallicFactor = 0.0;

// Light uniforms
uniform vec3 u_lightIrradiance = vec3(0.0);           // color * intensity: irradiance facing the light
uniform vec3 u_lightDirection = vec3(0.0, -1.0, 0.0); // direction the light travels

void main() {
    outPosition = fs_in.worldPosition;

    // The texel is the side of the surface the light reaches.
    vec3 normal = normalize(fs_in.worldNormal);
    if (dot(normal, u_lightDirection) > 0.0) {
        normal = -normal;
    }
    outNormal = normal;

    vec3 albedo = baseColorFactor.rgb;
    if (hasBaseColorTexture) {
        albedo *= texture(texture_diffuse, fs_in.texCoord).rgb;
    }
    // Metals reflect no diffuse light.
    albedo *= 1.0 - clamp(metallicFactor, 0.0, 1.0);

    // A texel covers a fixed area A perpendicular to the light, so the surface it sees receives
    // E * A whatever its slope (the cosine cancels against the larger surface area) and reflects
    // albedo * E * A. The injection pass multiplies by the area each VPL stands for.
    outFlux = albedo * u_lightIrradiance;
}
