#version 460 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aTangent;
layout(location=4) in ivec4 aBoneIDs;
layout(location=5) in vec4 aBoneWeights;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

// Skinning uniforms
uniform bool useSkinning = false;
uniform mat4 bones[64];

out VS_OUT {
    vec3 WorldPos;
    vec3 Normal;
    vec2 UV;
    vec4 TangentWS; // Tangent in world space (w = handedness)
} vs_out;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;
    vec3 localTangent = aTangent.xyz;
    
    // Apply skinning if enabled
    if (useSkinning) {
        mat4 skinMatrix = bones[aBoneIDs[0]] * aBoneWeights[0];
        skinMatrix += bones[aBoneIDs[1]] * aBoneWeights[1];
        skinMatrix += bones[aBoneIDs[2]] * aBoneWeights[2];
        skinMatrix += bones[aBoneIDs[3]] * aBoneWeights[3];
        
        localPos = skinMatrix * localPos;
        localNormal = mat3(skinMatrix) * localNormal;
        localTangent = mat3(skinMatrix) * localTangent;
    }
    
    // Transform to world space
    vec4 worldPos = model * localPos;
    vs_out.WorldPos = worldPos.xyz;
    
    // Transform normal and tangent to world space
    mat3 normalMatrix = mat3(transpose(inverse(model)));
    vs_out.Normal = normalize(normalMatrix * localNormal);
    
    // Transform tangent to world space, preserve handedness
    vec3 worldTangent = normalize(normalMatrix * localTangent);
    vs_out.TangentWS = vec4(worldTangent, aTangent.w);
    
    vs_out.UV = aUV;
    
    gl_Position = projection * view * worldPos;
}
