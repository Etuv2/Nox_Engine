#ifndef NOX_SKINNING_COMMON_GLSL
#define NOX_SKINNING_COMMON_GLSL

uniform bool u_enableSkinning;
uniform mat4 u_boneMatrices[128];

mat4 BuildSkinMatrix(ivec4 boneIDs, vec4 boneWeights, out bool hasValidWeights) {
    float totalWeight = boneWeights.x + boneWeights.y + boneWeights.z + boneWeights.w;
    hasValidWeights = totalWeight > 0.001;
    if (!hasValidWeights) {
        return mat4(1.0);
    }

    vec4 weights = boneWeights / totalWeight;
    int b0 = clamp(boneIDs.x, 0, 127);
    int b1 = clamp(boneIDs.y, 0, 127);
    int b2 = clamp(boneIDs.z, 0, 127);
    int b3 = clamp(boneIDs.w, 0, 127);

    mat4 skinMatrix = u_boneMatrices[b0] * weights.x;
    skinMatrix += u_boneMatrices[b1] * weights.y;
    skinMatrix += u_boneMatrices[b2] * weights.z;
    skinMatrix += u_boneMatrices[b3] * weights.w;
    return skinMatrix;
}

void ApplySkinning(
    ivec4 boneIDs,
    vec4 boneWeights,
    inout vec4 localPos,
    inout vec3 localNormal,
    inout vec3 localTangent
) {
    if (!u_enableSkinning) {
        return;
    }

    bool hasValidWeights;
    mat4 skinMatrix = BuildSkinMatrix(boneIDs, boneWeights, hasValidWeights);
    if (!hasValidWeights) {
        return;
    }

    localPos = skinMatrix * localPos;
    mat3 skinMatrix3 = mat3(skinMatrix);
    float skinDet = determinant(skinMatrix3);
    mat3 skinNormalMatrix = (abs(skinDet) > 1e-8) ? transpose(inverse(skinMatrix3)) : skinMatrix3;
    localNormal = skinNormalMatrix * localNormal;
    localTangent = skinNormalMatrix * localTangent;
}

void ApplySkinning(ivec4 boneIDs, vec4 boneWeights, inout vec4 localPos) {
    if (!u_enableSkinning) {
        return;
    }

    bool hasValidWeights;
    mat4 skinMatrix = BuildSkinMatrix(boneIDs, boneWeights, hasValidWeights);
    if (hasValidWeights) {
        localPos = skinMatrix * localPos;
    }
}

void BuildWorldTBN(
    mat3 normalMatrix,
    vec3 localNormal,
    vec3 localTangent,
    float handedness,
    out vec3 N,
    out vec3 T,
    out vec3 B
) {
    N = normalize(normalMatrix * localNormal);
    T = normalize(normalMatrix * localTangent);
    T = normalize(T - dot(T, N) * N);
    B = normalize(cross(N, T) * handedness);
}

#endif
