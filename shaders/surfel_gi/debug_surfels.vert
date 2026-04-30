#version 460 core

#include "common.glsl"

layout(binding = B_SURFELS, std430) readonly buffer SurfelBuffer
{
    Surfel surfels[];
};

layout(binding = B_RAY_REQUESTS, std430) readonly buffer SurfelRayRequestBuffer
{
    SurfelRayRequest rayRequests[];
};

layout(binding = B_RADIAL_DEPTH, std430) readonly buffer SurfelRadialDepthBuffer
{
    RadialDepthTexel radialDepth[];
};

layout(binding = B_GUIDE_MAP, std430) readonly buffer SurfelGuideMapBuffer
{
    GuideCell guideMap[];
};

uniform mat4 uView;
uniform mat4 uProjection;
uniform uint uMaxSurfels;
uniform uint uDebugView;
uniform uvec3 uGridResolution;
uniform vec3 uGridMin;
uniform vec3 uGridMax;
uniform vec2 uViewportSize;

out vec4 vColor;
out vec4 vStyle;
out vec4 vDisk0;
out vec4 vDisk1;
out vec4 vDisk2;

const uint DEBUG_SURFEL_SPHERES = 1u;
const uint DEBUG_SURFEL_NORMALS = 2u;
const uint DEBUG_SURFEL_AGE = 3u;
const uint DEBUG_SURFEL_VARIANCE = 4u;
const uint DEBUG_SURFEL_COVERAGE = 5u;
const uint DEBUG_SURFEL_GRID_CELLS = 6u;
const uint DEBUG_CELL_OCCUPANCY = 7u;
const uint DEBUG_SPAWN_RECYCLE = 8u;
const uint DEBUG_RAY_COUNTS = 9u;
const uint DEBUG_RAY_GUIDE = 10u;
const uint DEBUG_RADIAL_DEPTH = 11u;
const uint DEBUG_INDIRECT_ONLY = 12u;
const uint DEBUG_RECYCLE_SCORE = 15u;
const uint DEBUG_STALE_SURFELS = 16u;

float Safe01(float value)
{
    if (isnan(value) || isinf(value)) {
        return 0.0;
    }
    return clamp(value, 0.0, 1.0);
}

vec3 SafePositive(vec3 value)
{
    if (any(isnan(value)) || any(isinf(value))) {
        return vec3(0.0);
    }
    return max(value, vec3(0.0));
}

vec3 Heat(float value)
{
    float v = Safe01(value);
    return clamp(vec3(1.5 * v, 1.5 * (1.0 - abs(v - 0.5) * 2.0), 1.5 * (1.0 - v)), vec3(0.0), vec3(1.0));
}

vec3 HashColor(uint value)
{
    uint hash = SurfelHash(value);
    return vec3(
        0.20 + 0.80 * float(hash & 0xffu) / 255.0,
        0.20 + 0.80 * float((hash >> 8u) & 0xffu) / 255.0,
        0.20 + 0.80 * float((hash >> 16u) & 0xffu) / 255.0
    );
}

vec3 TonemapDebug(vec3 hdr)
{
    vec3 value = SafePositive(hdr);
    return value / (value + vec3(1.0));
}

void HidePoint()
{
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    gl_PointSize = 0.0;
    vColor = vec4(0.0);
    vStyle = vec4(0.0);
    vDisk0 = vec4(0.0);
    vDisk1 = vec4(0.0);
    vDisk2 = vec4(0.0);
}

float VarianceSignal(Surfel surfel)
{
    float storedVariance = max(surfel.debug.x, 0.0);
    float m2Variance = SurfelLuminance(SafePositive(surfel.shortM2.rgb));
    return Safe01(1.0 - exp(-max(storedVariance, m2Variance) * 8.0));
}

vec3 LiveSurfelColor(Surfel surfel)
{
    vec3 albedo = max(SafePositive(surfel.albedo_life.rgb), vec3(0.04));
    float confidence = Safe01(surfel.irradiance.a);
    return albedo * mix(0.45, 1.0, confidence);
}

vec3 LifecycleColor(Surfel surfel, bool alive, bool recycleMarker, out float ring)
{
    ring = 0.0;
    if (recycleMarker) {
        ring = 1.0;
        return vec3(1.0, 0.12, 0.03);
    }

    bool isNew = SurfelIsNew(surfel) || surfel.worldNormal_age.w < 2.0;
    if (isNew) {
        ring = 0.85;
        return vec3(0.10, 1.0, 0.32);
    }

    if (alive && surfel.ids.w > 0u) {
        ring = 0.45;
        return mix(vec3(0.30, 0.45, 1.0), vec3(0.95, 0.30, 1.0), Safe01(float(min(surfel.ids.w, 8u)) / 8.0));
    }

    return vec3(0.18, 0.58, 1.0);
}

