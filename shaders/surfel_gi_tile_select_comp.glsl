#version 460 core

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 24, std430) buffer TileCoverageBuffer {
    SurfelTileMeta tileMeta[];
};

layout(binding = 27, std430) buffer TileQueueBuffer {
    uvec4 queueHeader[4];
    uint tileQueue[];
};

layout(binding = 1, r32f) uniform readonly image2D uDeficitTex;

uniform sampler2D uDepthTex;
uniform sampler2D uPackedNormalRM;
uniform usampler2D uTransformIDTex;

uniform int uFrameIndex;
uniform vec2 uResolution;
uniform float uCoverageThreshold;
uniform int uTileScanCursor;
uniform int uMaxTilesToScan;
uniform float uCameraMotion;
uniform float uQualityScale;

shared float sCoverage[256];
shared float sDepth[256];
shared vec3 sNormal[256];
shared uint sValid[256];
shared uint sPackedPixel[256];

uint AtomicAddBucketCount(uint bucket, uint value)
{
    if (bucket == 0u) {
        return atomicAdd(queueHeader[0].x, value);
    }
    if (bucket == 1u) {
        return atomicAdd(queueHeader[0].y, value);
    }
    if (bucket == 2u) {
        return atomicAdd(queueHeader[0].z, value);
    }
    return atomicAdd(queueHeader[0].w, value);
}

void StoreTileQueue(uint tileIndex, float priority, uint frameIndex)
{
    uint maxTilesPerBucket = max(queueHeader[2].x, 1u);

    uint bucket = 3u;
    if (priority >= 0.78) {
        bucket = 0u;
    } else if (priority >= 0.55) {
        bucket = 1u;
    } else if (priority >= 0.30) {
        bucket = 2u;
    }

    uint slot = AtomicAddBucketCount(bucket, 1u);
    if (slot < maxTilesPerBucket) {
        tileQueue[bucket * maxTilesPerBucket + slot] = tileIndex;
        atomicAdd(header.tileWorkStats.z, 1u);
        atomicMax(tileMeta[tileIndex].state.w, frameIndex + 1u);
    } else {
        atomicAdd(header.queueStats.x, 1u);
    }
}

