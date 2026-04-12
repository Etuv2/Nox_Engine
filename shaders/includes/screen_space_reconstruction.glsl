vec3 DecodeOctNormal01(vec2 encoded) {
    vec2 e = encoded * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        vec2 signNotZero = mix(vec2(1.0), sign(e), step(vec2(0.0001), abs(e)));
        n.xy = (1.0 - abs(n.yx)) * signNotZero;
    }
    return normalize(n);
}

vec3 DecodeSceneNormalVS(vec2 encoded, mat4 viewMatrix, bool normalsInWorldSpace) {
    vec3 normal = DecodeOctNormal01(encoded);
    return normalsInWorldSpace ? normalize(mat3(viewMatrix) * normal) : normal;
}

vec3 ReconstructViewPosition(vec2 uv, float depth01, mat4 invProjection) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth01 * 2.0 - 1.0, 1.0);
    vec4 viewPosition = invProjection * ndc;
    return viewPosition.xyz / max(abs(viewPosition.w), 1e-6);
}

vec2 ProjectViewPositionToUV(vec3 viewPosition, mat4 projection) {
    vec4 clipPosition = projection * vec4(viewPosition, 1.0);
    vec2 ndc = clipPosition.xy / max(abs(clipPosition.w), 1e-6);
    return ndc * 0.5 + 0.5;
}

float LinearizeDepthFromProjection(float depth01, mat4 projection) {
    float z = depth01 * 2.0 - 1.0;
    float a = projection[2][2];
    float b = projection[3][2];
    float c = projection[2][3];
    return b / (z * c - a);
}
