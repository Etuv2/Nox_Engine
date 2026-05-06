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

layout(binding = B_RAY_HITS, std430) readonly buffer SurfelRayHitBuffer
{
    SurfelRayHit rayHits[];
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
uniform uint uMaxRays;
uniform uint uDebugView;
uniform uvec3 uGridResolution;
uniform vec3 uGridMin;
uniform vec3 uGridMax;
uniform vec2 uViewportSize;
uniform uint uUseNonLinearGrid;
uniform float uGridFarExtent;
uniform float uTargetSurfelScreenRadiusPx;

out vec4 vColor;
out vec4 vStyle;
out vec4 vDisk0;
out vec4 vDisk1;
out vec4 vDisk2;
out vec2 vDiskUV;

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
const uint DEBUG_RAY_HIT_RADIANCE = 17u;
const uint DEBUG_STORED_SURFEL_WORLD_POSITION = 25u;
const uint DEBUG_STORED_SURFEL_RADIUS = 26u;
const uint DEBUG_STORED_SURFEL_TRANSFORM_ID = 27u;
const uint DEBUG_STORED_SURFEL_FLAGS = 28u;
const uint DEBUG_DEBUG_DRAW_POSITION = 29u;
const uint DEBUG_STORED_SURFEL_ALBEDO = 30u;
const uint DEBUG_RADIUS_ERROR = 31u;
const uint DEBUG_GRID_AXIS_REGION = 32u;
const uint DEBUG_GRID_OVERFLOW = 33u;
const uint DEBUG_IRRADIANCE_CONFIDENCE = 34u;
const float DEBUG_DISK_GRAZING_RADIUS_SCALE = 0.28;
const float GIBS_PAPER_DISK_SCALE = 1.15;
const float GIBS_PAPER_MIN_RADIUS_PX = 3.0;
const float GIBS_PAPER_MAX_RADIUS_PX = 14.0;

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

vec3 GIBSPaperColor(uint surfelID, Surfel surfel)
{
    uint materialID = surfel.ids.z;
    uint transformID = surfel.ids.x;
    vec3 hashColor = HashColor(surfelID * 747796405u ^ materialID * 2891336453u ^ transformID * 1597334677u);
    vec3 albedo = max(SafePositive(surfel.albedo_life.rgb), vec3(0.08));
    float confidence = Safe01(surfel.irradiance.a);
    vec3 pastel = mix(vec3(1.0), hashColor, 0.72);
    vec3 materialTint = mix(pastel, albedo, 0.28);
    return materialTint * mix(0.82, 1.18, confidence);
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
    vDiskUV = vec2(2.0);
}

vec3 WorldPositionColor(vec3 worldPos)
{
    if (any(isnan(worldPos)) || any(isinf(worldPos))) {
        return vec3(1.0, 0.0, 1.0);
    }

    vec3 extent = max(uGridMax - uGridMin, vec3(1e-4));
    return clamp((worldPos - uGridMin) / extent, vec3(0.0), vec3(1.0));
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

vec3 RayHitRadianceColor(uint surfelID, out float ring)
{
    ring = 0.0;
    SurfelRayRequest request = rayRequests[surfelID];
    if (request.surfelID != surfelID || request.allocatedCount == 0u || request.firstRay >= uMaxRays) {
        ring = 0.85;
        return vec3(0.04, 0.05, 0.08);
    }

    vec3 radianceSum = vec3(0.0);
    float hitCount = 0.0;
    uint rayCount = min(min(request.allocatedCount, 8u), uMaxRays - request.firstRay);
    for (uint i = 0u; i < rayCount; ++i) {
        SurfelRayHit hit = rayHits[request.firstRay + i];
        float hitSignal = hit.normal_hitKind.w > 0.5 ? 1.0 : 0.0;
        radianceSum += SafePositive(hit.radiance_pdf.rgb);
        hitCount += hitSignal;
    }

    float invRayCount = 1.0 / max(float(rayCount), 1.0);
    ring = 1.0 - Safe01(hitCount * invRayCount);
    return max(TonemapDebug(radianceSum * invRayCount), vec3(0.015));
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
        SurfelGridAddress address;
        if (SurfelWorldToGridAddress(surfel.worldPos_radius.xyz, uGridMin, uGridMax, uGridResolution, uUseNonLinearGrid, uGridFarExtent, address)) {
            color = HashColor(address.cell);
        } else {
            color = vec3(1.0, 0.0, 1.0);
        }
    } else if (uDebugView == DEBUG_CELL_OCCUPANCY) {
        color = HashColor(surfel.ids.z ^ (surfelID * 1013904223u));
    } else if (uDebugView == DEBUG_SPAWN_RECYCLE) {
        color = LifecycleColor(surfel, alive, recycleMarker, ring);
    } else if (uDebugView == DEBUG_RAY_COUNTS) {
        color = RayCountColor(surfelID, surfel, ring);
    } else if (uDebugView == DEBUG_RAY_HIT_RADIANCE) {
        color = RayHitRadianceColor(surfelID, ring);
    } else if (uDebugView == DEBUG_STORED_SURFEL_WORLD_POSITION) {
        color = WorldPositionColor(surfel.worldPos_radius.xyz);
    } else if (uDebugView == DEBUG_STORED_SURFEL_RADIUS) {
        color = Heat(surfel.worldPos_radius.w / max(surfel.localPos_spawnRadius.w * 4.0, 0.001));
        ring = Safe01(surfel.debug.z);
    } else if (uDebugView == DEBUG_STORED_SURFEL_TRANSFORM_ID) {
        color = HashColor(surfel.ids.x);
    } else if (uDebugView == DEBUG_STORED_SURFEL_FLAGS) {
        uint flags = SurfelFlags(surfel);
        color = vec3(
            (flags & SURFEL_ALIVE) != 0u ? 0.0 : 1.0,
            (flags & SURFEL_NEW) != 0u ? 1.0 : 0.25,
            (flags & SURFEL_INVALID) != 0u ? 1.0 : 0.05);
        ring = (flags & SURFEL_INVALID) != 0u ? 1.0 : 0.0;
    } else if (uDebugView == DEBUG_DEBUG_DRAW_POSITION) {
        color = WorldPositionColor(surfel.worldPos_radius.xyz);
        ring = 0.35;
    } else if (uDebugView == DEBUG_STORED_SURFEL_ALBEDO) {
        color = SafePositive(surfel.albedo_life.rgb);
        ring = 0.25;
    } else if (uDebugView == DEBUG_RADIUS_ERROR) {
        float error = Safe01(surfel.debug.z);
        color = Heat(error);
        ring = error;
    } else if (uDebugView == DEBUG_GRID_AXIS_REGION) {
        SurfelGridAddress address;
        if (SurfelWorldToGridAddress(surfel.worldPos_radius.xyz, uGridMin, uGridMax, uGridResolution, uUseNonLinearGrid, uGridFarExtent, address)) {
            vec3 debugBoundsCenter;
            vec3 debugBoundsHalfExtent;
            uint debugRegion;
            SurfelGridCellDebugBounds(surfel.worldPos_radius.xyz, uGridMin, uGridMax, uGridResolution, uUseNonLinearGrid, uGridFarExtent, debugBoundsCenter, debugBoundsHalfExtent, debugRegion);
            color = HashColor(address.region * 92837111u + address.coord.z * 1013904223u);
            ring = address.region == SURFEL_GRID_REGION_CENTRAL ? 0.0 : 0.45;
        } else {
            color = vec3(1.0, 0.0, 1.0);
            ring = 1.0;
        }
    } else if (uDebugView == DEBUG_GRID_OVERFLOW) {
        SurfelGridAddress address;
        if (SurfelWorldToGridAddress(surfel.worldPos_radius.xyz, uGridMin, uGridMax, uGridResolution, uUseNonLinearGrid, uGridFarExtent, address)) {
            float overflowish = Safe01(float(min(address.coord.z, 24u)) / 24.0);
            color = mix(vec3(0.05, 0.20, 0.75), vec3(1.0, 0.08, 0.02), overflowish);
            ring = overflowish;
        } else {
            color = vec3(1.0, 0.0, 1.0);
            ring = 1.0;
        }
    } else if (uDebugView == DEBUG_IRRADIANCE_CONFIDENCE) {
        float confidence = Safe01(surfel.irradiance.a);
        color = mix(vec3(0.35, 0.02, 0.65), vec3(0.05, 0.95, 0.85), confidence);
        ring = 1.0 - confidence;
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
        color = GIBSPaperColor(surfelID, surfel);
    }

    bool paperStyle = uDebugView == DEBUG_SURFEL_SPHERES;
    vStyle = vec4(Safe01(ring), recycleMarker ? 1.0 : 0.0, alive ? 1.0 : 0.0, 0.0);
    vStyle.w = paperStyle ? 1.0 : 0.0;
    return vec4(color, recycleMarker ? 0.85 : 1.0);
}

void main()
{
    uint surfelID = uint(gl_InstanceID);
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
    vec3 normalView = SurfelSafeNormalize(mat3(uView) * normal);
    float rawSurfelRadius = alive ? max(surfel.worldPos_radius.w, 0.001) : max(surfel.localPos_spawnRadius.w, 0.05);
    float viewFacing = abs(dot(normalView, vec3(0.0, 0.0, 1.0)));
    bool paperStyle = uDebugView == DEBUG_SURFEL_SPHERES;
    float minGrazingScale = paperStyle ? 0.55 : DEBUG_DISK_GRAZING_RADIUS_SCALE;
    float grazingScale = mix(minGrazingScale, 1.0, smoothstep(0.12, 0.65, viewFacing));
    float surfelRadius = max(rawSurfelRadius * grazingScale, 0.001);
    vec3 tangent;
    vec3 bitangent;
    SurfelBuildBasis(normal, tangent, bitangent);

    vec2 corner = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0,
                       (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
    vDiskUV = corner;

    vec3 diskCenterWorld = surfel.worldPos_radius.xyz;
    vec4 diskCenterView4 = uView * vec4(diskCenterWorld, 1.0);
    if (diskCenterView4.z >= -0.001) {
        HidePoint();
        return;
    }

    if (paperStyle) {
        bool paperBillboard = true;
        float viewDepth = -diskCenterView4.z;
        float projectedRadiusPx = rawSurfelRadius * abs(uProjection[1][1]) * max(uViewportSize.y, 1.0) * 0.5 / max(viewDepth, 1e-4);
        float targetRadiusPx = clamp(projectedRadiusPx * GIBS_PAPER_DISK_SCALE,
                                     GIBS_PAPER_MIN_RADIUS_PX,
                                     GIBS_PAPER_MAX_RADIUS_PX);
        float screenRadiusToView = 2.0 * viewDepth /
            max(abs(uProjection[1][1]) * max(uViewportSize.y, 1.0), 1e-4);
        float screenRadiusToViewX = 2.0 * viewDepth /
            max(abs(uProjection[0][0]) * max(uViewportSize.x, 1.0), 1e-4);
        vec4 paperViewPos = vec4(
            diskCenterView4.xyz + vec3(corner.x * targetRadiusPx * screenRadiusToViewX,
                                       corner.y * targetRadiusPx * screenRadiusToView,
                                       0.0),
            1.0);
        vec4 clip = uProjection * paperViewPos;
        if (!paperBillboard || clip.w <= 0.0) {
            HidePoint();
            return;
        }

        gl_Position = clip;
        gl_PointSize = 1.0;
        vColor = DebugColor(surfelID, surfel, alive, recycleMarker);
        vDisk0 = vec4(diskCenterView4.xyz, targetRadiusPx * max(screenRadiusToView, screenRadiusToViewX));
        vDisk1 = vec4(normalView, targetRadiusPx);
        vDisk2 = vec4(clip.xy / max(abs(clip.w), 1e-6), max(uViewportSize, vec2(1.0)));
        return;
    }

    vec3 diskWorldPos = diskCenterWorld + (tangent * corner.x + bitangent * corner.y) * surfelRadius;
    vec4 centerView4 = uView * vec4(diskWorldPos, 1.0);
    vec4 clip = uProjection * centerView4;
    if (centerView4.z >= -0.001 || clip.w <= 0.0) {
        HidePoint();
        return;
    }

    float viewDepth = -diskCenterView4.z;
    float projectedRadiusPx = surfelRadius * uProjection[1][1] * max(uViewportSize.y, 1.0) * 0.5 / max(viewDepth, 1e-4);
    gl_Position = clip;
    gl_PointSize = 1.0;
    vColor = DebugColor(surfelID, surfel, alive, recycleMarker);
    vDisk0 = vec4(diskCenterView4.xyz, surfelRadius);
    vDisk1 = vec4(normalView, projectedRadiusPx);
    vDisk2 = vec4(clip.xy / max(abs(clip.w), 1e-6), max(uViewportSize, vec2(1.0)));
}
