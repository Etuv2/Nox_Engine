#version 460 core
#include "includes/pbr_common.glsl"
#include "includes/screen_space_reconstruction.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

// Spec: docs/ssgi_v2_visibility_bitmask_spec.md
// Screen-space indirect diffuse with a visibility bitmask (Therrien et al. 2023, on top of the
// GTAO slice parameterization of Jimenez et al. 2016).
//
// Each slice is a plane through the view vector V. Inside a slice, directions are measured by
// their angle theta from V; the receiver's normal projects to angle gamma with length |pn|.
// Irradiance over the normal hemisphere is
//     E = integral_0^pi dphi  integral L(theta) |pn| cos(theta - gamma) |sin(theta)| dtheta
// so with slices spread uniformly over phi in [0, pi):  E ~= pi * mean_slices( |pn| * sum L * W ).
// The hemisphere [gamma - pi/2, gamma + pi/2] of every slice is split into 32 sectors. Every
// on-screen sample occludes the sectors between its front face and its front face pushed back by
// the thickness; sectors hit for the first time receive that sample's outgoing radiance.
// W is the exact cosine- and Jacobian-weighted size of each sector, so a fully occluded
// hemisphere of radiance L returns E = pi * L, and the RGB output is irradiance (W/m^2 units of
// the radiance texture), which the lighting pass turns into outgoing radiance with albedo / pi.
// Sectors that stay open carry no screen-space light; their cosine-weighted share is written to
// alpha as visibility so the far field (IBL, LPV, surfel GI) can fill them.

// C++ binding footprint:
// 0: quarter-res linear view-space depth (min-depth mip chain), positive depth = -viewPos.z
// 1: quarter-res view-space normals, oct-encoded in RG
// 2: quarter-res bounceable radiance (mip chain)
layout(binding = 0) uniform sampler2D linearDepthQuarter;
layout(binding = 1) uniform sampler2D normalFromDepthTex;
layout(binding = 2) uniform sampler2D radianceTex;

// 3: RGB indirect irradiance, A = cosine-weighted visibility of the hemisphere
// 4: L00 + L1x/L1y/L1z luminance SH of the gathered irradiance, packed as vec4(sh0, sh1x, sh1y, sh1z)
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
uniform float thicknessVS;   // Assumed thickness of on-screen surfaces in view-space units.
uniform int rayCount;        // Slice count; every slice marches both screen directions.
uniform int stepCount;       // Samples per slice direction.
uniform int frameIndex;

const int SECTOR_COUNT = 32;
const float SECTOR_COUNT_F = 32.0;
const float HALF_PI = 1.57079632679;

vec3 SafeNormalizeStrict(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 > 1e-12) {
        return v * inversesqrt(len2);
    }
    return fallback;
}

