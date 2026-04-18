#version 460 core
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D linearDepthQuarter;
layout(binding = 1) uniform sampler2D normalQuarter;
layout(binding = 2) uniform sampler2D radianceTex;

layout(binding = 3, rgba16f) writeonly uniform image2D outIndirectRaw;
layout(binding = 4, rgba16f) writeonly uniform image2D outDirectionalRaw;
layout(binding = 5, rgba16f) writeonly uniform image2D outHorizonDebug;
layout(binding = 6, rgba16f) writeonly uniform image2D outSectorDebug;

uniform vec2 invQuarterSize;
uniform float projScaleX;
uniform float projScaleY;
uniform float maxRadiusVS;
uniform float thicknessVS;
uniform int sliceCount;
uniform int stepsPerSlice;
uniform int sectorCount;
uniform int frameIndex;
uniform float cameraNear;
uniform float cameraFar;

const int MAX_SECTORS = 24;

float Luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ViewPosFromLinearDepth(vec2 uv, float linearDepth) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(
        ndc.x * linearDepth / max(projScaleX, 1e-5),
        ndc.y * linearDepth / max(projScaleY, 1e-5),
        -linearDepth
    );
}

uint pcg(uint x) {
    uint state = x * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Hash01(uvec2 p) {
    uint h = pcg(p.x ^ (p.y * 1597334677u) ^ uint(frameIndex * 31337));
    return float(h) * (1.0 / 4294967296.0);
}

vec2 BuildSliceDirection(float angle) {
    return vec2(cos(angle), sin(angle));
}

uint BuildSectorBits(float thetaCenter, float thetaHalfWidth, int sectors) {
    float halfPi = 1.57079632679;
    float tMin = clamp(thetaCenter - thetaHalfWidth, 0.0, halfPi);
    float tMax = clamp(thetaCenter + thetaHalfWidth, 0.0, halfPi);

    int iMin = int(floor((tMin / halfPi) * float(sectors)));
    int iMax = int(floor((tMax / halfPi) * float(sectors)));
    iMin = clamp(iMin, 0, sectors - 1);
    iMax = clamp(iMax, 0, sectors - 1);

    uint bits = 0u;
    for (int i = iMin; i <= iMax; ++i) {
        bits |= (1u << uint(i));
    }
    return bits;
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outIndirectRaw);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invQuarterSize;
    float centerDepth = textureLod(linearDepthQuarter, uv, 0.0).r;
    if (centerDepth > 65000.0 || centerDepth <= 0.0) {
        imageStore(outIndirectRaw, id, vec4(0.0));
        imageStore(outDirectionalRaw, id, vec4(0.5, 0.5, 1.0, 0.0));
        imageStore(outHorizonDebug, id, vec4(0.0));
        imageStore(outSectorDebug, id, vec4(0.0));
        return;
    }

    vec3 centerPos = ViewPosFromLinearDepth(uv, centerDepth);
    vec3 centerNormal = DecodeOctNormal01(textureLod(normalQuarter, uv, 0.0).rg);
    centerNormal = normalize(centerNormal);

    if (length(centerNormal) < 0.5 || any(isnan(centerNormal))) {
        imageStore(outIndirectRaw, id, vec4(0.0));
        imageStore(outDirectionalRaw, id, vec4(0.5, 0.5, 1.0, 0.0));
        imageStore(outHorizonDebug, id, vec4(0.0));
        imageStore(outSectorDebug, id, vec4(0.0));
        return;
    }

    vec3 viewDir = normalize(-centerPos);
    vec3 tangent = normalize(abs(centerNormal.z) < 0.95 ? cross(centerNormal, vec3(0.0, 0.0, 1.0)) : cross(centerNormal, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = normalize(cross(centerNormal, tangent));

    int sectors = clamp(sectorCount, 8, MAX_SECTORS);
    float jitterBase = Hash01(uvec2(id));

    vec3 indirectAccum = vec3(0.0);
    vec3 directionMoment = vec3(0.0);
    float weightAccum = 0.0;
    float horizonAccum = 0.0;
    float sectorCoverageAccum = 0.0;
    float acceptedSamples = 0.0;

    int maxMip = textureQueryLevels(linearDepthQuarter) - 1;

    for (int slice = 0; slice < sliceCount; ++slice) {
        float azimuth = (6.28318530718 * (float(slice) + jitterBase)) / float(max(sliceCount, 1));
        vec2 sliceDir2 = BuildSliceDirection(azimuth);

        uint visibilityMask = 0u;
        float maxTheta = 0.0;

        for (int step = 0; step < stepsPerSlice; ++step) {
            float t = (float(step) + jitterBase) / float(max(stepsPerSlice, 1));
            float tWarp = max(t * t, 1e-4);
            float radiusVS = maxRadiusVS * tWarp;

            float uvScale = (radiusVS * projScaleY) / max(2.0 * centerDepth, 1e-4);
            vec2 sampleUV = uv + sliceDir2 * uvScale;
            if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
                break;
            }

            float lod = clamp(log2(1.0 + float(step) * 0.8), 0.0, float(maxMip));
            float sampleDepth = textureLod(linearDepthQuarter, sampleUV, lod).r;
            if (sampleDepth > 65000.0 || sampleDepth <= 0.0) {
                continue;
            }

            vec3 samplePos = ViewPosFromLinearDepth(sampleUV, sampleDepth);
            vec3 toSample = samplePos - centerPos;
            float distanceVS = length(toSample);
            if (distanceVS <= 1e-4) {
                continue;
            }

            vec3 rayDir = toSample / distanceVS;
            float nDotRay = max(dot(centerNormal, rayDir), 0.0);
            if (nDotRay <= 0.001) {
                continue;
            }

            float theta = acos(clamp(nDotRay, 0.0, 1.0));
            float thetaHalfWidth = atan(thicknessVS / max(distanceVS, 1e-4));
            uint sectorBits = BuildSectorBits(theta, thetaHalfWidth, sectors);

            uint newlyVisibleBits = sectorBits & (~visibilityMask);
            float visibleFrac = float(bitCount(newlyVisibleBits)) / float(max(bitCount(sectorBits), 1));
            visibilityMask |= sectorBits;

            if (visibleFrac <= 0.0) {
                continue;
            }

            float emitterFacing = max(dot(rayDir, -viewDir), 0.0);
            float attenuation = 1.0 / (1.0 + 0.22 * distanceVS * distanceVS);
            vec3 sampleRadiance = max(textureLod(radianceTex, sampleUV, lod).rgb, vec3(0.0));

            float sampleWeight = visibleFrac * nDotRay * attenuation * mix(0.65, 1.0, emitterFacing);
            vec3 contribution = sampleRadiance * sampleWeight;

            indirectAccum += contribution;
            directionMoment += contribution * rayDir;
            weightAccum += sampleWeight;
            acceptedSamples += 1.0;
            maxTheta = max(maxTheta, theta + thetaHalfWidth);
        }

        sectorCoverageAccum += float(bitCount(visibilityMask)) / float(sectors);
        horizonAccum += maxTheta / 1.57079632679;
    }

    vec3 indirect = vec3(0.0);
    if (weightAccum > 1e-5) {
        indirect = indirectAccum / weightAccum;
    }

    // Keep energy bounded before temporal accumulation.
    indirect = clamp(indirect, vec3(0.0), vec3(6.0));

    vec3 dominantDir = normalize(directionMoment + centerNormal * 1e-5);
    float anisotropy = clamp(length(directionMoment) / max(Luma(indirect) + 1e-4, 1e-4), 0.0, 1.0);
    float confidence = clamp((acceptedSamples / float(max(sliceCount * stepsPerSlice, 1))) * (0.35 + 0.65 * anisotropy), 0.0, 1.0);

    vec3 dirEncoded = dominantDir * 0.5 + 0.5;

    imageStore(outIndirectRaw, id, vec4(indirect, confidence));
    imageStore(outDirectionalRaw, id, vec4(dirEncoded, anisotropy));

    float avgHorizon = horizonAccum / float(max(sliceCount, 1));
    float avgCoverage = sectorCoverageAccum / float(max(sliceCount, 1));
    imageStore(outHorizonDebug, id, vec4(avgHorizon, avgCoverage, confidence, 1.0));

    float sampleRatio = acceptedSamples / float(max(sliceCount * stepsPerSlice, 1));
    imageStore(outSectorDebug, id, vec4(avgCoverage, sampleRatio, anisotropy, 1.0));
}
