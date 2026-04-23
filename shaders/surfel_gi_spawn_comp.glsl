#version 460 core

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#include "includes/transform_tracking_contract.glsl"
#include "includes/surfel_gi_common.glsl"

layout(binding = 6, std430) readonly buffer TransformBuffer {
    GpuTransformRecord transforms[];
};

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 22, std430) buffer FreeStackBuffer {
    uint freeStack[];
};

layout(binding = 24, std430) buffer TileCoverageBuffer {
    uvec4 tileCoverage[];
};

layout(binding = 25, std430) buffer GridHeaderBuffer {
    uvec4 gridHeaders[];
};

layout(binding = 26, std430) readonly buffer GridEntryBuffer {
    uint gridEntries[];
};

uniform sampler2D uPackedNormalRM;
uniform usampler2D uTransformIDTex;
uniform sampler2D uDepthTex;
layout(binding = 1, r32f) uniform readonly image2D uDeficitTex;

uniform int uFrameIndex;
uniform int uSpawnIteration;
uniform int uMaxTransformID;
uniform vec2 uResolution;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uInvViewProj;
uniform mat4 uInvView;
uniform float uTargetRadiusPixels;
uniform float uCoverageThreshold;
uniform float uNormalReject;
uniform vec3 uCameraPos;
uniform float uFreePoolReserveFraction;

shared float sCoverage[256];
shared float sScore[256];
shared uint sPackedPixel[256];
shared uint sTileVerifiedSkip;

const uint kMaxCoverageEntries = 20u;
const uint kMaxConfirmedCoverageEntries = 36u;
const uint kTileSkipVerificationSamples = 8u;
const uint kCoveredTileRecheckFrames = 4u;
const uint kUncertainTileRecheckFrames = 1u;

float SampleProjectedTileDeficit(ivec2 pixel);

vec3 ReconstructWorldPosition(ivec2 pixel, float depth)
{
    vec2 uv = (vec2(pixel) + vec2(0.5)) / max(uResolution, vec2(1.0));
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    return world.xyz / max(world.w, 0.00001);
}

