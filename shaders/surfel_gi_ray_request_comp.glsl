#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

layout(binding = 20, std430) buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 28, std430) buffer IrradianceHeaderBuffer {
    SurfelIrradianceHeader irradianceHeader;
};

uniform int uFrameIndex;
uniform int uSurfelStart;
uniform int uSurfelCount;
uniform int uMaxRaysPerSurfel;
uniform int uMaxRayTracedSurfels;
uniform int uGlobalRayBudget;
uniform int uMaxTransformID;
uniform int uSurfelGIValidationMode;
uniform int uFixedRaysPerSurfel;
uniform float uEffectiveBudgetScale;
uniform int uRTReady;
uniform int uDisableDormancy;

void StoreRejectedSurfel(uint id, SurfelRecord s)
{
    s.solveState = vec4(0.0, 0.0, 0.0, s.solveState.w);
    s.lightingState = vec4(
        float(SurfelLightingStateFromHistory(s)),
        SurfelHistoryConfidence(s),
        max(max(s.shortTermStats.y, s.longTermStats.y), 0.0),
        0.0);
    surfels[id] = s;
}

void main()
{
    uint localIndex = gl_GlobalInvocationID.x;
    uint surfelCount = uint(max(uSurfelCount, 0));
    if (localIndex >= surfelCount) {
        return;
    }

    if (header.counts.x == 0u) {
        atomicAdd(irradianceHeader.eligibilityReject4.x, 1u);
        return;
    }

    uint id = localIndex % header.counts.x;
    SurfelRecord s = surfels[id];
    if ((s.ids.y & SURFEL_FLAG_VALID) == 0u) {
        atomicAdd(irradianceHeader.eligibilityReject0.x, 1u);
        return;
    }

    bool controlledValidation = uSurfelGIValidationMode == SURFEL_GI_VALIDATION_MODE_BRUTE_FORCE;
    bool disableDormancy = controlledValidation || uDisableDormancy != 0;
    uint frameIndex = uint(max(uFrameIndex, 0));

    if ((s.ids.y & SURFEL_FLAG_TRANSFORM_INVALID) != 0u ||
        s.ids.x == 0u ||
        s.ids.x >= uint(max(uMaxTransformID, 1))) {
        atomicAdd(irradianceHeader.eligibilityReject0.y, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }
    if (s.grid.w != SURFEL_STATE_ACTIVE && s.grid.w != SURFEL_STATE_RECYCLABLE) {
        atomicAdd(irradianceHeader.eligibilityReject0.x, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }
    if (s.ids.z >= header.counts.x || s.ids.z != id) {
        atomicAdd(irradianceHeader.eligibilityReject4.x, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }

    vec3 normal = s.worldNormalRecycle.xyz;
    float normalLen = length(normal);
    if (normalLen < 0.25 || any(isnan(normal)) || any(isinf(normal))) {
        atomicAdd(irradianceHeader.eligibilityReject0.z, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }
    float radius = s.worldPositionRadius.w;
    if (radius <= 0.0 || isnan(radius) || isinf(radius)) {
        atomicAdd(irradianceHeader.eligibilityReject0.w, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }
    if (s.grid.x >= header.tiling.w || s.grid.z == 0u) {
        atomicAdd(irradianceHeader.eligibilityReject1.x, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }

    if (uEffectiveBudgetScale <= 0.0) {
        atomicAdd(irradianceHeader.eligibilityReject3.y, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }
    if (uMaxRayTracedSurfels <= 0) {
        atomicAdd(irradianceHeader.eligibilityReject3.z, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }
    if (uMaxRaysPerSurfel <= 0) {
        atomicAdd(irradianceHeader.eligibilityReject3.w, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }
    if (uGlobalRayBudget <= 0) {
        atomicAdd(irradianceHeader.eligibilityReject4.z, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }

    uint rayStart = uint(max(uSurfelStart, 0)) % max(header.counts.x, 1u);
    uint windowIndex = (id + header.counts.x - rayStart) % header.counts.x;
    bool outsideSelectionWindow = windowIndex >= uint(max(uMaxRayTracedSurfels, 0));

    uint ageFrames = frameIndex - min(s.frames.x, frameIndex);
    uint framesSinceVisible = frameIndex - min(s.frames.y, frameIndex);
    uint framesSinceContribution = frameIndex - min(s.frames.z, frameIndex);
    bool dormant = (s.ids.y & SURFEL_FLAG_DORMANT) != 0u;
    bool isLightingUninitialized = SurfelLightingIsUninitialized(s);
    float historyConfidence = SurfelHistoryConfidence(s);
    float historySamples = max(s.irradianceHistory.w, 0.0);
    if (historyConfidence <= 0.0001) {
        atomicAdd(irradianceHeader.eligibilityReject2.x, 1u);
    }
    if (historySamples <= 0.0) {
        atomicAdd(irradianceHeader.eligibilityReject2.y, 1u);
    }

    if (isLightingUninitialized) {
        dormant = false;
        s.ids.y &= ~SURFEL_FLAG_DORMANT;
    }

    float shortVariance = max(s.shortTermStats.y, 0.0);
    float longVariance = max(s.longTermStats.y, 0.0);
    float varianceSignal = clamp(sqrt(max(shortVariance, longVariance)) * 3.5, 0.0, 1.0);
    float instability = clamp(s.shortTermStats.z * 2.0, 0.0, 1.0);
    float newness = 1.0 - smoothstep(8.0, 64.0, float(ageFrames));
    float staleSolve = smoothstep(16.0, 180.0, float(framesSinceContribution));
    float recentVisibility = 1.0 - smoothstep(8.0, 180.0, float(framesSinceVisible));
    float historyDeficit = isLightingUninitialized ? 1.0 : (1.0 - smoothstep(4.0, 48.0, historySamples));
    float storedRelevance = clamp(s.metrics.w, 0.0, 1.0) * recentVisibility;
    float projectedRelevance = recentVisibility * smoothstep(0.75, 4.0, max(s.metrics.z, 0.0));
    float visibleRelevance = clamp(max(storedRelevance, projectedRelevance), 0.0, 1.0);
    bool projectedThisFrame = s.metrics.w > 0.95 && framesSinceVisible <= 2u;
    bool screenRelevant =
        projectedThisFrame ||
        (storedRelevance > 0.60 && framesSinceVisible <= 30u) ||
        (storedRelevance > 0.45 && framesSinceVisible <= 8u);
    bool bootstrapCritical =
        isLightingUninitialized &&
        (screenRelevant || projectedThisFrame || (framesSinceVisible <= 8u && storedRelevance > 0.40));
    bool bootstrapMaintenance = isLightingUninitialized && !bootstrapCritical;
    float rayBudgetPressure = clamp(
        float(max(uGlobalRayBudget, 1)) /
            max(float(max(header.counts.y, 1u)) * float(max(uMaxRaysPerSurfel, 1)), 1.0),
        0.0,
        1.0);

    float priority = newness * 0.45 +
        varianceSignal * 0.30 +
        instability * 0.20 +
        staleSolve * 0.10 +
        visibleRelevance * 0.15 +
        recentVisibility * 0.18 +
        historyDeficit * 0.35;
    if (dormant && !disableDormancy) {
        float wakeSignal = max(max(varianceSignal, newness), max(recentVisibility, historyDeficit));
        priority *= mix(0.18, 0.75, wakeSignal);
    }
    if (bootstrapCritical) {
        priority = max(priority, mix(0.35, 0.90, visibleRelevance));
    } else if (bootstrapMaintenance) {
        priority = max(priority, 0.08 + visibleRelevance * 0.18);
    } else if (max(recentVisibility, historyDeficit) > 0.35) {
        priority = max(priority, 0.16);
    }

    bool recentLightingRelevant =
        framesSinceContribution <= 45u &&
        (visibleRelevance > 0.20 || historyDeficit > 0.50 || varianceSignal > 0.20 || instability > 0.10);
    bool priorityBypassesSelectionCap =
        controlledValidation ||
        bootstrapCritical ||
        screenRelevant ||
        recentLightingRelevant ||
        (historyDeficit > 0.35 && (screenRelevant || projectedThisFrame || storedRelevance > 0.35 || (!isLightingUninitialized && framesSinceContribution <= 45u))) ||
        varianceSignal > 0.35 ||
        instability > 0.18;
    if (outsideSelectionWindow && !priorityBypassesSelectionCap) {
        atomicAdd(irradianceHeader.eligibilityReject4.z, 1u);
        StoreRejectedSurfel(id, s);
        return;
    }

    bool outsideResidency = !isLightingUninitialized && s.metrics.y > max(header.gridParams.z * 2.25, 64.0);
    if (outsideResidency) {
        atomicAdd(irradianceHeader.eligibilityReject1.z, 1u);
        s.solveState = vec4(0.0, 0.0, 0.0, s.solveState.w);
        s.lightingState = vec4(float(SurfelLightingStateFromHistory(s)), historyConfidence, max(shortVariance, longVariance), 0.0);
        surfels[id] = s;
        return;
    }

    bool staleAndUnimportant =
        !isLightingUninitialized &&
        framesSinceVisible > 360u &&
        framesSinceContribution > 240u &&
        visibleRelevance < 0.08;
    if (staleAndUnimportant) {
        atomicAdd(irradianceHeader.eligibilityReject1.y, 1u);
        s.solveState = vec4(0.0, 0.0, 0.0, s.solveState.w);
        s.lightingState = vec4(float(SurfelLightingStateFromHistory(s)), historyConfidence, max(shortVariance, longVariance), 0.0);
        surfels[id] = s;
        return;
    }

    uint maxRays = uint(clamp(uMaxRaysPerSurfel, 1, 32));
    uint fixedRays = uint(clamp(uFixedRaysPerSurfel, 1, 8));
    uint bootstrapRayFloor = historySamples < 4.0 ? min(maxRays, 2u) : 1u;
    bool alreadySolved =
        !isLightingUninitialized &&
        !controlledValidation &&
        SurfelLightingStateFromHistory(s) == SURFEL_LIGHTING_STATE_STABLE &&
        framesSinceContribution < 12u &&
        max(shortVariance, longVariance) < 0.012;
    uint requested = controlledValidation
        ? fixedRays
        : (alreadySolved ? 0u : (priority > 0.025 ? uint(clamp(ceil(priority * float(maxRays)), 1.0, float(maxRays))) : 0u));
    bool budgetCritical =
        controlledValidation ||
        bootstrapCritical ||
        screenRelevant ||
        (historyDeficit > 0.65 && (screenRelevant || projectedThisFrame || storedRelevance > 0.35 || (!isLightingUninitialized && framesSinceContribution <= 45u))) ||
        varianceSignal > 0.45 ||
        instability > 0.22;
    if (!budgetCritical && requested > 0u) {
        float requestChance = clamp(priority * mix(0.18, 1.35, rayBudgetPressure), 0.0, 1.0);
        if (bootstrapMaintenance) {
            requestChance = min(requestChance, mix(0.012, 0.12, rayBudgetPressure) * max(visibleRelevance, 0.20));
        }
        float lottery = SpatioTemporalBlueNoise01(s.ids.z ^ 0x6a09e667u, frameIndex);
        if (lottery > requestChance) {
            requested = 0u;
        }
    }
    if (!controlledValidation && bootstrapCritical) {
        requested = max(requested, bootstrapRayFloor);
    }
    if (dormant && !disableDormancy && !isLightingUninitialized && requested == 0u) {
        atomicAdd(irradianceHeader.eligibilityReject1.w, 1u);
    } else if (alreadySolved && requested == 0u) {
        atomicAdd(irradianceHeader.eligibilityReject2.z, 1u);
    } else if (requested == 0u) {
        atomicAdd(irradianceHeader.eligibilityReject4.z, 1u);
    }

    s.solveState = vec4(float(requested), 0.0, dormant ? 0.0 : max(priority, 0.001), s.solveState.w);
    s.lightingState = vec4(
        float(SurfelLightingStateFromHistory(s)),
        historyConfidence,
        max(shortVariance, longVariance),
        isLightingUninitialized ? 1.0 : 0.0);
    surfels[id] = s;

    if (requested > 0u) {
        atomicAdd(irradianceHeader.rayStats.x, requested);
        atomicAdd(irradianceHeader.rayStats.z, 1u);
    }
    atomicAdd(irradianceHeader.rayStats.w, 1u);
    if (isLightingUninitialized) {
        atomicAdd(irradianceHeader.debugStats.x, 1u);
    }
    if (dormant) {
        atomicAdd(irradianceHeader.debugStats.y, 1u);
    }
    if (uRTReady == 0) {
        atomicAdd(irradianceHeader.eligibilityReject4.y, 1u);
    }
}