float InterleavedGradientNoise(vec2 pixel, float frameSeed) {
    vec2 p = pixel + frameSeed * 5.588238;
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec4 SHBasisL1(vec3 dir) {
    // Compact luminance SH packing: L00, L1x, L1y, L1z.
    return vec4(0.282095, 0.488603 * dir.x, 0.488603 * dir.y, 0.488603 * dir.z);
}

// H(theta) = integral_0^theta cos(t - gamma) |sin(t)| dt, valid for theta in (-pi, pi).
float SliceWeightAntiderivative(float theta, float gamma, float cosGamma, float sinGamma) {
    float value = 0.25 * (cosGamma - cos(2.0 * theta - gamma)) + 0.5 * theta * sinGamma;
    return theta < 0.0 ? -value : value;
}

// Sector i spans theta in [gamma - pi/2 + i*pi/32, gamma - pi/2 + (i+1)*pi/32].
float SectorWeight(int sector, float gamma, float cosGamma, float sinGamma) {
    float theta0 = gamma - HALF_PI + float(sector) * (PI / SECTOR_COUNT_F);
    float theta1 = theta0 + PI / SECTOR_COUNT_F;
    return max(SliceWeightAntiderivative(theta1, gamma, cosGamma, sinGamma)
        - SliceWeightAntiderivative(theta0, gamma, cosGamma, sinGamma), 0.0);
}

float MaskWeight(uint mask, float gamma, float cosGamma, float sinGamma) {
    float weight = 0.0;
    while (mask != 0u) {
        int sector = findLSB(mask);
        weight += SectorWeight(sector, gamma, cosGamma, sinGamma);
        mask &= mask - 1u;
    }
    return weight;
}

// Sectors covered by the normalized interval [t0, t1] of the hemisphere (0 = gamma - pi/2).
uint SectorMaskFromInterval(float t0, float t1) {
    t0 = clamp(t0, 0.0, 1.0);
    t1 = clamp(t1, 0.0, 1.0);
    if (t1 <= t0) {
        return 0u;
    }
    int startSector = min(int(t0 * SECTOR_COUNT_F), SECTOR_COUNT - 1);
    int endSector = clamp(int(ceil(t1 * SECTOR_COUNT_F)), startSector + 1, SECTOR_COUNT);
    int count = endSector - startSector;
    uint bits = count >= SECTOR_COUNT ? 0xffffffffu : ((1u << uint(count)) - 1u);
    return bits << uint(startSector);
}

void WriteEmpty(ivec2 id) {
    imageStore(outIndirectRaw, id, vec4(0.0, 0.0, 0.0, 1.0));
    imageStore(outDirectionalRaw, id, vec4(0.0));
    imageStore(outHorizonDebug, id, vec4(0.0, 1.0, 0.0, 0.0));
    imageStore(outSectorDebug, id, vec4(0.0));
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
        WriteEmpty(id);
        return;
    }

    vec3 centerPos = ViewPosFromLinearDepth(uv, centerDepth, projScaleX, projScaleY);
    vec3 viewVec = SafeNormalizeStrict(-centerPos, vec3(0.0, 0.0, 1.0));

    vec3 centerNormal = SafeNormalizeStrict(DecodeOctNormal01(texelFetch(normalFromDepthTex, id, 0).rg), viewVec);
    if (dot(centerNormal, viewVec) < 0.0) {
        centerNormal = -centerNormal;
    }
    if (any(isnan(centerNormal)) || any(isnan(centerPos))) {
        WriteEmpty(id);
        return;
    }

    int sliceCount = max(rayCount, 1);
    int samplesPerSide = max(stepCount, 1);
    int depthMipMax = max(textureQueryLevels(linearDepthQuarter) - 1, 0);
    int radianceMipMax = max(textureQueryLevels(radianceTex) - 1, 0);
    vec2 quarterSize = vec2(outSize);

    // Screen-space radius of the gather: rayLength at the receiver's depth.
    float pixelsPerViewUnit = 0.5 * projScaleY * quarterSize.y / max(centerDepth, 1e-4);
    float maxRadiusPx = clamp(max(rayLength, 0.05) * pixelsPerViewUnit, 2.0, length(quarterSize));

    float sliceNoise = InterleavedGradientNoise(vec2(id), float(frameIndex));
    float stepNoise = InterleavedGradientNoise(vec2(id) + vec2(37.0, 17.0), float(frameIndex));
    float thickness = max(thicknessVS, 1e-3);

    vec3 irradianceAccum = vec3(0.0);
    vec4 shAccum = vec4(0.0);
    float visibilityAccum = 0.0;
    float coverageAccum = 0.0;
    vec4 firstSliceDebug = vec4(0.0);

    for (int slice = 0; slice < sliceCount; ++slice) {
        float phi = (float(slice) + sliceNoise) * (PI / float(sliceCount));
        vec2 dirScreen = vec2(cos(phi), sin(phi));

        // Slice plane through V containing the screen direction.
        vec3 dirVS = vec3(dirScreen, 0.0);
        vec3 orthoDir = SafeNormalizeStrict(dirVS - viewVec * dot(dirVS, viewVec), vec3(1.0, 0.0, 0.0));
        vec3 sliceAxis = SafeNormalizeStrict(cross(orthoDir, viewVec), vec3(0.0, 0.0, 1.0));
        vec3 projectedNormal = centerNormal - sliceAxis * dot(centerNormal, sliceAxis);
        float projectedNormalLen = length(projectedNormal);
        if (projectedNormalLen < 1e-4) {
            continue;
        }
        float cosN = clamp(dot(projectedNormal, viewVec) / projectedNormalLen, -1.0, 1.0);
        float gamma = sign(dot(projectedNormal, orthoDir)) * acos(cosN);
        float cosGamma = cos(gamma);
        float sinGamma = sin(gamma);

        // A view-space direction with z = 0 projects to the same pixel-space direction (the
        // projection's aspect ratio cancels against the render target's), so the slice plane
        // span(V, dirVS) is exactly the set of points on this screen line.
        uint occludedMask = 0u;
        vec3 sliceIrradiance = vec3(0.0);
        vec4 sliceSh = vec4(0.0);

        for (int side = 0; side < 2; ++side) {
            float sideSign = side == 0 ? 1.0 : -1.0;
            bool hasPrevious = false;
            vec3 previousPos = vec3(0.0);
            vec3 previousNormal = vec3(0.0);
            vec3 previousRadiance = vec3(0.0);
            float previousThetaFront = 0.0;

            for (int sampleIndex = 0; sampleIndex < samplesPerSide; ++sampleIndex) {
                // Quadratic spacing: dense near the receiver where contact bounce dominates.
                float t = (float(sampleIndex) + fract(stepNoise + 0.618034 * float(side))) / float(samplesPerSide);
                float radiusPx = max(maxRadiusPx * t * t, float(sampleIndex) + 1.0);
                if (radiusPx > maxRadiusPx) {
                    break;
                }
                vec2 samplePx = vec2(id) + 0.5 + sideSign * dirScreen * radiusPx;
                vec2 sampleUV = samplePx * invQuarterSize;
                if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThanEqual(sampleUV, vec2(1.0)))) {
                    break;
                }

                // Match the footprint of coarse samples to the spacing between them.
                float lod = clamp(log2(max(radiusPx / float(samplesPerSide), 1.0)), 0.0, float(max(depthMipMax, radianceMipMax)));
                int depthMip = min(int(lod), depthMipMax);
                ivec2 depthSize = textureSize(linearDepthQuarter, depthMip);
                ivec2 depthCoord = clamp(ivec2(sampleUV * vec2(depthSize)), ivec2(0), depthSize - ivec2(1));
                float sampleDepth = texelFetch(linearDepthQuarter, depthCoord, depthMip).r;
                if (!IsValidLinearDepth(sampleDepth)) {
                    hasPrevious = false;
                    continue;
                }

                vec3 samplePos = ViewPosFromLinearDepth(sampleUV, sampleDepth, projScaleX, projScaleY);
                vec3 frontDelta = samplePos - centerPos;
                vec3 backDelta = frontDelta - SafeNormalizeStrict(-samplePos, viewVec) * thickness;
                if (dot(frontDelta, frontDelta) < 1e-10) {
                    continue;
                }
                ivec2 normalCoord = clamp(ivec2(sampleUV * quarterSize), ivec2(0), ivec2(quarterSize) - ivec2(1));
                vec3 sampleNormal = DecodeOctNormal01(texelFetch(normalFromDepthTex, normalCoord, 0).rg);
                vec3 sampleRadiance = max(textureLod(radianceTex, sampleUV, min(lod, float(radianceMipMax))).rgb, vec3(0.0));

                // Signed angles from V inside the slice plane.
                float thetaFront = atan(dot(frontDelta, orthoDir), dot(frontDelta, viewVec));
                float thetaBack = atan(dot(backDelta, orthoDir), dot(backDelta, viewVec));
                float thetaLow = min(thetaFront, thetaBack);
                float thetaHigh = max(thetaFront, thetaBack);

                // A constant thickness only covers a sliver of a surface seen edge-on (a floor seen
                // from a wall), leaving gaps between samples that would leak its light. When this
                // sample and the previous one lie on the same plane, the surface continues between
                // them, so it also occludes the directions in between.
                vec3 segment = samplePos - previousPos;
                bool continuesPrevious = hasPrevious &&
                    dot(sampleNormal, previousNormal) > 0.9 &&
                    abs(dot(segment, sampleNormal)) < 0.15 * length(segment) + 0.5 * thickness;
                vec3 emittedRadiance = sampleRadiance;
                if (continuesPrevious) {
                    thetaLow = min(thetaLow, previousThetaFront);
                    thetaHigh = max(thetaHigh, previousThetaFront);
                    emittedRadiance = 0.5 * (sampleRadiance + previousRadiance);
                }
                hasPrevious = true;
                previousPos = samplePos;
                previousNormal = sampleNormal;
                previousRadiance = sampleRadiance;
                previousThetaFront = thetaFront;

                uint sampleMask = SectorMaskFromInterval((thetaLow - gamma + HALF_PI) / PI, (thetaHigh - gamma + HALF_PI) / PI);
                uint newMask = sampleMask & ~occludedMask;
                occludedMask |= sampleMask;
                if (newMask == 0u) {
                    continue;
                }

                // Lambertian emitters only send light to the side their normal faces.
                if (dot(sampleNormal, -frontDelta) <= 0.0) {
                    continue;
                }

                float weight = MaskWeight(newMask, gamma, cosGamma, sinGamma);
                sliceIrradiance += emittedRadiance * weight;
                sliceSh += SHBasisL1(SafeNormalizeStrict(frontDelta, viewVec)) * (Luma(emittedRadiance) * weight);
            }
        }

        float openWeight = MaskWeight(~occludedMask, gamma, cosGamma, sinGamma);
        irradianceAccum += sliceIrradiance * projectedNormalLen;
        shAccum += sliceSh * projectedNormalLen;
        visibilityAccum += openWeight * projectedNormalLen;
        coverageAccum += float(bitCount(occludedMask)) / SECTOR_COUNT_F;
        if (slice == 0) {
            int lowest = occludedMask != 0u ? findLSB(occludedMask) : 0;
            int highest = occludedMask != 0u ? findMSB(occludedMask) : 0;
            firstSliceDebug = vec4(float(lowest) / SECTOR_COUNT_F, float(highest + 1) / SECTOR_COUNT_F,
                float(bitCount(occludedMask)) / SECTOR_COUNT_F, projectedNormalLen);
        }
    }

    // mean over slices of (|pn| * sum W) is 1 for a fully open or fully occluded hemisphere.
    float invSlices = 1.0 / float(sliceCount);
    vec3 irradiance = PI * irradianceAccum * invSlices;
    vec4 shOut = PI * shAccum * invSlices;
    float visibility = clamp(visibilityAccum * invSlices, 0.0, 1.0);
    float coverage = clamp(coverageAccum * invSlices, 0.0, 1.0);

    if (any(isnan(irradiance)) || any(isinf(irradiance))) {
        irradiance = vec3(0.0);
        shOut = vec4(0.0);
    }

    imageStore(outIndirectRaw, id, vec4(min(irradiance, vec3(NOX_FP16_MAX)), visibility));
    imageStore(outDirectionalRaw, id, shOut);
    imageStore(outHorizonDebug, id, vec4(coverage, visibility, 1.0 - visibility, maxRadiusPx / max(0.5 * quarterSize.y, 1.0)));
    imageStore(outSectorDebug, id, firstSliceDebug);
}
