#version 460 core
#include "includes/pbr_common.glsl"
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

// C++ binding footprint:
// 0: quarter-res linear view-space depth, positive depth = -viewPos.z
// 1: quarter-res canonical view-space normals, oct-encoded in RG
// 2: quarter-res bounceable radiance
layout(binding = 0) uniform sampler2D linearDepthQuarter;
layout(binding = 1) uniform sampler2D normalFromDepthTex;
layout(binding = 2) uniform sampler2D radianceTex;

// 3: RGB indirect irradiance, A = scalar visibility / AO
// 4: L00 + L1x/L1y/L1z luminance SH, packed as vec4(sh0, sh1x, sh1y, sh1z)
// 5: coverage summary for debug-present routing
// 6: interval / sector debug summary for debug-present routing
layout(binding = 3, rgba16f) writeonly uniform image2D outIndirectRaw;
layout(binding = 4, rgba16f) writeonly uniform image2D outDirectionalRaw;
layout(binding = 5, rgba16f) writeonly uniform image2D outHorizonDebug;
layout(binding = 6, rgba16f) writeonly uniform image2D outSectorDebug;

uniform vec2 invQuarterSize;
uniform vec2 fullResolution;
uniform float projScaleX;
uniform float projScaleY;
uniform float rayLength;     // Gather radius in view-space units.
uniform float thicknessVS;   // Constant thickness in view-space units.
uniform int rayCount;        // Slice count. Default target: 4.
uniform int stepCount;       // Samples per slice. Default target: 4.
uniform int frameIndex;

const float HALF_PI = 1.57079632679;
const float INV_HALF_PI = 0.63661977237;
const float SECTOR_COUNT_F = 32.0;
const float INV_SECTOR_COUNT = 1.0 / 32.0;
const float INV_SECTOR_ANGLE = SECTOR_COUNT_F / HALF_PI;
const uint FULL_MASK_32 = 0xffffffffu;

vec3 SafeNormalizeStrict(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 > 1e-12) {
        return v * inversesqrt(len2);
    }
    return fallback;
}

float InterleavedGradientNoise(vec2 pixel, float frameSeed) {
    float seed = dot(pixel, vec2(0.06711056, 0.00583715)) + frameSeed * 0.754877666;
    return fract(52.9829189 * fract(seed));
}

vec3 BuildTangent(vec3 normal, vec3 cameraVec) {
    vec3 projectedCamera = cameraVec - normal * dot(cameraVec, normal);
    if (dot(projectedCamera, projectedCamera) > 1e-8) {
        return normalize(projectedCamera);
    }

    vec3 axis = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return normalize(cross(axis, normal));
}

vec3 BuildBitangent(vec3 normal, vec3 tangent) {
    return normalize(cross(normal, tangent));
}

vec3 ProjectOntoPlane(vec3 v, vec3 planeNormal) {
    return v - planeNormal * dot(v, planeNormal);
}

vec4 SHBasisL1(vec3 dir) {
    // Compact luminance SH packing: L00, L1x, L1y, L1z.
    return vec4(0.282095, 0.488603 * dir.x, 0.488603 * dir.y, 0.488603 * dir.z);
}

vec3 ShapeGatherRadiance(vec3 radiance) {
    radiance = max(radiance, vec3(0.0));
    float luma = Luma(radiance);
    if (luma <= 1e-5) {
        return vec3(0.0);
    }

    float lowLift = mix(1.30, 1.0, smoothstep(0.035, 0.35, luma));
    float softKnee = 1.0 / (1.0 + max(luma - 1.15, 0.0) * 0.45);
    return radiance * (lowLift * softKnee);
}

int ClampSectorIndex(float theta) {
    return int(clamp(floor(theta * INV_SECTOR_ANGLE), 0.0, 31.0));
}

uint SectorMaskInclusive(int startSector, int endSector) {
    if (endSector < startSector) {
        return 0u;
    }

    int startClamped = clamp(startSector, 0, 31);
    int endClamped = clamp(endSector, 0, 31);
    if (endClamped < startClamped) {
        return 0u;
    }

    uint count = uint(endClamped - startClamped + 1);
    if (count >= 32u) {
        return FULL_MASK_32;
    }
    return ((1u << count) - 1u) << uint(startClamped);
}