float EvaluatePersistentTransformCoverage(
    vec3 worldPos,
    vec3 normal,
    uint transformID,
    uint maxEntries,
    bool includeAdjacentPrimaryCells)
{
    vec3 viewPos = (uView * vec4(worldPos, 1.0)).xyz;
    uint centerCell = GridCellIndexForViewPosition(viewPos, header);
    uint centerCount = min(min(gridHeaders[centerCell].x, header.gridDims.w), maxEntries);
    bool hasCandidates = centerCount > 0u;
    if (includeAdjacentPrimaryCells) {
        vec3 gridCoord = NonLinearGridCoord(viewPos, header);
        uvec3 baseCell = uvec3(floor(gridCoord));
        uvec3 dims = max(header.gridDims.xyz, uvec3(1u));
        for (int axis = 0; axis < 3 && !hasCandidates; ++axis) {
            for (int side = -1; side <= 1; side += 2) {
                ivec3 neighbor = ivec3(baseCell);
                neighbor[axis] += side;
                if (any(lessThan(neighbor, ivec3(0))) || any(greaterThanEqual(neighbor, ivec3(dims)))) {
                    continue;
                }
                uint cell = GridCellIndexFromCoord(uvec3(neighbor), header);
                hasCandidates = min(min(gridHeaders[cell].x, header.gridDims.w), maxEntries) > 0u;
                if (hasCandidates) {
                    break;
                }
            }
        }
    }
    if (!hasCandidates) {
        return 0.0;
    }

    GpuTransformRecord transformRecord = transforms[transformID];
    mat4 world = transformRecord.world;
    mat4 invWorld = inverse(world);
    vec3 localPos = (invWorld * vec4(worldPos, 1.0)).xyz;
    vec3 localNormal = normalize(transpose(mat3(world)) * normal);
    float coverage = 0.0;
    bool markedPersistentHit = false;

    vec3 centerGridCoord = NonLinearGridCoord(viewPos, header);
    uvec3 baseCell = uvec3(floor(centerGridCoord));
    uvec3 dims = max(header.gridDims.xyz, uvec3(1u));
    for (int pass = 0; pass < 7; ++pass) {
        if (pass > 0 && !includeAdjacentPrimaryCells) {
            break;
        }

        ivec3 cellCoord = ivec3(baseCell);
        if (pass == 1) cellCoord.x -= 1;
        if (pass == 2) cellCoord.x += 1;
        if (pass == 3) cellCoord.y -= 1;
        if (pass == 4) cellCoord.y += 1;
        if (pass == 5) cellCoord.z -= 1;
        if (pass == 6) cellCoord.z += 1;
        if (any(lessThan(cellCoord, ivec3(0))) || any(greaterThanEqual(cellCoord, ivec3(dims)))) {
            continue;
        }

        uint cell = (pass == 0) ? centerCell : GridCellIndexFromCoord(uvec3(cellCoord), header);
        uint count = min(min(gridHeaders[cell].x, header.gridDims.w), maxEntries);
        for (uint i = 0u; i < count; ++i) {
            uint surfelID = gridEntries[cell * header.gridDims.w + i];
            if (surfelID >= header.counts.x) {
                continue;
            }

            SurfelRecord candidate = surfels[surfelID];
            if (!IsSurfelValid(candidate) || candidate.ids.x != transformID) {
                continue;
            }
            if (includeAdjacentPrimaryCells && candidate.grid.x != cell) {
                continue;
            }

            float localNormalAlign = dot(localNormal, candidate.localNormalDebug.xyz);
            float worldNormalAlign = dot(normal, candidate.worldNormalRecycle.xyz);
            float normalAlign = min(localNormalAlign, worldNormalAlign);
            if (normalAlign < 1.0 - uNormalReject) {
                continue;
            }

            vec3 localDelta = localPos - candidate.localPositionAge.xyz;
            vec3 localTangentDelta = localDelta - candidate.localNormalDebug.xyz * dot(localDelta, candidate.localNormalDebug.xyz);
            float localTangentDistance = length(localTangentDelta);
            float localPlaneDistance = abs(dot(localDelta, candidate.localNormalDebug.xyz));

            vec3 worldDelta = worldPos - candidate.worldPositionRadius.xyz;
            float worldPlaneDistance = abs(dot(worldDelta, candidate.worldNormalRecycle.xyz));
            vec3 worldTangentDelta = worldDelta - candidate.worldNormalRecycle.xyz * dot(worldDelta, candidate.worldNormalRecycle.xyz);
            float worldTangentDistance = length(worldTangentDelta);

            float radius = max(candidate.worldPositionRadius.w, 0.001);
            float coverageRadius = radius * 1.45;
            float tangentDistance = min(localTangentDistance, worldTangentDistance);
            float planeDistance = min(localPlaneDistance, worldPlaneDistance);
            float tangentWeight = 1.0 - smoothstep(coverageRadius * 0.45, coverageRadius, tangentDistance);
            float planeWeight = 1.0 - smoothstep(0.0, coverageRadius * 0.45, planeDistance);
            float disc = tangentWeight * planeWeight;
            if (disc > 0.35 && !markedPersistentHit) {
                uint frameIndex = uint(max(uFrameIndex, 0));
                atomicMax(surfels[surfelID].frames.y, frameIndex);
                uint ageFrames = frameIndex - min(candidate.frames.x, frameIndex);
                if (ageFrames > 45u) {
                    atomicAdd(header.contributionStats.x, 1u);
                } else {
                    atomicAdd(header.contributionStats.y, 1u);
                }
                markedPersistentHit = true;
            }
            coverage += disc * clamp(normalAlign, 0.0, 1.0);
            if (coverage >= 2.2) {
                return coverage;
            }
        }
    }

    return coverage;
}

float ConfirmPersistentCoverageAtPixel(ivec2 pixel)
{
    float depth = texelFetch(uDepthTex, pixel, 0).r;
    if (depth >= 1.0) {
        return 9999.0;
    }

    uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
    if (transformID == 0u || transformID >= uint(max(uMaxTransformID, 1))) {
        return 9999.0;
    }

    float threshold = clamp(uCoverageThreshold, 0.05, 2.5);
    float deficit = SampleProjectedTileDeficit(pixel);
    return max(threshold - deficit, 0.0);
}

