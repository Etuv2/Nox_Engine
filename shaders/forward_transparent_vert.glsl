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
uniform mat4 bones[128];

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
    
    // Normal Matrix (transpose of inverse for non-uniform scaling)
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    
    // Gram-Schmidt orthogonalization for seamless tangent space
    // This matches gbuffer_vert.glsl for consistent normal mapping
    vec3 N = normalize(normalMatrix * localNormal);
    vec3 T = normalize(normalMatrix * localTangent);
    
    // Re-orthogonalize tangent with respect to normal (Gram-Schmidt process)
    // This ensures T is perpendicular to N, preventing seams at UV boundaries
    T = normalize(T - dot(T, N) * N);
    
    // Store outputs
    vs_out.Normal = N;
    
    // Transform tangent to world space, preserve handedness
    // The handedness (aTangent.w) determines if we need to flip the bitangent
    vs_out.TangentWS = vec4(T, aTangent.w);
    
    vs_out.UV = aUV;
    
    gl_Position = projection * view * worldPos;
}
