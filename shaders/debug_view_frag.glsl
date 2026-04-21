#version 460 core
#include "includes/screen_space_reconstruction.glsl"
layout(location = 0) out vec4 FragColor;

in vec2 TexCoord;

uniform sampler2D uAlbedo;
uniform sampler2D uNormalPacked;
uniform usampler2D uMaterialID;
uniform usampler2D uTransformID;
uniform sampler2D uDepth;
uniform sampler2DArray uShadowArray;
uniform int uMode;          // 1=albedo,2=normal,3=depth,4=shadow,5=materialId,6=transformId
uniform int uShadowLayer;

vec3 hsv2rgb(vec3 hsv) {
    vec3 p = abs(fract(hsv.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return hsv.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), hsv.y);
}

vec3 StableDebugIDColor(uint id) {
    if (id == 0u) {
        return vec3(0.0);
    }

    uint hash = id;
    hash ^= hash >> 16u;
    hash *= 2246822519u;
    hash ^= hash >> 13u;
    hash *= 3266489917u;
    hash ^= hash >> 16u;

    float hue = fract(float(hash) * 0.61803398875);
    float saturation = 0.65 + 0.25 * float((hash >> 8u) & 255u) / 255.0;
    float value = 0.70 + 0.25 * float((hash >> 16u) & 255u) / 255.0;
    return hsv2rgb(vec3(hue, saturation, value));
}

void main() {
    vec2 uv = TexCoord;

    if (uMode == 1) {
        FragColor = vec4(texture(uAlbedo, uv).rgb, 1.0);
        return;
    }

    if (uMode == 2) {
        vec2 oct = texture(uNormalPacked, uv).rg;
        vec3 n = DecodeOctNormal01(oct);
        FragColor = vec4(n * 0.5 + 0.5, 1.0);
        return;
    }

    if (uMode == 3) {
        float d = texture(uDepth, uv).r;
        FragColor = vec4(vec3(1.0 - d), 1.0);
        return;
    }

    if (uMode == 4) {
        float s = texture(uShadowArray, vec3(uv, float(uShadowLayer))).r;
        FragColor = vec4(vec3(s), 1.0);
        return;
    }

    if (uMode == 5) {
        uint id = texture(uMaterialID, uv).r;
        FragColor = vec4(StableDebugIDColor(id), 1.0);
        return;
    }

    if (uMode == 6) {
        uint id = texture(uTransformID, uv).r;
        FragColor = vec4(StableDebugIDColor(id), 1.0);
        return;
    }

    FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
