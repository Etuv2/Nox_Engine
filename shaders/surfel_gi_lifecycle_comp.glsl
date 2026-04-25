#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

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

layout(binding = 25, std430) readonly buffer GridHeaderBuffer {
    uvec4 gridHeaders[];
};

layout(binding = 26, std430) readonly buffer GridEntryBuffer {
    uint gridEntries[];
};

uniform int uFrameIndex;
uniform int uMaxTransformID;
uniform vec2 uResolution;
uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraPos;
uniform float uTargetRadiusPixels;
uniform float uCoverageThreshold;
uniform float uRecyclePressure;
uniform float uFreePoolReserveFraction;
uniform int uSurfelStart;
uniform int uSurfelCount;

float EstimateLocalCoverage(uint selfID, SurfelRecord self)
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

void main()
{
    uint localIndex = gl_GlobalInvocationID.x;
    uint surfelCount = uint(max(uSurfelCount, 0));
    if (localIndex >= surfelCount || header.counts.x == 0u) {
        return;
    }

    uint id = (uint(max(uSurfelStart, 0)) + localIndex) % header.counts.x;

    SurfelRecord s = surfels[id];
    if (!IsSurfelValid(s)) {
        return;
    }

    uint frameIndex = uint(max(uFrameIndex, 0));
    if (s.ids.z != id) {
        atomicAdd(header.recycleStats.w, 1u);
        s.ids.y |= SURFEL_FLAG_TRANSFORM_INVALID;
        s.grid.w = SURFEL_STATE_RECYCLABLE;
        surfels[id] = s;
        return;
    }

    uint transformID = s.ids.x;
    if (transformID == 0u || transformID >= uint(max(uMaxTransformID, 1))) {
        s.ids.y |= SURFEL_FLAG_TRANSFORM_INVALID;
        s.worldNormalRecycle.w = 1.0;
        s.metrics.w = 0.0;
        s.recycleData = vec4(1.0, float(SURFEL_RECYCLE_INVALID_TRANSFORM), 1.0, 1.0);
        s.grid.w = SURFEL_STATE_RECYCLABLE;
        surfels[id] = s;
        return;
    }

    GpuTransformRecord transformRecord = transforms[transformID];
    if (s.guidingState.w >= 0.0 && uint(s.guidingState.w + 0.5) != transformRecord.metadata.z) {
        s.ids.y |= SURFEL_FLAG_TRANSFORM_INVALID;
        s.worldNormalRecycle.w = 1.0;
        s.metrics.w = 0.0;
        s.recycleData = vec4(1.0, float(SURFEL_RECYCLE_INVALID_TRANSFORM), 1.0, 1.0);
        s.grid.w = SURFEL_STATE_RECYCLABLE;
        surfels[id] = s;
        return;
    }

    mat4 world = transformRecord.world;
    vec4 followedPos = world * vec4(s.localPositionAge.xyz, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(world)));
    vec3 followedNormal = normalize(normalMatrix * s.localNormalDebug.xyz);

    vec4 viewPos = uView * followedPos;
    float targetRadiusPixels = ResolveTargetRadiusPixels(uTargetRadiusPixels, float(header.tiling.z), uCoverageThreshold);
    float desiredRadius = ComputeWorldRadiusForProjectedPixels(viewPos.z, targetRadiusPixels, uResolution.y, uProjection);
    float radius = clamp(desiredRadius, 0.005, 8.0);

    s.worldPositionRadius = vec4(followedPos.xyz, radius);
    s.worldNormalRecycle.xyz = followedNormal;
    s.localPositionAge.w += 1.0;

    vec4 clip = uProjection * viewPos;
    bool inFrustum = false;
    if (clip.w > 0.0) {
        vec3 ndc = clip.xyz / clip.w;
        inFrustum = all(greaterThanEqual(ndc, vec3(-1.15))) && all(lessThanEqual(ndc, vec3(1.15)));
    }
    if (inFrustum) {
        s.frames.y = frameIndex;
    }
    float cameraDistance = distance(followedPos.xyz, uCameraPos);
    float age = max(s.localPositionAge.w, 1.0);
    float framesSinceVisible = float(frameIndex - min(s.frames.y, frameIndex));
    float framesSinceContributing = float(frameIndex - min(s.frames.z, frameIndex));
    float visibleProtectionFrames = 120.0;
    bool recentlyVisible = framesSinceVisible < visibleProtectionFrames;
    bool recentlyContributed = framesSinceContributing < 90.0;
    float userRecyclePressure = clamp(uRecyclePressure, 0.0, 2.0);
    float freeFraction = float(header.counts.z) / max(float(header.counts.x), 1.0);
    float capacityPressure = clamp(1.0 - freeFraction * 2.0, 0.0, 1.0);
    float reservePressure = clamp((max(uFreePoolReserveFraction, 0.001) - freeFraction) / max(uFreePoolReserveFraction, 0.001), 0.0, 1.0);
    float emergencyPressure = smoothstep(0.018, 0.0, freeFraction);
    float poolPressure = max(reservePressure, emergencyPressure);
    float ageScore = smoothstep(120.0, 600.0, age);
    float staleContributionScore = smoothstep(45.0, 240.0, framesSinceContributing);
    float staleVisibilityScore = smoothstep(60.0, 210.0, framesSinceVisible);
    float dormantScore = max(staleContributionScore, staleVisibilityScore);
    float distanceScore = smoothstep(64.0, 240.0, cameraDistance);
    float projectedRadius = max(radius * uResolution.y * abs(uProjection[1][1]) / max(abs(viewPos.z), 0.05), 0.0);
    float surfelPixels = max(3.14159265 * projectedRadius * projectedRadius, 1.0);
    float tilePixels = 256.0;
    float idealSurfelsForTile = max(tilePixels / surfelPixels, 1.0);
    bool wasDormant = (s.ids.y & SURFEL_FLAG_DORMANT) != 0u;
    bool refreshCoverage = (!wasDormant && ((id ^ frameIndex) & 7u) == 0u) || reservePressure > 0.35;
    float screenCoverageRatio = max(s.metrics.x, 0.0);
    if (refreshCoverage) {
        float localCoverage = EstimateLocalCoverage(id, s);
        screenCoverageRatio = localCoverage / idealSurfelsForTile;
    }
    float oversubScore = smoothstep(1.05, 2.1, screenCoverageRatio);
    float youngProtection = 1.0 - smoothstep(0.0, 90.0, age);
    youngProtection *= 1.0 - poolPressure;
    float stalePressure = recentlyContributed ? reservePressure * 0.25 : max(capacityPressure, reservePressure);
    float visibleOversubPressure = (recentlyVisible && !recentlyContributed) ? smoothstep(2.6, 4.4, screenCoverageRatio) : 0.0;
    float persistentProtection = recentlyContributed ? mix(0.45, 0.10, reservePressure) : (recentlyVisible ? mix(0.18, 0.03, reservePressure) : 0.0);
    float recyclePriority = clamp(
        stalePressure * 0.58 +
        dormantScore * 0.36 +
        ageScore * poolPressure * 0.72 +
        distanceScore * (recentlyContributed ? reservePressure * 0.08 : 0.28) +
        oversubScore * (recentlyContributed ? mix(0.04, 0.68, emergencyPressure) : 0.44) +
        visibleOversubPressure * 0.72 -
        youngProtection * mix(0.55, 0.08, emergencyPressure) -
        persistentProtection,
        0.0,
        1.0);
    float recycleScore = clamp(recyclePriority * poolPressure * max(userRecyclePressure, 0.0), 0.0, 1.0);
    uint recycleReason = SURFEL_RECYCLE_NONE;
    if (recycleScore > 0.05) {
        if (oversubScore > 0.35) {
            recycleReason = SURFEL_RECYCLE_OVERSAMPLED;
        } else if (poolPressure > 0.15) {
            recycleReason = SURFEL_RECYCLE_POOL_PRESSURE;
        } else {
            recycleReason = SURFEL_RECYCLE_STALE;
        }
    }

    s.metrics.x = screenCoverageRatio;
    s.metrics.y = cameraDistance;
    s.metrics.z = projectedRadius;
    s.metrics.w = 1.0 - recycleScore;
    s.worldNormalRecycle.w = recycleScore;
    s.recycleData = vec4(recycleScore, float(recycleReason), oversubScore, poolPressure);
    s.grid.w = recycleScore > 0.05 ? SURFEL_STATE_RECYCLABLE : SURFEL_STATE_ACTIVE;

    if (framesSinceContributing > 120.0 || framesSinceVisible > 300.0) {
        s.ids.y |= SURFEL_FLAG_DORMANT;
        atomicAdd(header.frameStats.z, 1u);
    } else {
        s.ids.y &= ~SURFEL_FLAG_DORMANT;
    }

    if (recycleScore > 0.05) {
        atomicAdd(header.recycleStats.x, 1u);
    }

    atomicAdd(header.budgetStats.y, 1u);

    surfels[id] = s;
}