vec3 RayCountColor(uint surfelID, Surfel surfel, out float ring)
{
    SurfelRayRequest request = rayRequests[surfelID];
    bool requestValid = request.surfelID == surfelID;
    float requested = requestValid ? float(request.requestedCount) : max(surfel.shortM2.a, 0.0);
    float allocated = requestValid ? float(request.allocatedCount) : max(surfel.debug.z, 0.0);
    float demand = Safe01(requested / 12.0);
    float allocatedSignal = Safe01(allocated / 12.0);
    float ratio = requested <= 0.0 ? 1.0 : Safe01(allocated / max(requested, 1.0));

    ring = requested > allocated ? 0.75 : 0.0;
    if (requested <= 0.0 && allocated <= 0.0) {
        return vec3(0.06, 0.08, 0.16);
    }

    vec3 ratioColor = mix(vec3(1.0, 0.08, 0.03), vec3(0.08, 0.95, 0.38), ratio);
    vec3 demandColor = mix(vec3(0.08, 0.18, 0.55), ratioColor, max(demand, allocatedSignal));
    return demandColor;
}

vec4 GuideStats(uint surfelID)
{
    vec3 mappedRadiance = vec3(0.0);
    float confidence = 0.0;
    float populated = 0.0;
    uint base = surfelID * SURFEL_GUIDE_CELLS;

    for (uint i = 0u; i < SURFEL_GUIDE_CELLS; ++i) {
        uint packedGuideRadiance = guideMap[base + i].packedRadiance;
        if (packedGuideRadiance == 0u) {
            continue;
        }

        vec4 cell = unpackUnorm4x8(packedGuideRadiance);
        mappedRadiance += cell.rgb;
        confidence += cell.a;
        populated += 1.0;
    }

    if (populated <= 0.0) {
        return vec4(0.0);
    }

    return vec4(mappedRadiance / populated, Safe01(confidence / populated));
}

vec3 GuideConfidenceColor(uint surfelID, Surfel surfel, out float ring)
{
    vec4 guide = GuideStats(surfelID);
    float surfelConfidence = Safe01(surfel.irradiance.a);
    float guideConfidence = guide.a;
    float confidence = max(surfelConfidence, guideConfidence);
    float guideSignal = Safe01(SurfelLuminance(guide.rgb) * 2.0 + guideConfidence);

    ring = 1.0 - confidence;
    vec3 confidenceColor = mix(vec3(0.50, 0.08, 0.78), vec3(0.05, 0.85, 1.0), confidence);
    return mix(confidenceColor, vec3(1.0, 0.86, 0.22), guideSignal * 0.45);
}

vec3 RadialDepthColor(uint surfelID, Surfel surfel, out float ring)
{
    float defaultDepth = max(surfel.worldPos_radius.w * 2.0, 0.001);
    float valid = 0.0;
    float rejectionSum = 0.0;
    float varianceSum = 0.0;
    uint base = surfelID * SURFEL_RADIAL_DEPTH_TEXELS;

    for (uint bin = 0u; bin < SURFEL_RADIAL_DEPTH_TEXELS; ++bin) {
        RadialDepthTexel texel = radialDepth[base + bin];
        if (!SurfelRadialDepthMomentsValid(texel)) {
            continue;
        }

        float mean = max(texel.meanDepth, 0.0);
        float variance = max(texel.meanDepthSq - mean * mean, 0.0);
        float visibility = SurfelRadialDepthVisibility(texel, defaultDepth, 1e-4);
        rejectionSum += 1.0 - visibility;
        varianceSum += variance / max(defaultDepth * defaultDepth, 1e-4);
        valid += 1.0;
    }

    if (valid <= 0.0) {
        ring = 0.8;
        return vec3(0.12, 0.12, 0.14);
    }

    float rejection = Safe01(rejectionSum / valid);
    float variance = Safe01((varianceSum / valid) * 4.0);
    float coverage = Safe01(valid / float(SURFEL_RADIAL_DEPTH_TEXELS));
    ring = rejection;

    vec3 rejectionColor = mix(vec3(0.08, 0.22, 0.78), Heat(rejection), max(coverage, 0.25));
    return mix(rejectionColor, vec3(1.0, 0.0, 0.9), variance * 0.30);
}

vec3 CoverageProxyColor(Surfel surfel)
{
    float radiusSignal = Safe01(surfel.worldPos_radius.w / max(surfel.localPos_spawnRadius.w * 2.0, 0.001));
    float life = Safe01(surfel.albedo_life.a);
    return mix(vec3(0.05, 0.12, 0.30), Heat(radiusSignal), life);
}

vec3 RecycleScoreColor(Surfel surfel, out float ring)
{
    float score = Safe01(surfel.debug.w);
    ring = score;
    return mix(vec3(0.05, 0.55, 0.20), vec3(1.0, 0.05, 0.02), score);
}

vec3 StaleColor(Surfel surfel, out float ring)
{
    float age = Safe01(max(surfel.worldNormal_age.w - surfel.shortMean.a, 0.0) / 240.0);
    float contributionSignal = 1.0 - Safe01(surfel.irradiance.a);
    float stale = max(age, contributionSignal);
    ring = stale;
    return mix(vec3(0.05, 0.18, 0.65), vec3(1.0, 0.70, 0.05), stale);
}