void main() {
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    ivec2 outSize = imageSize(outIndirectRaw);
    if (any(greaterThanEqual(id, outSize))) {
        return;
    }

    vec2 uv = (vec2(id) + 0.5) * invQuarterSize;
    float centerDepth = texelFetch(linearDepthQuarter, id, 0).r;
    if (!IsValidLinearDepth(centerDepth)) {
        imageStore(outIndirectRaw, id, vec4(0.0));
        imageStore(outDirectionalRaw, id, vec4(0.0));
        imageStore(outHorizonDebug, id, vec4(0.0));
        imageStore(outSectorDebug, id, vec4(0.0));
        return;
    }

    vec3 centerPos = ViewPosFromLinearDepth(uv, centerDepth, projScaleX, projScaleY);
    vec3 cameraVec = SafeNormalizeStrict(-centerPos, vec3(0.0, 0.0, 1.0));

    vec3 centerNormal = DecodeOctNormal01(textureLod(normalFromDepthTex, uv, 0.0).rg);
    centerNormal = SafeNormalizeStrict(centerNormal, vec3(0.0, 0.0, 1.0));
    if (dot(centerNormal, cameraVec) < 0.0) {
        centerNormal = -centerNormal;
    }

    if (any(isnan(centerNormal)) || any(isnan(centerPos))) {
        imageStore(outIndirectRaw, id, vec4(0.0));
        imageStore(outDirectionalRaw, id, vec4(0.0));
        imageStore(outHorizonDebug, id, vec4(0.0));
        imageStore(outSectorDebug, id, vec4(0.0));
        return;
    }

    int sliceCount = max(rayCount, 1);
    int samplesPerSlice = max(stepCount, 1);
    int depthMipMax = max(textureQueryLevels(linearDepthQuarter) - 1, 0);
    int radianceMipMax = max(textureQueryLevels(radianceTex) - 1, 0);
    int normalMipMax = max(textureQueryLevels(normalFromDepthTex) - 1, 0);

    vec3 tangent = BuildTangent(centerNormal, cameraVec);
    vec3 bitangent = BuildBitangent(centerNormal, tangent);

    float pixelSeed = InterleavedGradientNoise(vec2(id), float(frameIndex));
    float frameSeed = fract(float(frameIndex) * 0.754877666);

    vec3 indirectAccum = vec3(0.0);
    vec4 shAccum = vec4(0.0);
    float visibilityAccum = 0.0;
    float coverageAccum = 0.0;
    float newSectorAccum = 0.0;
    float intervalAccum = 0.0;

    vec2 firstSliceInterval = vec2(0.0);
    float firstSliceCoverage = 0.0;
    float firstSliceNewSector = 0.0;
    bool firstSliceCaptured = false;

    float thicknessHalf = max(thicknessVS * 0.5, 1e-4);
    float invSliceCount = 1.0 / float(sliceCount);
    float invSamplesPerSlice = 1.0 / float(samplesPerSlice);
    vec2 quarterSize = vec2(textureSize(linearDepthQuarter, 0));
    float projectedPixelsPerViewUnit =
        0.5 * max(projScaleX * quarterSize.x, projScaleY * quarterSize.y) / max(centerDepth, 1e-4);
    float maxRadiusPx = clamp(
        max(rayLength, 0.05) * projectedPixelsPerViewUnit,
        2.0,
        0.5 * max(quarterSize.x, quarterSize.y)
    );

    for (int slice = 0; slice < sliceCount; ++slice) {
        float sliceJitter = InterleavedGradientNoise(vec2(id) + vec2(float(slice), 3.0), float(frameIndex));
        float sliceAngle = TAU * ((float(slice) + sliceJitter + pixelSeed) * invSliceCount);

        vec3 sliceDirVS = SafeNormalizeStrict(
            tangent * cos(sliceAngle) + bitangent * sin(sliceAngle),
            tangent
        );
        vec3 slicePlaneNormal = SafeNormalizeStrict(
            cross(sliceDirVS, cameraVec),
            cross(sliceDirVS, tangent)
        );
        vec3 slicePerpVS = SafeNormalizeStrict(cross(slicePlaneNormal, sliceDirVS), bitangent);
        vec2 sliceDirUV = SafeNormalizeStrict(
            vec3(sliceDirVS.x * projScaleX, sliceDirVS.y * projScaleY, 0.0),
            vec3(1.0, 0.0, 0.0)
        ).xy;
        vec2 slicePerpUV = SafeNormalizeStrict(
            vec3(slicePerpVS.x * projScaleX, slicePerpVS.y * projScaleY, 0.0),
            vec3(0.0, 1.0, 0.0)
        ).xy;
        vec3 projectedNormal = SafeNormalizeStrict(
            ProjectOntoPlane(centerNormal, slicePlaneNormal),
            centerNormal
        );

        uint coveredMask = 0u;
        vec3 sliceIndirect = vec3(0.0);
        vec4 sliceSh = vec4(0.0);
        int sliceNewSectorCount = 0;
        int sliceIntervalCount = 0;

        vec2 sliceIntervalBounds = vec2(1.0, 0.0);

        for (int sampleIndex = 0; sampleIndex < samplesPerSlice; ++sampleIndex) {
            float sampleJitter = InterleavedGradientNoise(
                vec2(id) + vec2(float(slice) * 17.0, float(sampleIndex) * 7.0),
                float(frameIndex) + 13.0 * frameSeed
            );

            float sampleT = (float(sampleIndex) + 1.0 + sampleJitter) / float(samplesPerSlice + 1);
            sampleT = sqrt(clamp(sampleT, 0.0, 1.0));
            float sampleRadiusPx = maxRadiusPx * sampleT;
            float laneJitter = (InterleavedGradientNoise(
                vec2(id) + vec2(float(slice) * 31.0, float(sampleIndex) * 11.0),
                float(frameIndex) + 5.0
            ) - 0.5) * 2.0;
            float crossOffsetPx = laneJitter * mix(0.35, 0.95, sampleT);
            vec2 sampleUV = uv
                + sliceDirUV * (sampleRadiusPx * invQuarterSize)
                + slicePerpUV * (crossOffsetPx * invQuarterSize);
            if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) {
                break;
            }

            float baseLod = clamp(log2(sampleRadiusPx + 1.0) - 1.0, 0.0, float(max(max(depthMipMax, radianceMipMax), normalMipMax)));
            float depthLod = clamp(baseLod, 0.0, float(depthMipMax));
            float radianceLod = clamp(baseLod, 0.0, float(radianceMipMax));
            float normalLod = clamp(baseLod, 0.0, float(normalMipMax));

            int depthMip = int(clamp(floor(depthLod + 0.5), 0.0, float(depthMipMax)));
            ivec2 depthSize = textureSize(linearDepthQuarter, depthMip);
            ivec2 depthCoord = clamp(ivec2(sampleUV * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
            float sampleDepth = texelFetch(linearDepthQuarter, depthCoord, depthMip).r;
            if (!IsValidLinearDepth(sampleDepth)) {
                continue;
            }

            vec3 samplePos = ViewPosFromLinearDepth(sampleUV, sampleDepth, projScaleX, projScaleY);
            vec3 deltaVS = samplePos - centerPos;
            float dist = length(deltaVS);
            if (dist <= 1e-6) {
                continue;
            }

            vec3 sampleDir = deltaVS / dist;
            float planarDist = max(abs(dot(deltaVS, sliceDirVS)), 1e-5);
            vec3 sampleDirPlane = SafeNormalizeStrict(
                ProjectOntoPlane(sampleDir, slicePlaneNormal),
                sampleDir
            );
            float sampleElevation = asin(clamp(dot(sampleDirPlane, projectedNormal), -1.0, 1.0));
            float thicknessAngle = atan(thicknessHalf, planarDist);

            float thetaMin = sampleElevation - thicknessAngle;
            float thetaMax = sampleElevation + thicknessAngle;
            thetaMin = clamp(thetaMin, 0.0, HALF_PI);
            thetaMax = clamp(thetaMax, 0.0, HALF_PI);
            if (thetaMax <= 0.0) {
                continue;
            }

            int sectorMin = ClampSectorIndex(thetaMin);
            int sectorMax = ClampSectorIndex(thetaMax);
            uint sampleMask = SectorMaskInclusive(sectorMin, sectorMax);
            if (sampleMask == 0u) {
                continue;
            }

            uint newMask = sampleMask & ~coveredMask;
            int newSectorCount = bitCount(newMask);
            if (newSectorCount <= 0) {
                coveredMask |= sampleMask;
                continue;
            }

            coveredMask |= sampleMask;
            sliceNewSectorCount += newSectorCount;
            sliceIntervalCount += bitCount(sampleMask);
            sliceIntervalBounds = vec2(
                min(sliceIntervalBounds.x, thetaMin * INV_HALF_PI),
                max(sliceIntervalBounds.y, thetaMax * INV_HALF_PI)
            );

            float sectorWeight = float(newSectorCount) * INV_SECTOR_COUNT;
            float cosineWeight = max(dot(projectedNormal, sampleDirPlane), 0.0);
            float normalizedDist = clamp(dist / max(rayLength, 0.25), 0.0, 4.0);
            float distanceWeight = exp(-normalizedDist * 0.38);
            float farFieldBoost = mix(0.90, 1.90, clamp(sampleT, 0.0, 1.0));
            float transport = sectorWeight * mix(0.45, 1.0, cosineWeight) * distanceWeight * farFieldBoost * 3.15;
            if (transport <= 1e-6) {
                continue;
            }

            vec3 sampleNormal = DecodeOctNormal01(textureLod(normalFromDepthTex, sampleUV, normalLod).rg);
            sampleNormal = SafeNormalizeStrict(sampleNormal, centerNormal);
            if (dot(sampleNormal, -sampleDir) < 0.0) {
                sampleNormal = -sampleNormal;
            }

            float surfaceFacing = max(dot(sampleNormal, -sampleDir), 0.0);
            transport *= mix(0.50, 1.0, surfaceFacing);
            if (transport <= 1e-6) {
                continue;
            }

            vec3 sampleRadiance = ShapeGatherRadiance(textureLod(radianceTex, sampleUV, radianceLod).rgb);
            float sampleLuma = Luma(sampleRadiance);

            sliceIndirect += sampleRadiance * transport;
            sliceSh += SHBasisL1(sampleDir) * (sampleLuma * transport);
        }

        float sliceCoverage = clamp(float(bitCount(coveredMask)) * INV_SECTOR_COUNT, 0.0, 1.0);
        float sliceVisibility = 1.0 - sliceCoverage;
        float sliceNewRatio = clamp(float(sliceNewSectorCount) * INV_SECTOR_COUNT, 0.0, 1.0);
        float sliceIntervalRatio = clamp(float(sliceIntervalCount) * INV_SECTOR_COUNT * invSamplesPerSlice, 0.0, 1.0);

        indirectAccum += sliceIndirect;
        shAccum += sliceSh;
        visibilityAccum += sliceVisibility;
        coverageAccum += sliceCoverage;
        newSectorAccum += sliceNewRatio;
        intervalAccum += sliceIntervalRatio;

        if (!firstSliceCaptured) {
            firstSliceInterval = sliceCoverage > 0.0 ? sliceIntervalBounds : vec2(0.0);
            firstSliceCoverage = sliceCoverage;
            firstSliceNewSector = sliceNewRatio;
            firstSliceCaptured = true;
        }
    }

    float invSliceCountSafe = 1.0 / float(max(sliceCount, 1));
    vec3 indirectRGB = indirectAccum * invSliceCountSafe;
    vec4 shOut = shAccum * invSliceCountSafe;
    float visibility = clamp(visibilityAccum * invSliceCountSafe, 0.0, 1.0);
    float coverage = clamp(coverageAccum * invSliceCountSafe, 0.0, 1.0);
    float newSector = clamp(newSectorAccum * invSliceCountSafe, 0.0, 1.0);
    float intervalWidth = clamp(intervalAccum * invSliceCountSafe, 0.0, 1.0);

    imageStore(outIndirectRaw, id, vec4(indirectRGB, visibility));
    imageStore(outDirectionalRaw, id, shOut);
    imageStore(outHorizonDebug, id, vec4(coverage, visibility, newSector, intervalWidth));
    imageStore(outSectorDebug, id, vec4(firstSliceInterval, firstSliceCoverage, firstSliceNewSector));
}
