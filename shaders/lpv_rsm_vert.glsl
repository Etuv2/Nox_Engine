#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec3 aTangent;

// Output to fragment shader
out VS_OUT {
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 texCoord;
} vs_out;

// Uniforms
uniform mat4 model;
uniform mat4 lightSpaceMatrix; // light view-projection (RenderShadowCascade contract)

void main() {
    // Transform to world space
    vec4 worldPos = model * vec4(aPosition, 1.0);
    vs_out.worldPosition = worldPos.xyz;
    
    // Transform normal to world space (assume uniform scaling for simplicity)
    mat3 normalMatrix = mat3(transpose(inverse(model)));
    vs_out.worldNormal = normalize(normalMatrix * aNormal);
    
    vs_out.texCoord = aTexCoord;
    
    // Transform to light space
    gl_Position = lightSpaceMatrix * worldPos;
}
