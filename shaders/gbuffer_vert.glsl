#version 450 core

layout(location = 0) in vec3 aPos;       
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec4 aTangent;

// Outputs to the fragment shader (geometry pass)
out vec3 WorldPos;
out vec3 WorldNormal;
out vec2 TexCoords;
out mat3 TBN;
out vec4 RawTangent; 

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{
    // Compute world-space position
    vec4 worldPos = model * vec4(aPos, 1.0);
    WorldPos = worldPos.xyz;
    TexCoords = aTexCoords;

    // Normal Matrix
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    // Convert normal & tangent to world space
    vec3 N = normalize(normalMatrix * aNormal);
    vec3 T = normalize(normalMatrix * aTangent.xyz);
    // Rebuild B from (N x T), applying the handedness in aTangent.w
    vec3 B = normalize(cross(N, T) * aTangent.w);

    WorldNormal = N;
    TBN = mat3(T, B, N);
    RawTangent = aTangent;

    gl_Position = projection * view * worldPos;
}
