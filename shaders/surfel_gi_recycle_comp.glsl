#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 22, std430) buffer FreeStackBuffer {
    uint freeStack[];
};

layout(binding = 25, std430) readonly buffer GridHeaderBuffer { uvec4 gridHeaders[]; };
layout(binding = 26, std430) readonly buffer GridEntryBuffer { uint gridEntries[]; };

uniform int uFrameIndex;
uniform float uFreePoolReserveFraction;
uniform float uRecyclePressure;
uniform float uCoverageDemandPressure;

float EstimateCurrentGridCoverage(uint selfID, SurfelRecord self)
{
    uint cell = self.grid.x;
    if (cell >= header.tiling.w) {
        return 0.0;
    }

    uint count = min(gridHeaders[cell].x, header.gridDims.w);
    float coverage = 0.0;
    for (uint i = 0u; i < count; ++i) {
        uint surfelID = gridEntries[cell * header.gridDims.w + i];
        if (surfelID == selfID || surfelID >= header.counts.x) {
            continue;
        }

        SurfelRecord other = surfels[surfelID];
        if (!IsSurfelValid(other)) {
            continue;
        }

        float normalAlign = dot(self.worldNormalRecycle.xyz, other.worldNormalRecycle.xyz);
        if (normalAlign < 0.65) {
            continue;
        }

        vec3 delta = self.worldPositionRadius.xyz - other.worldPositionRadius.xyz;
        float planeDistance = abs(dot(delta, other.worldNormalRecycle.xyz));
        vec3 tangentDelta = delta - other.worldNormalRecycle.xyz * dot(delta, other.worldNormalRecycle.xyz);
        float tangentDistance = length(tangentDelta);
        float radius = max(other.worldPositionRadius.w, 0.001);
        float coverageRadius = radius * 1.35;
        coverage += (1.0 - smoothstep(coverageRadius * 0.45, coverageRadius, tangentDistance)) *
            (1.0 - smoothstep(0.0, coverageRadius * 0.45, planeDistance)) *
            clamp(normalAlign, 0.0, 1.0);
    }

    return coverage;
}

void PushFreeSurfelID(uint id)
{
    uint slot = atomicAdd(header.counts.z, 1u);
    if (slot < header.counts.x) {
        freeStack[slot] = id;
    } else {
        atomicExchange(header.counts.z, header.counts.x);
    }

    uint liveCountBefore = header.counts.y;
    while (liveCountBefore > 0u) {
        uint previous = atomicCompSwap(header.counts.y, liveCountBefore, liveCountBefore - 1u);
        if (previous == liveCountBefore) {
            break;
        }
        liveCountBefore = previous;
    }
}

void RecyclePersistentSurfel(uint id, uint frameIndex, uint recycleReason)
{
    if (surfels[id].grid.w != SURFEL_STATE_RECYCLABLE) {
        atomicAdd(header.recycleStats.w, 1u);
        return;
    }

    uint oldFlags = atomicExchange(surfels[id].ids.y, SURFEL_FLAG_RECYCLED);
    if ((oldFlags & SURFEL_FLAG_VALID) == 0u || (oldFlags & SURFEL_FLAG_RECYCLED) != 0u) {
        atomicAdd(header.recycleStats.w, 1u);
        return;
    }

    surfels[id].ids.y = SURFEL_FLAG_RECYCLED;
    surfels[id].frames.w = frameIndex;
    surfels[id].worldNormalRecycle.w = 1.0;
    surfels[id].recycleData.x = 1.0;
    surfels[id].recycleData.y = float(recycleReason);
    surfels[id].grid.w = SURFEL_STATE_DEAD;
    PushFreeSurfelID(id);

    atomicAdd(header.frameStats.y, 1u);
    if (recycleReason == SURFEL_RECYCLE_INVALID_TRANSFORM) {
        atomicAdd(header.recycleStats.z, 1u);
    } else {
        atomicAdd(header.recycleStats.y, 1u);
    }
}