vec4 DebugColor(uint surfelID, Surfel surfel, bool alive, bool recycleMarker)
{
    float ring = 0.0;
    vec3 color;

    if (uDebugView == DEBUG_SURFEL_NORMALS) {
        color = SurfelSafeNormalize(surfel.worldNormal_age.xyz) * 0.5 + 0.5;
    } else if (uDebugView == DEBUG_SURFEL_AGE) {
        color = Heat(surfel.worldNormal_age.w / 240.0);
    } else if (uDebugView == DEBUG_SURFEL_VARIANCE) {
        float signal = VarianceSignal(surfel);
        ring = signal;
        color = Heat(signal);
    } else if (uDebugView == DEBUG_SURFEL_COVERAGE) {
        color = CoverageProxyColor(surfel);
    } else if (uDebugView == DEBUG_SURFEL_GRID_CELLS) {
        uvec3 cellCoord;
        if (SurfelWorldToGridCell(surfel.worldPos_radius.xyz, uGridMin, uGridMax, uGridResolution, cellCoord)) {
            color = HashColor(SurfelFlattenCell(cellCoord, uGridResolution));
        } else {
            color = vec3(1.0, 0.0, 1.0);
        }
    } else if (uDebugView == DEBUG_CELL_OCCUPANCY) {
        color = HashColor(surfel.ids.z ^ (surfelID * 1013904223u));
    } else if (uDebugView == DEBUG_SPAWN_RECYCLE) {
        color = LifecycleColor(surfel, alive, recycleMarker, ring);
    } else if (uDebugView == DEBUG_RAY_COUNTS) {
        color = RayCountColor(surfelID, surfel, ring);
    } else if (uDebugView == DEBUG_RAY_GUIDE) {
        color = GuideConfidenceColor(surfelID, surfel, ring);
    } else if (uDebugView == DEBUG_RADIAL_DEPTH) {
        color = RadialDepthColor(surfelID, surfel, ring);
    } else if (uDebugView == DEBUG_INDIRECT_ONLY) {
        color = max(TonemapDebug(surfel.irradiance.rgb), vec3(0.02));
    } else if (uDebugView == DEBUG_RECYCLE_SCORE) {
        color = RecycleScoreColor(surfel, ring);
    } else if (uDebugView == DEBUG_STALE_SURFELS) {
        color = StaleColor(surfel, ring);
    } else {
        color = LiveSurfelColor(surfel);
    }

    vStyle = vec4(Safe01(ring), recycleMarker ? 1.0 : 0.0, alive ? 1.0 : 0.0, 0.0);
    return vec4(color, recycleMarker ? 0.85 : 1.0);
}

void main()
{
    uint surfelID = uint(gl_VertexID);
    if (surfelID >= uMaxSurfels) {
        HidePoint();
        return;
    }

    Surfel surfel = surfels[surfelID];
    bool alive = SurfelIsAlive(surfel);
    bool lifecycleMode = uDebugView == DEBUG_SPAWN_RECYCLE;
    bool hasPosition = !any(isnan(surfel.worldPos_radius.xyz)) && !any(isinf(surfel.worldPos_radius.xyz));
    bool recycleMarker = lifecycleMode && !alive && hasPosition && (surfel.debug.w > 0.5 || surfel.ids.w > 0u);
    if ((!alive && !recycleMarker) || !hasPosition) {
        HidePoint();
        return;
    }

    vec3 normal = SurfelSafeNormalize(surfel.worldNormal_age.xyz);
    float surfelRadius = alive ? max(surfel.worldPos_radius.w, 0.001) : max(surfel.localPos_spawnRadius.w, 0.05);
    float lift = recycleMarker ? 0.0 : min(max(surfelRadius * 0.025, 0.0005), 0.015);
    vec3 diskWorldPos = surfel.worldPos_radius.xyz + normal * lift;
    vec4 centerView4 = uView * vec4(diskWorldPos, 1.0);
    vec3 normalView = SurfelSafeNormalize(mat3(uView) * normal);
    vec4 clip = uProjection * centerView4;
    if (centerView4.z >= -0.001 || clip.w <= 0.0) {
        HidePoint();
        return;
    }

    float viewDepth = -centerView4.z;
    float projectedRadiusPx = surfelRadius * uProjection[1][1] * max(uViewportSize.y, 1.0) * 0.5 / viewDepth;
    float densityScale = mix(0.70, 1.0, Safe01(surfel.albedo_life.a));
    float pointRadiusPx = recycleMarker ? 6.0 : clamp(projectedRadiusPx * densityScale, 2.5, 36.0);

    gl_Position = clip;
    gl_PointSize = pointRadiusPx * 2.0;
    vColor = DebugColor(surfelID, surfel, alive, recycleMarker);
    vDisk0 = vec4(centerView4.xyz, surfelRadius);
    vDisk1 = vec4(normalView, pointRadiusPx);
    vDisk2 = vec4(clip.xy / max(abs(clip.w), 1e-6), max(uViewportSize, vec2(1.0)));
}
