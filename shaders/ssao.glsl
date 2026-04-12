/*
    Screen-Space Ambient Occlusion (SSAO) Shader
    Half-resolution AO with lightweight temporal stabilization.
*/
#version 460 core
#include "includes/screen_space_reconstruction.glsl"

in vec2 TexCoord;
out float FragColor;

uniform sampler2D gPackedNormalRM;
uniform sampler2D gDepth;
uniform sampler2D noiseTex;
uniform sampler2D historyAO;
uniform vec2 screenSize;

uniform mat4 proj;
uniform mat4 invProj;
uniform mat4 view;

uniform int normalsInWorldSpace = 1;
uniform int historyValid = 0;
uniform vec3 samples[64];
uniform int sampleCount = 32;

uniform float radius = 0.75;
uniform float bias = 0.015;
uniform float intensity = 1.2;
uniform float aoMin = 0.0;
uniform float temporalBlend = 0.12;

void main() {
    vec2 uv = TexCoord;
    float depth = texture(gDepth, uv).r;

    if (depth >= 0.9999) {
        FragColor = 1.0;
        return;
    }

    vec3 normalVS = DecodeSceneNormalVS(texture(gPackedNormalRM, uv).rg, view, normalsInWorldSpace == 1);
    vec3 positionVS = ReconstructViewPosition(uv, depth, invProj);

    if (any(isnan(positionVS)) || any(isinf(positionVS))) {
        FragColor = 1.0;
        return;
    }

    vec3 rand = texture(noiseTex, TexCoord * (screenSize / 4.0)).xyz;
    vec3 tangent = normalize(rand - normalVS * dot(rand, normalVS));
    vec3 bitangent = cross(normalVS, tangent);
    mat3 tbn = mat3(tangent, bitangent, normalVS);

    float occlusion = 0.0;
    float weightSum = 0.0;
    int activeSamples = clamp(sampleCount, 1, 64);

    for (int i = 0; i < activeSamples; ++i) {
        vec3 sampleVS = positionVS + (tbn * samples[i]) * radius;
        vec2 sampleUV = ProjectViewPositionToUV(sampleVS, proj);

        if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
            continue;
        }

        float sampleDepth = texture(gDepth, sampleUV).r;
        if (sampleDepth >= 0.9999) {
            continue;
        }

        vec3 sceneVS = ReconstructViewPosition(sampleUV, sampleDepth, invProj);
        if (any(isnan(sceneVS)) || any(isinf(sceneVS))) {
            continue;
        }

        vec3 direction = normalize(sceneVS - positionVS);
        float ndot = max(dot(normalVS, direction), 0.0);
        if (ndot < 0.01) {
            continue;
        }

        float distanceToSample = length(sceneVS - positionVS);
        float rangeFalloff = 1.0 - smoothstep(0.0, radius, distanceToSample);
        float weight = ndot * rangeFalloff;
        if (weight < 1e-4) {
            continue;
        }

        float depthDiff = sceneVS.z - sampleVS.z;
        if (depthDiff > bias && depthDiff < radius) {
            occlusion += weight;
        }

        weightSum += weight;
    }

    float ao = 1.0;
    if (weightSum > 1e-6) {
        ao = 1.0 - (occlusion / weightSum);
    }

    ao = pow(ao, intensity);
    ao = clamp(ao, aoMin, 1.0);

    if (historyValid != 0) {
        float history = texture(historyAO, uv).r;
        float delta = abs(history - ao);
        float currentWeight = mix(temporalBlend, 0.45, clamp(delta * 3.0, 0.0, 1.0));
        ao = mix(history, ao, currentWeight);
    }

    FragColor = ao;
}
