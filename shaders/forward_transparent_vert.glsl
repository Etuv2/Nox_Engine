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

// Skinning uniforms - consistent naming with gbuffer_vert.glsl
uniform bool u_enableSkinning;
uniform mat4 u_boneMatrices[128];

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
    if (u_enableSkinning) {
        // Calculate total weight to detect degenerate cases
        float totalWeight = aBoneWeights[0] + aBoneWeights[1] + aBoneWeights[2] + aBoneWeights[3];
        
        // Only apply skinning if we have valid bone weights
        if (totalWeight > 0.001) {
            // Normalize weights to ensure they sum to 1.0
            vec4 normalizedWeights = aBoneWeights / totalWeight;
            
            // Clamp bone IDs to valid range
            int boneID0 = clamp(aBoneIDs[0], 0, 127);
            int boneID1 = clamp(aBoneIDs[1], 0, 127);
            int boneID2 = clamp(aBoneIDs[2], 0, 127);
            int boneID3 = clamp(aBoneIDs[3], 0, 127);
            
            mat4 skinMatrix = u_boneMatrices[boneID0] * normalizedWeights[0];
            skinMatrix += u_boneMatrices[boneID1] * normalizedWeights[1];
            skinMatrix += u_boneMatrices[boneID2] * normalizedWeights[2];
            skinMatrix += u_boneMatrices[boneID3] * normalizedWeights[3];
            
            localPos = skinMatrix * localPos;
            localNormal = mat3(skinMatrix) * localNormal;
            localTangent = mat3(skinMatrix) * localTangent;
        }
        // If totalWeight <= 0.001, use the original vertex position (identity skinning)
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
