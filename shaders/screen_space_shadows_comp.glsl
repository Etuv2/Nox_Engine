#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(binding = 0) uniform sampler2D uDepth;
layout(binding = 1) uniform sampler2D uHistory;
layout(r8, binding = 2) writeonly uniform image2D uOutShadow;

layout(std140, binding = 3) uniform Params {
    vec2 invScreen;
    vec2 screenSize;

    vec3 lightDirVS;
    float maxRayLength;
    int numSteps;
    float thickness;
    float edgeThreshold;
    float jitterStrength;
    int enableJitter;
    int debugMode;
    int _pad0;
    int _pad1;

    mat4 invProj;
    mat4 proj;

    uint frameIndex;
    uint historyValid;
    uint enableTemporalAccumulation;
    float temporalBlend;

    int enableBilateralBlur;
    int blurRadius;
    float depthSensitivity;
    float _pad5;
};

float Hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

vec2 SubpixelDither(ivec2 pix) {
    float n = Hash21(vec2(pix) * 37.0) - 0.5;
    return n * 0.35 * invScreen;
}

float SampleDepthEdgeAware(vec2 uv, float centerDepth, float edgeThr) {
    ivec2 size = textureSize(uDepth, 0);
    vec2 st = uv * vec2(size) - 0.5;
    ivec2 ij = ivec2(floor(st));
    vec2 f = fract(st);

    float d00 = texelFetch(uDepth, clamp(ij, ivec2(0), size - 1), 0).r;
    float d10 = texelFetch(uDepth, clamp(ij + ivec2(1, 0), ivec2(0), size - 1), 0).r;
    float d01 = texelFetch(uDepth, clamp(ij + ivec2(0, 1), ivec2(0), size - 1), 0).r;
    float d11 = texelFetch(uDepth, clamp(ij + ivec2(1, 1), ivec2(0), size - 1), 0).r;

    if (abs(d00 - d10) > edgeThr || abs(d01 - d11) > edgeThr) {
        return centerDepth;
    }

    float dx0 = mix(d00, d10, f.x);
    float dx1 = mix(d01, d11, f.x);
    return mix(dx0, dx1, f.y);
}

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    if (pix.x >= int(screenSize.x) || pix.y >= int(screenSize.y)) {
        return;
    }

    vec2 uv = (vec2(pix) + 0.5) * invScreen;
    float depthCenter = texture(uDepth, uv).r;
    if (depthCenter >= 0.9999) {
        imageStore(uOutShadow, pix, vec4(1.0));
        return;
    }

    vec3 posVS = ReconstructViewPosition(uv, depthCenter, invProj);
    vec2 jitter = vec2(0.0);
    if (enableJitter != 0) {
        float h = Hash21(vec2(pix) + float(frameIndex));
        float a = h * 6.2831853;
        jitter = vec2(cos(a), sin(a)) * jitterStrength;
    }

    vec2 dither = SubpixelDither(pix);
    vec3 dirVS = normalize(lightDirVS);
    float stepLenVS = maxRayLength / float(max(1, numSteps));
    vec3 stepVS = dirVS * stepLenVS;

    float visibility = 1.0;
    vec3 sampleVS = posVS + dirVS * (stepLenVS * 0.5);
    bool hit = false;

    for (int i = 0; i < numSteps; ++i) {
        vec2 sampleUV = ProjectViewPositionToUV(sampleVS, proj) + (jitter + dither) * invScreen;
        if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
            break;
        }

        float sampleDepth = SampleDepthEdgeAware(sampleUV, depthCenter, edgeThreshold);
        if (sampleDepth < 0.9999) {
            float zSmp = LinearizeDepthFromProjection(sampleDepth, proj);
            if ((zSmp > sampleVS.z - thickness) && (zSmp < sampleVS.z + thickness)) {
                hit = true;
                visibility = 0.0;
                break;
            }
        }

        sampleVS += stepVS;
    }

    if (enableTemporalAccumulation != 0 && historyValid != 0) {
        float history = texture(uHistory, uv).r;
        float current = visibility;
        float delta = abs(history - current);
        float currentWeight = mix(temporalBlend, 0.5, clamp(delta * 2.5, 0.0, 1.0));
        visibility = mix(history, current, currentWeight);
    }

    if (debugMode == 1) {
        visibility = hit ? 0.0 : 1.0;
    } else if (debugMode == 2) {
        visibility = clamp(abs(posVS.z) / maxRayLength, 0.0, 1.0);
    }

    visibility = clamp(visibility, 0.0, 1.0);
    imageStore(uOutShadow, pix, vec4(visibility, visibility, visibility, 1.0));
}
