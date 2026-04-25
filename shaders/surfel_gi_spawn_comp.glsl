#version 460 core

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

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
    SurfelTileMeta tileCoverage[];
};

layout(binding = 25, std430) buffer GridHeaderBuffer {
    uvec4 gridHeaders[];
};

layout(binding = 26, std430) readonly buffer GridEntryBuffer {
    uint gridEntries[];
};

layout(binding = 27, std430) buffer TileQueueBuffer {
    uvec4 queueHeader[4];
    uint tileQueue[];
};

uniform sampler2D uPackedNormalRM;
uniform usampler2D uTransformIDTex;
uniform sampler2D uDepthTex;
layout(binding = 1, r32f) uniform readonly image2D uDeficitTex;

uniform int uFrameIndex;
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
uniform int uMaxSpawnCandidates;
uniform int uMaxSpawns;

uint AtomicAddBucketRead(uint bucket, uint value)
{
    if (bucket == 0u) {
        return atomicAdd(queueHeader[1].x, value);
    }
    if (bucket == 1u) {
        return atomicAdd(queueHeader[1].y, value);
    }
    if (bucket == 2u) {
        return atomicAdd(queueHeader[1].z, value);
    }
    return atomicAdd(queueHeader[1].w, value);
}

uint BucketCount(uint bucket)
{
    if (bucket == 0u) {
        return queueHeader[0].x;
    }
    if (bucket == 1u) {
        return queueHeader[0].y;
    }
    if (bucket == 2u) {
        return queueHeader[0].z;
    }
    return queueHeader[0].w;
}

bool PopNextTile(out uint tileIndex)
{
    uint maxTilesPerBucket = max(queueHeader[2].x, 1u);
    for (uint bucket = 0u; bucket < 4u; ++bucket) {
        uint readIndex = AtomicAddBucketRead(bucket, 1u);
        uint count = BucketCount(bucket);
        if (readIndex < count && readIndex < maxTilesPerBucket) {
            tileIndex = tileQueue[bucket * maxTilesPerBucket + readIndex];
            return true;
        }
    }
    tileIndex = 0xffffffffu;
    return false;
}

vec3 ReconstructWorldPosition(ivec2 pixel, float depth)
{
    vec2 uv = (vec2(pixel) + vec2(0.5)) / max(uResolution, vec2(1.0));
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    return world.xyz / max(world.w, 0.00001);
}