float ProjectedRadiusAtPixel(ivec2 pixel)
{
    float depth = texelFetch(uDepthTex, pixel, 0).r;
    if (depth >= 1.0) {
        return 0.0;
    }

    vec3 worldPos = ReconstructWorldPosition(pixel, depth);
    vec4 viewPos = uView * vec4(worldPos, 1.0);
    float targetRadiusPixels = ResolveTargetRadiusPixels(uTargetRadiusPixels, float(header.tiling.z), uCoverageThreshold);
    float radius = ComputeWorldRadiusForProjectedPixels(viewPos.z, targetRadiusPixels, uResolution.y, uProjection);
    return radius * uResolution.y * max(abs(uProjection[1][1]), 0.0001) / max(abs(viewPos.z), 0.05);
}

float SampleProjectedTileDeficit(ivec2 pixel)
{
    return max(imageLoad(uDeficitTex, pixel).r, 0.0);
}

bool HasPersistentDuplicateSurfel(vec3 worldPos, vec3 normal, uint transformID, float radius)
{
    vec3 viewPos = (uView * vec4(worldPos, 1.0)).xyz;
    vec3 centerGridCoord = NonLinearGridCoord(viewPos, header);
    uvec3 baseCell = uvec3(floor(centerGridCoord));
    uvec3 dims = max(header.gridDims.xyz, uvec3(1u));

    GpuTransformRecord transformRecord = transforms[transformID];
    mat4 invWorld = inverse(transformRecord.world);
    vec3 localPos = (invWorld * vec4(worldPos, 1.0)).xyz;
    vec3 localNormal = normalize(transpose(mat3(transformRecord.world)) * normal);

    for (int pass = 0; pass < 7; ++pass) {
        ivec3 cellCoord = ivec3(baseCell);
        if (pass == 1) cellCoord.x -= 1;
        if (pass == 2) cellCoord.x += 1;
        if (pass == 3) cellCoord.y -= 1;
        if (pass == 4) cellCoord.y += 1;
        if (pass == 5) cellCoord.z -= 1;
        if (pass == 6) cellCoord.z += 1;
        if (any(lessThan(cellCoord, ivec3(0))) || any(greaterThanEqual(cellCoord, ivec3(dims)))) {
            continue;
        }

        uint cell = GridCellIndexFromCoord(uvec3(cellCoord), header);
        uint count = min(gridHeaders[cell].x, header.gridDims.w);
        for (uint i = 0u; i < count; ++i) {
            uint surfelID = gridEntries[cell * header.gridDims.w + i];
            if (surfelID >= header.counts.x) {
                continue;
            }

            SurfelRecord candidate = surfels[surfelID];
            if (!IsSurfelValid(candidate) || candidate.ids.x != transformID) {
                continue;
            }

            float localNormalAlign = dot(localNormal, candidate.localNormalDebug.xyz);
            float worldNormalAlign = dot(normal, candidate.worldNormalRecycle.xyz);
            if (min(localNormalAlign, worldNormalAlign) < 1.0 - uNormalReject * 0.55) {
                continue;
            }

            vec3 localDelta = localPos - candidate.localPositionAge.xyz;
            float localPlaneDistance = abs(dot(localDelta, candidate.localNormalDebug.xyz));
            vec3 localTangentDelta = localDelta - candidate.localNormalDebug.xyz * dot(localDelta, candidate.localNormalDebug.xyz);
            float localTangentDistance = length(localTangentDelta);
            float duplicateRadius = max(max(radius, candidate.worldPositionRadius.w), 0.001);
            float radiusRatio = radius / max(candidate.worldPositionRadius.w, 0.001);

            bool nearlySamePatch =
                localPlaneDistance < duplicateRadius * 0.20 &&
                localTangentDistance < duplicateRadius * 0.58 &&
                radiusRatio > 0.50 &&
                radiusRatio < 2.0;
            if (nearlySamePatch) {
                return true;
            }
        }
    }

    return false;
}

