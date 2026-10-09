#version 460 core
#include "includes/skinning_common.glsl"
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=10) in vec2 aUV1;
layout(location=3) in vec4 aTangent;
layout(location=4) in ivec4 aBoneIDs;
layout(location=5) in vec4 aBoneWeights;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out VS_OUT {
    vec3 WorldPos;
    vec3 Normal;
    vec2 UV;
    vec2 UV1;
    vec4 TangentWS; // Tangent in world space (w = handedness)
    vec3 ObjectPos;
    vec3 ObjectNormal;
} vs_out;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;
    vec3 localTangent = aTangent.xyz;
    
    ApplySkinning(aBoneIDs, aBoneWeights, localPos, localNormal, localTangent);
    
    // Transform to world space
    vec4 worldPos = model * localPos;
    vs_out.WorldPos = worldPos.xyz;
    
    // Normal Matrix (transpose of inverse for non-uniform scaling)
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    
    vec3 N;
    vec3 T;
    vec3 B;
    BuildWorldTBN(normalMatrix, localNormal, localTangent, aTangent.w, N, T, B);
    
    // Store outputs
    vs_out.Normal = N;
    
    // Transform tangent to world space, preserve handedness
    // The handedness (aTangent.w) determines if we need to flip the bitangent
    vs_out.TangentWS = vec4(T, aTangent.w);
    
    vs_out.UV = aUV;

    // Object-local position/normal with rotation and translation removed but scale kept, so
    // procedural detail (rain beads) sticks to moving objects at a world-metre scale.
    mat3 objectRotation = mat3(normalize(model[0].xyz), normalize(model[1].xyz), normalize(model[2].xyz));
    vs_out.ObjectPos = transpose(objectRotation) * (worldPos.xyz - model[3].xyz);
    vs_out.ObjectNormal = transpose(objectRotation) * N;
    vs_out.UV1 = aUV1;
    
    gl_Position = projection * view * worldPos;
}