void main()
{
    uint id = gl_GlobalInvocationID.x;
    if (id >= header.counts.x) {
        return;
    }

    SurfelRecord s = surfels[id];
    if (!IsSurfelValid(s)) {
        return;
    }

    uint frameIndex = uint(max(uFrameIndex, 0));
    bool invalidTransform = (s.ids.y & SURFEL_FLAG_TRANSFORM_INVALID) != 0u;
    if (invalidTransform) {
        surfels[id].grid.w = SURFEL_STATE_RECYCLABLE;
        RecyclePersistentSurfel(id, frameIndex, SURFEL_RECYCLE_INVALID_TRANSFORM);
        return;
    }

    float freeFraction = float(header.counts.z) / max(float(header.counts.x), 1.0);
    float reserve = max(uFreePoolReserveFraction, 0.001);
    float reservePressure = clamp((reserve - freeFraction) / reserve, 0.0, 1.0);
    float emergencyPressure = smoothstep(0.018, 0.0, freeFraction);
    float tileCount = max(float(header.tiling.x * header.tiling.y), 1.0);
    float frameAttemptPressure = clamp((float(header.frameStats.w) / tileCount - 0.002) / 0.045, 0.0, 1.0);
    float frameRejectedPressure = clamp((float(header.coverageStats.w) / tileCount - 0.001) / 0.030, 0.0, 1.0);
    float frameExhaustedPressure = header.coverageStats.z > 0u ? 1.0 : 0.0;
    float frameCoverageDemandPressure = clamp(
        frameAttemptPressure * 0.80 +
        frameRejectedPressure * 0.55 +
        frameExhaustedPressure,
        0.0,
        1.0);
    float demandPressure = max(clamp(uCoverageDemandPressure, 0.0, 1.0), frameCoverageDemandPressure);
    bool poolStall = header.coverageStats.z > 0u || freeFraction <= 0.0005 || freeFraction < reserve * 0.5;
    float userRecyclePressure = clamp(uRecyclePressure, 0.0, 2.0);
    float poolPressure = clamp(max(max(reservePressure, emergencyPressure), demandPressure) * userRecyclePressure, 0.0, 1.0);
    if (poolPressure <= 0.001) {
        return;
    }

    float age = max(s.localPositionAge.w, 1.0);
    float framesSinceVisible = float(frameIndex - min(s.frames.y, frameIndex));
    float framesSinceContributing = float(frameIndex - min(s.frames.z, frameIndex));
    bool farFromCamera = s.metrics.y > mix(180.0, 72.0, demandPressure);
    float offscreenScore = smoothstep(45.0, 180.0, framesSinceVisible);
    float distanceScore = smoothstep(64.0, 240.0, s.metrics.y);
    float projectedRadius = max(s.metrics.z, 0.0);
    float surfelPixels = max(3.14159265 * projectedRadius * projectedRadius, 1.0);
    float idealSurfelsForTile = max(256.0 / surfelPixels, 1.0);
    float currentCoverage = EstimateCurrentGridCoverage(id, s);
    float currentCoverageRatio = currentCoverage / idealSurfelsForTile;
    float oversubScore = smoothstep(1.05, 2.1, currentCoverageRatio);
    float demandReclaimScore = demandPressure * (offscreenScore * 0.58 + distanceScore * 0.42 + oversubScore * 0.52);
    float recycleScore = clamp(max(s.worldNormalRecycle.w, s.recycleData.x) + oversubScore * poolPressure * 0.36 + demandReclaimScore, 0.0, 1.0);

    surfels[id].metrics.x = currentCoverageRatio;
    surfels[id].recycleData.z = oversubScore;
    surfels[id].recycleData.w = poolPressure;

    float stallPressure = clamp(max(max(reservePressure, emergencyPressure), demandPressure), 0.0, 1.0);
    float visibleProtectionFrames = poolStall ? mix(24.0, 6.0, stallPressure) : mix(240.0, 36.0, demandPressure);
    float contributionProtectionFrames = poolStall ? mix(18.0, 4.0, stallPressure) : 120.0;
    bool protectedByRecentVisibility = framesSinceVisible <= visibleProtectionFrames;
    bool protectedByRecentContribution = framesSinceContributing <= contributionProtectionFrames;
    bool pressureAllowsReclaim = demandPressure > 0.35 || reservePressure > 0.35 || emergencyPressure > 0.35;
    bool demandAllowsReclaim = demandPressure > 0.08;
    bool demandReclaimOldFarCoverage = demandAllowsReclaim &&
        farFromCamera &&
        framesSinceVisible > mix(90.0, 18.0, demandPressure) &&
        framesSinceContributing > mix(60.0, 12.0, demandPressure);
    bool staleEnough =
        (!protectedByRecentVisibility && !protectedByRecentContribution && framesSinceContributing > 150.0) ||
        demandReclaimOldFarCoverage ||
        (pressureAllowsReclaim && farFromCamera && framesSinceVisible > 45.0 && framesSinceContributing > 30.0) ||
        (reservePressure > 0.55 && framesSinceVisible > 180.0 && framesSinceContributing > 90.0) ||
        (age > 480.0 && framesSinceVisible > 300.0 && framesSinceContributing > 180.0);
    bool redundantEnough = oversubScore > mix(0.82, 0.46, demandPressure) &&
        age > mix(120.0, 45.0, demandPressure) &&
        framesSinceContributing > mix(120.0, 18.0, demandPressure) &&
        (!protectedByRecentVisibility || farFromCamera || offscreenScore > 0.18);
    bool emergency = emergencyPressure > 0.75 &&
        age > 90.0 &&
        (!protectedByRecentVisibility || oversubScore > 0.92);
    float targetFreeReserve = max(reserve * 0.55, 0.006);
    bool emergencyReserveRefill = poolStall && freeFraction < targetFreeReserve;
    bool stallRecycleCandidate =
        farFromCamera ||
        offscreenScore > 0.03 ||
        redundantEnough ||
        oversubScore > mix(0.46, 0.18, stallPressure) ||
        (!protectedByRecentContribution && framesSinceVisible > mix(18.0, 2.0, stallPressure));
    float stallRecycleHash = Hash01(HashUInt(id ^ (frameIndex * 1103515245u) ^ 0x9e3779b9u));
    float emergencyRefillProbability = emergencyReserveRefill
        ? mix(0.035, 0.24, stallPressure) * (farFromCamera ? 1.0 : 0.45)
        : 0.0;
    bool emergencyRefillAllowed =
        emergencyReserveRefill &&
        stallRecycleCandidate &&
        age > mix(30.0, 6.0, stallPressure) &&
        stallRecycleHash < emergencyRefillProbability;
    bool allowedByPersistence = staleEnough || redundantEnough || emergency || emergencyRefillAllowed;
    if (!allowedByPersistence) {
        return;
    }

    bool forceRecycle = poolStall && (
        emergencyRefillAllowed ||
        demandReclaimOldFarCoverage ||
        (redundantEnough && (farFromCamera || offscreenScore > 0.12)) ||
        (!protectedByRecentVisibility && framesSinceContributing > mix(45.0, 12.0, stallPressure)) ||
        (oversubScore > mix(0.55, 0.22, stallPressure) && framesSinceContributing > mix(60.0, 15.0, stallPressure)) ||
        (freeFraction <= 0.0005 &&
            age > mix(18.0, 8.0, stallPressure) &&
            farFromCamera &&
            framesSinceVisible > mix(12.0, 3.0, stallPressure)));

    uint recycleReason = SURFEL_RECYCLE_POOL_PRESSURE;
    if (emergencyRefillAllowed) {
        recycleReason = SURFEL_RECYCLE_POOL_PRESSURE;
    } else if (redundantEnough || oversubScore > 0.35) {
        recycleReason = SURFEL_RECYCLE_OVERSAMPLED;
    } else if (staleEnough) {
        recycleReason = SURFEL_RECYCLE_STALE;
    }
    surfels[id].grid.w = SURFEL_STATE_RECYCLABLE;
    surfels[id].recycleData.y = float(recycleReason);

    float recycleRand = Hash01(HashUInt(id ^ (frameIndex * 747796405u)));
    float demandRecycleFloor = demandPressure * (
        (demandReclaimOldFarCoverage ? 0.58 : 0.0) +
        (redundantEnough ? 0.46 : 0.0) +
        offscreenScore * 0.20);
    bool recycle = forceRecycle ||
        recycleScore > recycleRand ||
        demandRecycleFloor > recycleRand ||
        (emergency && recycleScore > 0.18);
    if (recycle) {
        RecyclePersistentSurfel(id, frameIndex, recycleReason);
    }
}