uint AllocateSurfelID(uint tileIndex, uint frameIndex, out bool fromFreeStack)
{
    fromFreeStack = false;

    uint freeCountBefore = header.counts.z;
    while (freeCountBefore > 0u) {
        uint previous = atomicCompSwap(header.counts.z, freeCountBefore, freeCountBefore - 1u);
        if (previous == freeCountBefore) {
            uint surfelID = freeStack[freeCountBefore - 1u];
            if (surfelID < header.counts.x) {
                if (!IsSurfelAllocatable(surfels[surfelID])) {
                    uint restoreSlot = atomicAdd(header.counts.z, 1u);
                    if (restoreSlot < header.counts.x) {
                        freeStack[restoreSlot] = surfelID;
                    }
                    atomicAdd(header.recycleStats.w, 1u);
                    return 0xffffffffu;
                }
                fromFreeStack = true;
                return surfelID;
            }
            return 0xffffffffu;
        }
        freeCountBefore = previous;
    }
    return 0xffffffffu;
}

void SpawnSurfel(ivec2 pixel, float coverage, uint tileIndex, uint frameIndex, uint spawnReason)
{
    float depth = texelFetch(uDepthTex, pixel, 0).r;
    if (depth >= 1.0) {
        return;
    }

    uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
    if (transformID == 0u || transformID >= uint(max(uMaxTransformID, 1))) {
        return;
    }

    vec4 packedNormal = texelFetch(uPackedNormalRM, pixel, 0);
    vec3 normal = DecodeNormalOctSurfel(packedNormal.xy);
    vec3 worldPos = ReconstructWorldPosition(pixel, depth);
    vec4 viewPos = uView * vec4(worldPos, 1.0);
    float targetRadiusPixels = ResolveTargetRadiusPixels(uTargetRadiusPixels, float(header.tiling.z), uCoverageThreshold);
    float radius = ComputeWorldRadiusForProjectedPixels(viewPos.z, targetRadiusPixels, uResolution.y, uProjection);
    float projectedRadius = radius * uResolution.y * max(abs(uProjection[1][1]), 0.0001) / max(abs(viewPos.z), 0.05);

    if (HasPersistentDuplicateSurfel(worldPos, normal, transformID, radius)) {
        atomicAdd(header.coverageStats.y, 1u);
        return;
    }

    bool fromFreeStack = false;
    uint surfelID = AllocateSurfelID(tileIndex, frameIndex, fromFreeStack);
    if (surfelID >= header.counts.x) {
        atomicAdd(header.coverageStats.z, 1u);
        return;
    }

    GpuTransformRecord transformRecord = transforms[transformID];
    mat4 world = transformRecord.world;
    mat4 invWorld = inverse(world);
    vec3 localPos = (invWorld * vec4(worldPos, 1.0)).xyz;
    vec3 localNormal = normalize(transpose(mat3(world)) * normal);

    SurfelRecord s;
    s.worldPositionRadius = vec4(worldPos, radius);
    s.localPositionAge = vec4(localPos, 0.0);
    s.worldNormalRecycle = vec4(normal, 0.0);
    s.localNormalDebug = vec4(localNormal, float(spawnReason));
    s.ids = uvec4(transformID, SURFEL_FLAG_VALID, surfelID, HashUInt(surfelID ^ frameIndex ^ tileIndex));
    s.frames = uvec4(frameIndex, frameIndex, frameIndex, 0u);
    s.grid = uvec4(0u, 0u, 0u, SURFEL_STATE_ACTIVE);
    s.metrics = vec4(coverage, distance(worldPos, uCameraPos), projectedRadius, 1.0);
    s.irradianceHistory = vec4(0.0);
    s.shortTermStats = vec4(0.0);
    s.longTermStats = vec4(0.0);
    s.recycleData = vec4(0.0);
    s.depthMoments = vec4(0.0, 0.0, 0.0, radius);
    s.guidingState = vec4(0.0, 0.0, 0.0, float(transformRecord.metadata.z));
    surfels[surfelID] = s;

    if (fromFreeStack) {
        atomicAdd(header.counts.y, 1u);
    }
    atomicAdd(header.frameStats.x, 1u);
}