void main()
{
    uint scanIndex = gl_WorkGroupID.x;
    uint maxScan = uint(max(uMaxTilesToScan, 0));
    if (scanIndex >= maxScan) {
        return;
    }

    uint tileSize = max(header.tiling.z, 1u);
    uint tileCountX = max(header.tiling.x, 1u);
    uint tileCountY = max(header.tiling.y, 1u);
    uint totalTiles = max(tileCountX * tileCountY, 1u);
    uint frameIndex = uint(max(uFrameIndex, 0));

    uint jitterOffset = HashUInt(frameIndex * 1664525u + 1013904223u) % totalTiles;
    uint tileIndex = (uint(max(uTileScanCursor, 0)) + scanIndex + jitterOffset) % totalTiles;
    uvec2 tile = uvec2(tileIndex % tileCountX, tileIndex / tileCountX);

    uint localX = gl_LocalInvocationID.x;
    uint localY = gl_LocalInvocationID.y;
    uint localIndex = localY * 16u + localX;
    ivec2 pixel = ivec2(tile * tileSize + uvec2(localX, localY));

    bool inBounds = pixel.x >= 0 &&
        pixel.y >= 0 &&
        pixel.x < int(uResolution.x) &&
        pixel.y < int(uResolution.y);

    float coverage = 9999.0;
    float depth = 1.0;
    vec3 normal = vec3(0.0, 0.0, 1.0);
    bool valid = false;

    if (inBounds) {
        depth = texelFetch(uDepthTex, pixel, 0).r;
        uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
        valid = depth < 1.0 && transformID != 0u;
        if (valid) {
            float deficit = max(imageLoad(uDeficitTex, pixel).r, 0.0);
            coverage = max(uCoverageThreshold - deficit, 0.0);
            normal = DecodeNormalOctSurfel(texelFetch(uPackedNormalRM, pixel, 0).xy);
        }
    }

    sCoverage[localIndex] = coverage;
    sDepth[localIndex] = depth;
    sNormal[localIndex] = normal;
    sValid[localIndex] = valid ? 1u : 0u;
    sPackedPixel[localIndex] = (uint(max(pixel.y, 0)) << 16u) | uint(max(pixel.x, 0));
    barrier();

    if (localIndex != 0u) {
        return;
    }

    atomicAdd(header.tileWorkStats.x, 1u);

    SurfelTileMeta previous = tileMeta[tileIndex];
    float threshold = clamp(uCoverageThreshold, 0.05, 2.5);
    float minCoverage = 9999.0;
    float coverageSum = 0.0;
    float minDepth = 1.0;
    float maxDepth = 0.0;
    vec3 normalSum = vec3(0.0);
    uint validCount = 0u;
    uint candidatePacked = previous.coverage.z;

    for (uint i = 0u; i < 256u; ++i) {
        if (sValid[i] == 0u) {
            continue;
        }
        float sampleCoverage = sCoverage[i];
        float sampleDepth = sDepth[i];
        validCount += 1u;
        coverageSum += sampleCoverage;
        minDepth = min(minDepth, sampleDepth);
        maxDepth = max(maxDepth, sampleDepth);
        normalSum += sNormal[i];
        if (sampleCoverage < minCoverage) {
            minCoverage = sampleCoverage;
            candidatePacked = sPackedPixel[i];
        }
    }

    if (validCount == 0u) {
        SurfelTileMeta updated = previous;
        updated.coverage.x = 0u;
        updated.coverage.y = uint(clamp(threshold * 1024.0, 0.0, 1048576.0));
        updated.coverage.w = frameIndex + 1u;
        updated.geom = uvec4(0u);
        updated.state.z &= ~(SURFEL_TILE_FLAG_VISIBLE |
            SURFEL_TILE_FLAG_UNDERCOVERED |
            SURFEL_TILE_FLAG_HIGH_MOTION |
            SURFEL_TILE_FLAG_NEWLY_EXPOSED |
            SURFEL_TILE_FLAG_NEAR_CAMERA);
        tileMeta[tileIndex] = updated;
        return;
    }

    float normalVariance = 1.0 - clamp(length(normalSum / float(validCount)), 0.0, 1.0);
    float previousDepth = float(previous.geom.x) / 65535.0;
    float previousVariance = float(previous.geom.z) / 65535.0;

    bool previouslyVisible = (previous.state.z & SURFEL_TILE_FLAG_VISIBLE) != 0u;
    bool previouslyUndercovered = (previous.state.z & SURFEL_TILE_FLAG_UNDERCOVERED) != 0u;
    bool newlyExposed = !previouslyVisible;
    bool nearCamera = minDepth < 0.40;
    bool highMotion =
        abs(minDepth - previousDepth) > 0.03 ||
        abs(normalVariance - previousVariance) > 0.08 ||
        uCameraMotion > mix(0.025, 0.010, clamp(uQualityScale, 0.25, 2.0));

    uint lastCoveredFrame = previous.state.x > 0u ? (previous.state.x - 1u) : 0u;
    uint framesSinceCovered = previous.state.x > 0u ? (frameIndex - min(lastCoveredFrame, frameIndex)) : 0xffffffffu;
    uint lastUpdatedFrame = previous.coverage.w > 0u ? (previous.coverage.w - 1u) : 0u;
    uint framesSinceUpdated = previous.coverage.w > 0u ? (frameIndex - min(lastUpdatedFrame, frameIndex)) : 0xffffffffu;

    bool recentlyCovered = framesSinceCovered < 6u;
    bool shouldRecheck =
        previouslyUndercovered ||
        newlyExposed ||
        nearCamera ||
        highMotion ||
        framesSinceUpdated > (recentlyCovered ? 8u : 3u);

    SurfelTileMeta updated = previous;
    updated.coverage.x = uint(clamp(coverageSum, 0.0, 1048576.0) * 1024.0);
    updated.coverage.y = uint(clamp(minCoverage, 0.0, 1024.0) * 1024.0);
    updated.coverage.z = candidatePacked;
    updated.coverage.w = frameIndex + 1u;
    updated.geom.x = uint(clamp(minDepth, 0.0, 1.0) * 65535.0);
    updated.geom.y = uint(clamp(maxDepth, 0.0, 1.0) * 65535.0);
    updated.geom.z = uint(clamp(normalVariance, 0.0, 1.0) * 65535.0);

    vec2 tileCenterPx = (vec2(tile) + vec2(0.5)) * float(tileSize);
    vec2 centerUv = tileCenterPx / max(uResolution, vec2(1.0));
    float centerScore = 1.0 - clamp(length(centerUv - vec2(0.5)) * 1.75, 0.0, 1.0);
    float severity = clamp((threshold - minCoverage) / max(threshold, 0.001), 0.0, 1.0);
    float nearScore = 1.0 - clamp(minDepth, 0.0, 1.0);
    float areaScore = clamp(float(validCount) / 256.0, 0.0, 1.0);
    float motionScore = clamp(uCameraMotion * 6.0 + abs(minDepth - previousDepth) * 8.0 + abs(normalVariance - previousVariance) * 4.0, 0.0, 1.0);
    float priority = clamp(
        severity * 0.42 +
        nearScore * 0.20 +
        centerScore * 0.12 +
        areaScore * 0.10 +
        normalVariance * 0.10 +
        motionScore * 0.16 +
        (previouslyUndercovered ? 0.10 : 0.0),
        0.0,
        1.0);
    updated.geom.w = uint(priority * 65535.0);

    uint flags = updated.state.z;
    flags |= SURFEL_TILE_FLAG_VISIBLE;
    flags &= ~(SURFEL_TILE_FLAG_UNDERCOVERED |
        SURFEL_TILE_FLAG_HIGH_MOTION |
        SURFEL_TILE_FLAG_NEWLY_EXPOSED |
        SURFEL_TILE_FLAG_CONFIDENT_COVERED |
        SURFEL_TILE_FLAG_NEAR_CAMERA);
    if (highMotion) {
        flags |= SURFEL_TILE_FLAG_HIGH_MOTION;
    }
    if (newlyExposed) {
        flags |= SURFEL_TILE_FLAG_NEWLY_EXPOSED;
    }
    if (nearCamera) {
        flags |= SURFEL_TILE_FLAG_NEAR_CAMERA;
    }

    bool undercovered = minCoverage < threshold;
    if (undercovered) {
        flags |= SURFEL_TILE_FLAG_UNDERCOVERED;
    } else {
        flags |= SURFEL_TILE_FLAG_CONFIDENT_COVERED;
        updated.state.x = frameIndex + 1u;
    }

    if (!shouldRecheck && !undercovered && recentlyCovered) {
        atomicAdd(header.tileWorkStats.y, 1u);
        updated.stats.y += 1u;
        updated.state.z = flags;
        updated.stats.x += 1u;
        tileMeta[tileIndex] = updated;
        return;
    }

    updated.state.z = flags;
    updated.stats.x += 1u;
    tileMeta[tileIndex] = updated;

    if (undercovered) {
        StoreTileQueue(tileIndex, priority, frameIndex);
    }
}