bool HasPersistentDuplicateSurfel(vec3 worldPos, vec3 normal, uint transformID, float radius)
{
    vec3 centerGridCoord = NonLinearGridCoord(worldPos - uCameraPos, header);
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

uint AllocateSurfelID()
{
    uint freeCountBefore = header.counts.z;
    while (freeCountBefore > 0u) {
        uint previous = atomicCompSwap(header.counts.z, freeCountBefore, freeCountBefore - 1u);
        if (previous == freeCountBefore) {
            uint surfelID = freeStack[freeCountBefore - 1u];
            if (surfelID < header.counts.x && IsSurfelAllocatable(surfels[surfelID])) {
                return surfelID;
            }
            uint restoreSlot = atomicAdd(header.counts.z, 1u);
            if (restoreSlot < header.counts.x) {
                freeStack[restoreSlot] = surfelID;
            }
            atomicAdd(header.recycleStats.w, 1u);
            return 0xffffffffu;
        }
        freeCountBefore = previous;
    }
    return 0xffffffffu;
}

void ReleaseSpawnReservation()
{
    uint current = header.frameStats.x;
    while (current > 0u) {
        uint previous = atomicCompSwap(header.frameStats.x, current, current - 1u);
        if (previous == current) {
            return;
        }
        current = previous;
    }
}

bool ReserveSpawnBudget(uint maxSpawns)
{
    uint current = header.frameStats.x;
    while (current < maxSpawns) {
        uint previous = atomicCompSwap(header.frameStats.x, current, current + 1u);
        if (previous == current) {
            return true;
        }
        current = previous;
    }
    return false;
}

void SpawnSurfelFromTile(uint tileIndex, SurfelTileMeta tile, uint frameIndex)
{
    float threshold = clamp(uCoverageThreshold, 0.05, 2.5);
    float minCoverage = float(tile.coverage.y) / 1024.0;
    if (minCoverage >= threshold) {
        atomicAdd(header.coverageStats.x, 1u);
        return;
    }

    ivec2 pixel = ivec2(int(tile.coverage.z & 0xffffu), int(tile.coverage.z >> 16u));
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= int(uResolution.x) || pixel.y >= int(uResolution.y)) {
        atomicAdd(header.coverageStats.w, 1u);
        return;
    }

    float depth = texelFetch(uDepthTex, pixel, 0).r;
    if (depth >= 1.0) {
        atomicAdd(header.coverageStats.w, 1u);
        return;
    }

    uint transformID = texelFetch(uTransformIDTex, pixel, 0).r;
    if (transformID == 0u || transformID >= uint(max(uMaxTransformID, 1))) {
        atomicAdd(header.coverageStats.w, 1u);
        return;
    }

    float deficit = max(imageLoad(uDeficitTex, pixel).r, 0.0);
    float severity = clamp(deficit / max(threshold, 0.001), 0.0, 1.0);

    uint lastSpawnFrame = tile.state.y > 0u ? (tile.state.y - 1u) : 0u;
    uint framesSinceSpawn = tile.state.y > 0u ? (frameIndex - min(lastSpawnFrame, frameIndex)) : 0xffffffffu;
    bool motionCriticalTile = (tile.state.z & (SURFEL_TILE_FLAG_HIGH_MOTION | SURFEL_TILE_FLAG_NEWLY_EXPOSED)) != 0u;
    float repeatSpawnSeverity = motionCriticalTile ? 0.45 : 0.70;
    if (framesSinceSpawn < 2u && severity < repeatSpawnSeverity) {
        atomicAdd(header.coverageStats.w, 1u);
        tileCoverage[tileIndex].stats.w += 1u;
        return;
    }

    float freeFraction = float(header.counts.z) / max(float(header.counts.x), 1.0);
    if (freeFraction <= uFreePoolReserveFraction && severity < 0.85) {
        atomicAdd(header.coverageStats.z, 1u);
        return;
    }

    atomicAdd(header.frameStats.w, 1u);

    if (!ReserveSpawnBudget(uint(max(uMaxSpawns, 1)))) {
        atomicAdd(header.queueStats.y, 1u);
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
        tileCoverage[tileIndex].stats.z += 1u;
        ReleaseSpawnReservation();
        return;
    }

    uint surfelID = AllocateSurfelID();
    if (surfelID >= header.counts.x) {
        atomicAdd(header.coverageStats.z, 1u);
        ReleaseSpawnReservation();
        return;
    }

    GpuTransformRecord transformRecord = transforms[transformID];
    mat4 world = transformRecord.world;
    mat4 invWorld = inverse(world);
    vec3 localPos = (invWorld * vec4(worldPos, 1.0)).xyz;
    vec3 localNormal = normalize(transpose(mat3(world)) * normal);

    uint spawnReason = severity > 0.55 ? SURFEL_SPAWN_REFINEMENT : SURFEL_SPAWN_COVERAGE_GAP;

    SurfelRecord s;
    s.worldPositionRadius = vec4(worldPos, radius);
    s.localPositionAge = vec4(localPos, 0.0);
    s.worldNormalRecycle = vec4(normal, 0.0);
    s.localNormalDebug = vec4(localNormal, float(spawnReason));
    s.ids = uvec4(transformID, SURFEL_FLAG_VALID, surfelID, HashUInt(surfelID ^ frameIndex ^ tileIndex));
    s.frames = uvec4(frameIndex, frameIndex, frameIndex, 0u);
    s.grid = uvec4(0u, 0u, 0u, SURFEL_STATE_ACTIVE);
    s.metrics = vec4(minCoverage, distance(worldPos, uCameraPos), projectedRadius, 1.0);
    s.irradianceHistory = vec4(0.0);
    s.shortTermStats = vec4(0.0);
    s.longTermStats = vec4(0.0);
    s.recycleData = vec4(0.0);
    s.depthMoments = vec4(0.0, 0.0, 0.0, radius);
    s.guidingState = vec4(0.0, 0.0, 0.0, float(transformRecord.metadata.z));
    s.rawIrradiance = vec4(0.0);
    s.sharedIrradiance = vec4(0.0);
    s.solveState = vec4(0.0, 0.0, 1.0, float(frameIndex));
    surfels[surfelID] = s;

    atomicAdd(header.counts.y, 1u);
    tileCoverage[tileIndex].state.y = frameIndex + 1u;
}

void main()
{
    uint candidateIndex = gl_GlobalInvocationID.x;
    if (candidateIndex >= uint(max(uMaxSpawnCandidates, 0))) {
        return;
    }

    uint tileIndex = 0u;
    if (!PopNextTile(tileIndex)) {
        return;
    }

    if (tileIndex >= max(header.tiling.x * header.tiling.y, 1u)) {
        return;
    }

    atomicAdd(header.tileWorkStats.w, 1u);
    uint frameIndex = uint(max(uFrameIndex, 0));
    SurfelTileMeta tile = tileCoverage[tileIndex];
    SpawnSurfelFromTile(tileIndex, tile, frameIndex);
}