void main()
{
    uvec2 local = gl_LocalInvocationID.xy;
    uint localIndex = local.y * 16u + local.x;
    uint tileSize = max(header.tiling.z, 1u);
    uvec2 tile = gl_WorkGroupID.xy;
    uint tileIndex = tile.y * header.tiling.x + tile.x;
    ivec2 pixel = ivec2(tile * tileSize + local);
    uint frameIndex = uint(max(uFrameIndex, 0));
    uvec4 previousTile = tileCoverage[tileIndex];

    if (localIndex == 0u) {
        sTileVerifiedSkip = 0u;
    }
    barrier();

    float jitter = Hash01(HashUInt(tileIndex ^ (uint(max(uSpawnIteration, 0)) * 1013904223u)));
    float randomizedThreshold = uCoverageThreshold * mix(0.90, 1.10, jitter);
    float threshold = clamp(randomizedThreshold, 0.05, 2.5);
    float tilePersistentCoverage = float(previousTile.x) / 1024.0;
    uint framesSinceCoverageEval = previousTile.w == 0u
        ? 0xffffffffu
        : (frameIndex + 1u) - min(previousTile.w, frameIndex + 1u);
    bool skipTileEvaluation =
        tilePersistentCoverage >= threshold &&
        framesSinceCoverageEval < kCoveredTileRecheckFrames;
    if (!skipTileEvaluation &&
        tilePersistentCoverage >= threshold * 0.65 &&
        framesSinceCoverageEval < kUncertainTileRecheckFrames) {
        skipTileEvaluation = true;
    }

    if (skipTileEvaluation && localIndex < kTileSkipVerificationSamples) {
        uvec2 sampleGrid = uvec2(localIndex & 1u, localIndex >> 1u);
        uvec2 sampleLocal = min((sampleGrid * tileSize + tileSize / 2u) / 2u, uvec2(tileSize - 1u));
        ivec2 samplePixel = ivec2(tile * tileSize + sampleLocal);
        bool inBounds = samplePixel.x < int(uResolution.x) && samplePixel.y < int(uResolution.y);
        if (inBounds) {
            float sampleDepth = texelFetch(uDepthTex, samplePixel, 0).r;
            uint sampleTransformID = texelFetch(uTransformIDTex, samplePixel, 0).r;
            bool validSample =
                sampleDepth < 1.0 &&
                sampleTransformID != 0u &&
                sampleTransformID < uint(max(uMaxTransformID, 1));
            if (validSample) {
                float sampleDeficit = SampleProjectedTileDeficit(samplePixel);
                float sampledCoverage = max(threshold - sampleDeficit, 0.0);
                sCoverage[localIndex] = sampledCoverage >= threshold
                    ? ConfirmPersistentCoverageAtPixel(samplePixel)
                    : sampledCoverage;
            } else {
                sCoverage[localIndex] = 9999.0;
            }
        } else {
            sCoverage[localIndex] = 9999.0;
        }
    }
    barrier();
    if (skipTileEvaluation && localIndex == 0u) {
        float verifiedMinCoverage = 9999.0;
        uint validSampleCount = 0u;
        for (uint i = 0u; i < kTileSkipVerificationSamples; ++i) {
            if (sCoverage[i] < 9998.0) {
                verifiedMinCoverage = min(verifiedMinCoverage, sCoverage[i]);
                validSampleCount += 1u;
            }
        }
        if (validSampleCount > 0u && verifiedMinCoverage >= threshold) {
            tileCoverage[tileIndex] = uvec4(
                uint(clamp(verifiedMinCoverage, 0.0, 16.0) * 1024.0),
                previousTile.y,
                previousTile.z,
                frameIndex + 1u);
            atomicAdd(header.coverageStats.x, 1u);
            sTileVerifiedSkip = 1u;
        }
    }
    barrier();
    if (sTileVerifiedSkip != 0u) {
        return;
    }

    float coverage = 9999.0;
    bool validPixel = local.x < tileSize && local.y < tileSize &&
        pixel.x < int(uResolution.x) && pixel.y < int(uResolution.y);

    if (validPixel) {
        coverage = ConfirmPersistentCoverageAtPixel(pixel);
        validPixel = coverage < 9998.0;
    }

    float tileDeficit = 0.0;
    float orderScore = 9999.0;
    if (validPixel) {
        tileDeficit = SampleProjectedTileDeficit(pixel);
        uint noiseSeed =
            uint(max(pixel.x, 0)) * 73856093u ^
            uint(max(pixel.y, 0)) * 19349663u ^
            tileIndex * 83492791u ^
            uint(max(uSpawnIteration, 0)) * 2246822519u;
        float stableJitter = Hash01(HashUInt(noiseSeed));
        orderScore = -tileDeficit + stableJitter * 0.00025;
    }

    sCoverage[localIndex] = coverage;
    sScore[localIndex] = orderScore;
    sPackedPixel[localIndex] = (uint(max(pixel.y, 0)) << 16u) | uint(max(pixel.x, 0));
    barrier();

    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (localIndex < stride) {
            uint otherIndex = localIndex + stride;
            if (sScore[otherIndex] < sScore[localIndex]) {
                sCoverage[localIndex] = sCoverage[otherIndex];
                sScore[localIndex] = sScore[otherIndex];
                sPackedPixel[localIndex] = sPackedPixel[otherIndex];
            }
        }
        barrier();
    }

    if (localIndex == 0u) {
        float minCoverage = sCoverage[0];
        uint packedPixel = sPackedPixel[0];
        ivec2 leastPixel = ivec2(int(packedPixel & 0xffffu), int(packedPixel >> 16u));
        if (minCoverage >= 9998.0) {
            return;
        }

        float confirmedCoverage = ConfirmPersistentCoverageAtPixel(leastPixel);
        minCoverage = confirmedCoverage;
        if (minCoverage >= 9998.0) {
            return;
        }

        float tileDeficit = SampleProjectedTileDeficit(leastPixel);
        if (tileDeficit <= 0.0) {
            tileDeficit = max(threshold - minCoverage, 0.0);
        }

        tileCoverage[tileIndex] = uvec4(
            uint(clamp(minCoverage, 0.0, 16.0) * 1024.0),
            packedPixel,
            previousTile.z,
            frameIndex + 1u);

        if (minCoverage >= threshold) {
            atomicAdd(header.coverageStats.x, 1u);
            return;
        }

        float severity = clamp(tileDeficit / max(threshold, 0.001), 0.0, 1.0);
        float severeGapBudgetScale = 1.0;
        if (severity > 0.82) {
            severeGapBudgetScale = 0.0;
        }

        uint freeCount = header.counts.z;
        float freeFraction = float(header.counts.z) / max(float(header.counts.x), 1.0);
        float reserve = uFreePoolReserveFraction * severeGapBudgetScale;
        if (severity > 0.82 && freeCount > 0u) {
            reserve = 0.0;
        }
        if (freeFraction <= reserve) {
            atomicAdd(header.coverageStats.z, 1u);
            return;
        }

        float framesSinceTileSpawn = previousTile.z == 0u
            ? 9999.0
            : float((frameIndex + 1u) - min(previousTile.z, frameIndex + 1u));

        if (framesSinceTileSpawn < 1.5 && severity < 0.55) {
            atomicAdd(header.coverageStats.w, 1u);
            return;
        }

        atomicAdd(header.frameStats.w, 1u);

        float projectedRadius = ProjectedRadiusAtPixel(leastPixel);
        float surfelPixels = max(3.14159265 * projectedRadius * projectedRadius, 1.0);
        float tilePixels = float(tileSize * tileSize);
        float idealSurfelsForTile = max(tilePixels / surfelPixels, 1.0);
        float missingSurfels = max(tileDeficit * idealSurfelsForTile, 0.0);

        uint spawnReason = missingSurfels > 1.25 ? SURFEL_SPAWN_REFINEMENT : SURFEL_SPAWN_COVERAGE_GAP;
        tileCoverage[tileIndex].z = frameIndex + 1u;
        SpawnSurfel(leastPixel, minCoverage, tileIndex, frameIndex, spawnReason);
    }
}
