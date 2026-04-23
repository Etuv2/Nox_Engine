#version 460 core

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "includes/transform_tracking_contract.glsl"
#include "includes/surfel_gi_common.glsl"
#include "includes/surfel_gi_projection_support.glsl"
#include "includes/surfel_gi_visibility_compat.glsl"

layout(binding = 6, std430) readonly buffer TransformBuffer {
    GpuTransformRecord transforms[];
};

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 0, r32ui) uniform uimage2D uProjectedCoverage;
layout(binding = 2, r32ui) uniform uimage2D uRawProjectedSupport;
layout(binding = 3, r32ui) uniform uimage2D uDepthReject;
layout(binding = 4, r32ui) uniform uimage2D uNormalRejectImage;
layout(binding = 5, r32ui) uniform uimage2D uWinnerSurfelID;

uniform sampler2D uPackedNormalRM;
uniform usampler2D uTransformIDTex;
uniform sampler2D uDepthTex;

uniform int uFrameIndex;
uniform vec2 uResolution;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uInvViewProj;
uniform float uNormalReject;
uniform float uDepthThicknessScale;
uniform int uMaxTransformID;

void main()
{
    uint surfelID = gl_GlobalInvocationID.x;
    if (surfelID >= header.counts.x) {
        return;
    }

    SurfelRecord s = surfels[surfelID];
    if (!IsSurfelValid(s)) {
        return;
    }

    uint surfelTransformID = s.ids.x;
    if (surfelTransformID == 0u || surfelTransformID >= uint(max(uMaxTransformID, 1))) {
        return;
    }

    GpuTransformRecord transformRecord = transforms[surfelTransformID];
    mat4 worldFromLocal = transformRecord.world;
    vec3 worldPos = (worldFromLocal * vec4(s.localPositionAge.xyz, 1.0)).xyz;
    vec3 normal = normalize(transpose(inverse(mat3(worldFromLocal))) * s.localNormalDebug.xyz);
    float radius = max(s.worldPositionRadius.w, 0.001);

    vec4 viewCenter = uView * vec4(worldPos, 1.0);
    vec4 clipCenter = uProjection * viewCenter;
    if (clipCenter.w <= 0.0001) {
        return;
    }

    vec3 tangent;
    vec3 bitangent;
    SurfelBuildOrthonormalBasis(normal, tangent, bitangent);

    vec2 centerPx;
    vec2 corner00;
    vec2 corner10;
    vec2 corner01;
    vec2 corner11;
    if (!SurfelProjectPatchCorners(
        worldPos,
        tangent,
        bitangent,
        radius,
        uView,
        uProjection,
        uResolution,
        centerPx,
        corner00,
        corner10,
        corner01,
        corner11)) {
        return;
    }

    ivec2 minPixel;
    ivec2 maxPixel;
    SurfelSupportBoundsFromCornerPixels(centerPx, corner00, corner10, corner01, corner11, uResolution, minPixel, maxPixel);
    if (any(greaterThan(minPixel, maxPixel))) {
        return;
    }

    bool contributed = false;
    uint frameIndex = uint(max(uFrameIndex, 0));
    float depthTolBase = max(radius * max(uDepthThicknessScale, 0.05), 0.01);
    float viewThickness = depthTolBase + abs(viewCenter.z) * 0.0045;

    for (int y = minPixel.y; y <= maxPixel.y; ++y) {
        for (int x = minPixel.x; x <= maxPixel.x; ++x) {
            ivec2 pixel = ivec2(x, y);
            float depth = texelFetch(uDepthTex, pixel, 0).r;
            if (depth >= 1.0) {
                continue;
            }

            uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
            if (transformID == 0u || transformID >= uint(max(uMaxTransformID, 1))) {
                continue;
            }
            if (transformID != surfelTransformID) {
                continue;
            }

            vec2 samplePx = vec2(pixel) + vec2(0.5);
            if (!SurfelPointInConvexQuad(samplePx, corner00, corner10, corner11, corner01)) {
                continue;
            }

            float supportWeight = 1.0;
            uint quantizedRawWeight = uint(clamp(supportWeight * 1024.0, 1.0, 4096.0));
            imageAtomicAdd(uRawProjectedSupport, pixel, quantizedRawWeight);

            vec3 receiverNormal = SurfelGIDecodeNormalOct(texelFetch(uPackedNormalRM, pixel, 0).xy);
            vec3 receiverWorldPos = SurfelGIReconstructWorldPosition(pixel, depth, uResolution, uInvViewProj);
            SurfelGIVisibilityCompatResult compat = SurfelGIEvaluateVisibilityCompatibility(
                worldPos,
                normal,
                receiverWorldPos,
                receiverNormal,
                uView,
                viewThickness,
                uNormalReject);
            if ((compat.reasonBits & SURFEL_GI_VISIBILITY_REJECT_DEPTH) != 0u) {
                imageAtomicAdd(uDepthReject, pixel, quantizedRawWeight);
            }
            if ((compat.reasonBits & SURFEL_GI_VISIBILITY_REJECT_NORMAL) != 0u) {
                imageAtomicAdd(uNormalRejectImage, pixel, quantizedRawWeight);
            }
            if (compat.reasonBits != 0u) {
                continue;
            }

            float momentWeight = SurfelDepthValidityWeight(s, receiverWorldPos, receiverNormal);
            momentWeight = max(mix(1.0, momentWeight, 0.35), 0.65);
            float weight = supportWeight * compat.validCoverage * momentWeight;
            if (weight <= 0.001) {
                continue;
            }

            uint quantizedWeight = uint(clamp(weight * 1024.0, 1.0, 4096.0));
            imageAtomicAdd(uProjectedCoverage, pixel, quantizedWeight);
            imageAtomicMin(uWinnerSurfelID, pixel, surfelID);
            contributed = true;
        }
    }

    if (contributed) {
        atomicMax(surfels[surfelID].frames.y, frameIndex);
        uint ageFrames = frameIndex - min(s.frames.x, frameIndex);
        if (ageFrames > 45u) {
            atomicAdd(header.contributionStats.x, 1u);
        } else {
            atomicAdd(header.contributionStats.y, 1u);
        }
    }
}
