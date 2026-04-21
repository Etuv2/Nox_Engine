#ifndef NOX_SCREEN_SPACE_RECONSTRUCTION_GLSL
#define NOX_SCREEN_SPACE_RECONSTRUCTION_GLSL

const float NOX_FP16_MAX = 65504.0;
const vec3 NOX_LUMA_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);

float Luma(vec3 color) {
    return dot(color, NOX_LUMA_WEIGHTS);
}

bool IsValidLinearDepth(float depth) {
    return depth > 0.0 && depth < NOX_FP16_MAX;
}

vec2 EncodeOctNormal01(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-6);
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy + vec2(1e-6));
    }
    return n.xy * 0.5 + 0.5;
}

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

vec3 ViewPosFromLinearDepth(vec2 uv, float linearDepth, float projScaleX, float projScaleY) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(
        ndc.x * linearDepth / max(projScaleX, 1e-5),
        ndc.y * linearDepth / max(projScaleY, 1e-5),
        -linearDepth
    );
}

float ViewDepthFromDeviceDepth(vec2 uv, float depth01, mat4 invProjection) {
    vec3 viewPos = ReconstructViewPosition(uv, depth01, invProjection);
    return max(-viewPos.z, 1e-4);
}

float QuarterNormalMip(sampler2D normalTex, vec2 fullResolution, ivec2 quarterResolution) {
    int maxMip = max(textureQueryLevels(normalTex) - 1, 0);
    if (maxMip == 0) {
        return 0.0;
    }
    float desiredMip = max(log2(max(fullResolution.x / max(float(quarterResolution.x), 1.0), 1.0)), 0.0);
    return clamp(desiredMip, 0.0, float(maxMip));
}

float QuarterNormalMip(sampler2D normalTex, sampler2D depthQuarterTex) {
    return max(log2(max(float(textureSize(normalTex, 0).x) / max(float(textureSize(depthQuarterTex, 0).x), 1.0), 1.0)), 0.0);
}

float LinearizeDepthFromProjection(float depth01, mat4 projection) {
    float z = depth01 * 2.0 - 1.0;
    float a = projection[2][2];
    float b = projection[3][2];
    float c = projection[2][3];
    return b / (z * c - a);
}

#endif
