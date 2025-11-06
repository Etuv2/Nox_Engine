#version 460 core

layout(location = 0) in vec3 aPos;       
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec4 aTangent; // XYZ = tangent direction, W = handedness

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

    // Normal Matrix (transpose of inverse for non-uniform scaling)
     mat3 normalMatrix = transpose(inverse(mat3(model)));
    
    // CRITICAL FIX: Gram-Schmidt orthogonalization for seamless tangent space
    // Transform normal and tangent to world space
    vec3 N = normalize(normalMatrix * aNormal);
    vec3 T = normalize(normalMatrix * aTangent.xyz);
    
    // Re-orthogonalize tangent with respect to normal (Gram-Schmidt process)
    // This ensures T is perpendicular to N, preventing seams at UV boundaries
    T = normalize(T - dot(T, N) * N);
    
    // Compute bitangent using cross product and handedness from tangent.w
    // The handedness (aTangent.w) determines if we need to flip the bitangent
    vec3 B = cross(N, T) * aTangent.w;
    
    // Ensure bitangent is normalized (cross product of two unit vectors should be unit, but be safe)
    B = normalize(B);

    // Store outputs
    WorldNormal = N;
    TBN = mat3(T, B, N);
    RawTangent = aTangent;

    gl_Position = projection * view * worldPos;
}
