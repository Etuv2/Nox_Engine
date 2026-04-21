#version 460 core
#include "includes/transform_tracking_contract.glsl"
#include "includes/skinning_common.glsl"

layout(location = 0) in vec3 aPos;       
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 10) in vec2 aTexCoords1;
layout(location = 3) in vec4 aTangent; // XYZ = tangent direction, W = handedness
layout(location = 4) in ivec4 aBoneIDs;
layout(location = 5) in vec4 aBoneWeights;
layout(location = 11) in uint aTransformRecordIndex;
layout(location = 12) in uint aStableTransformID;

// Outputs to the fragment shader (geometry pass)
out vec3 WorldPos;
out vec3 WorldNormal;
out vec2 TexCoords;
out vec2 TexCoords1;
out mat3 TBN;
out vec4 RawTangent; 
flat out uint TransformID;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform mat3 normalMatrix;
uniform uint uTransformID;
uniform bool uUseVertexTransformID = false;

layout(std430, binding = 6) readonly buffer GlobalTransformBuffer {
    GpuTransformRecord gpuTransforms[];
};

void main()
{
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;
    vec3 localTangent = aTangent.xyz;
    
    ApplySkinning(aBoneIDs, aBoneWeights, localPos, localNormal, localTangent);
    
    uint resolvedTransformID = uUseVertexTransformID ? aStableTransformID : uTransformID;
    mat4 resolvedModel = model;
    mat3 resolvedNormalMatrix = normalMatrix;
    if (uUseVertexTransformID) {
        resolvedModel = gpuTransforms[aTransformRecordIndex].world;
        resolvedNormalMatrix = transpose(inverse(mat3(resolvedModel)));
    }

    // Compute world-space position
    vec4 worldPos = resolvedModel * localPos;
    WorldPos = worldPos.xyz;
    TexCoords = aTexCoords;
    TexCoords1 = aTexCoords1;

    vec3 N;
    vec3 T;
    vec3 B;
    BuildWorldTBN(resolvedNormalMatrix, localNormal, localTangent, aTangent.w, N, T, B);

    // Store outputs
    WorldNormal = N;
    TBN = mat3(T, B, N);
    RawTangent = aTangent;
    TransformID = resolvedTransformID;

    gl_Position = projection * view * worldPos;
}
