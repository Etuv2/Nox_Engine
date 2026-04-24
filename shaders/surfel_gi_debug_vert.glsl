#version 460 core

#include "includes/surfel_gi_common.glsl"

layout(binding = 20, std430) readonly buffer SurfelBuffer {
    SurfelRecord surfels[];
};

layout(binding = 21, std430) readonly buffer HeaderBuffer {
    SurfelPoolHeader header;
};

layout(binding = 25, std430) readonly buffer GridHeaderBuffer {
    uvec4 gridHeaders[];
};

uniform mat4 uViewProj;
uniform vec2 uScreenSize;
uniform int uDebugMode;
uniform uint uFrameIndex;
uniform uint uDebugInstanceStride;

out vec2 vDiscUV;
out vec4 vColor;
out vec3 vWorldPos;
out vec3 vWorldNormal;

vec3 HeatColor(float t)
{
    t = clamp(t, 0.0, 1.0);
    vec3 a = mix(vec3(0.05, 0.20, 0.95), vec3(0.0, 0.85, 0.25), smoothstep(0.0, 0.45, t));
    vec3 b = mix(a, vec3(1.0, 0.88, 0.08), smoothstep(0.45, 0.75, t));
    return mix(b, vec3(1.0, 0.12, 0.05), smoothstep(0.75, 1.0, t));
}

vec3 IdColor(uint id)
{
    uint h = HashUInt(id * 747796405u + 2891336453u);
    return vec3(
        float((h >> 0u) & 255u),
        float((h >> 8u) & 255u),
        float((h >> 16u) & 255u)) / 255.0;
}

void main()
{
    uint surfelID = uint(gl_InstanceID) * max(uDebugInstanceStride, 1u);
    if (surfelID >= header.counts.x) {
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        vDiscUV = vec2(0.0);
        vColor = vec4(0.0);
        vWorldPos = vec3(0.0);
        vWorldNormal = vec3(0.0, 1.0, 0.0);
        return;
    }
    SurfelRecord s = surfels[surfelID];

    vec2 corners[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0)
    );
    vec2 corner = corners[gl_VertexID & 3];
    vDiscUV = corner;

    bool valid = IsSurfelValid(s);
    bool showRecentRecycle = uDebugMode == 6 || uDebugMode == 10;
    bool recentRecycle = showRecentRecycle &&
        !valid &&
        (s.ids.y & SURFEL_FLAG_RECYCLED) != 0u &&
        s.frames.w != 0u &&
        (uFrameIndex - min(s.frames.w, uFrameIndex)) < 45u;

    if (!valid && !recentRecycle) {
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        vColor = vec4(0.0);
        vWorldPos = vec3(0.0);
        vWorldNormal = vec3(0.0, 1.0, 0.0);
        return;
    }

    vec3 normal = normalize(s.worldNormalRecycle.xyz);
    vec3 helper = abs(normal.y) < 0.92 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, normal));
    vec3 bitangent = cross(normal, tangent);
    float radius = max(s.worldPositionRadius.w, 0.001);

    vec4 centerClip = uViewProj * vec4(s.worldPositionRadius.xyz, 1.0);
    if (centerClip.w <= max(radius * 1.5, 0.05)) {
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        vColor = vec4(0.0);
        vWorldPos = s.worldPositionRadius.xyz;
        vWorldNormal = normal;
        return;
    }

    vec3 worldPos = s.worldPositionRadius.xyz + (tangent * corner.x + bitangent * corner.y) * radius;
    vec4 clip = uViewProj * vec4(worldPos, 1.0);
    if (clip.w <= 0.0) {
        clip = centerClip;
    }

    gl_Position = clip;
    vWorldPos = worldPos;
    vWorldNormal = normal;

    uint ageFrames = uFrameIndex - min(s.frames.x, uFrameIndex);
    uint framesSinceContribution = uFrameIndex - min(s.frames.z, uFrameIndex);
    vec3 color = recentRecycle ? vec3(1.0, 0.10, 0.04) : mix(IdColor(s.ids.z), vec3(1.0), 0.18);
    float alpha = recentRecycle ? 0.58 : 0.82;

    if (uDebugMode == 2) {
        color = normal * 0.5 + 0.5;
        alpha = 0.78;
    } else if (uDebugMode == 3) {
        color = HeatColor(clamp(s.metrics.z / 24.0, 0.0, 1.0));
    } else if (uDebugMode == 4) {
        color = HeatColor(clamp(s.metrics.x / 2.0, 0.0, 1.0));
    } else if (uDebugMode == 5) {
        uint stored = s.grid.x < header.tiling.w ? gridHeaders[s.grid.x].x : 0u;
        uint overflow = s.grid.x < header.tiling.w ? gridHeaders[s.grid.x].y : 0u;
        color = HeatColor(float(stored + overflow) / max(float(header.gridDims.w), 1.0));
        color = mix(color, vec3(1.0, 0.12, 0.03), clamp(float(overflow) / 16.0, 0.0, 1.0));
        alpha = 0.75;
    } else if (uDebugMode == 6) {
        color = recentRecycle ? vec3(1.0, 0.1, 0.05) : IdColor(s.ids.z);
        alpha = 0.80;
    } else if (uDebugMode == 7) {
        bool invalidTransform = (s.ids.y & SURFEL_FLAG_TRANSFORM_INVALID) != 0u;
        float movedCells = s.grid.x == s.grid.y ? 0.0 : 1.0;
        color = invalidTransform ? vec3(1.0, 0.08, 0.04) :
            mix(IdColor(s.ids.x), vec3(1.0, 0.85, 0.05), movedCells * 0.55);
        alpha = 0.78;
    } else if (uDebugMode == 8) {
        if (s.grid.w == SURFEL_STATE_RECYCLABLE) {
            color = vec3(1.0, 0.82, 0.08);
            alpha = 0.82;
        } else {
            bool dormant = (s.ids.y & SURFEL_FLAG_DORMANT) != 0u;
            color = dormant ? vec3(0.45, 0.45, 0.55) : vec3(0.05, 0.95, 0.38);
            alpha = dormant ? 0.38 : 0.72;
        }
    } else if (uDebugMode == 9) {
        color = HeatColor(clamp(s.worldNormalRecycle.w, 0.0, 1.0));
        alpha = 0.78;
    } else if (uDebugMode == 10) {
        bool newlySpawned = valid && ageFrames < 45u;
        uint spawnReason = uint(max(s.localNormalDebug.w, 0.0) + 0.5);
        uint recycleReason = uint(max(s.recycleData.y, 0.0) + 0.5);
        if (recentRecycle) {
            color = recycleReason == SURFEL_RECYCLE_INVALID_TRANSFORM ? vec3(1.0, 0.04, 0.04) :
                (recycleReason == SURFEL_RECYCLE_OVERSAMPLED ? vec3(1.0, 0.72, 0.06) : vec3(1.0, 0.22, 0.08));
        } else if (newlySpawned) {
            color = spawnReason == SURFEL_SPAWN_REFINEMENT ? vec3(0.0, 0.72, 1.0) : vec3(0.05, 1.0, 0.35);
        } else {
            color = vec3(0.25, 0.45, 1.0);
        }
        alpha = (newlySpawned || recentRecycle) ? 0.88 : 0.32;
    } else if (uDebugMode == 11) {
        color = HeatColor(clamp(float(framesSinceContribution) / 900.0, 0.0, 1.0));
        alpha = 0.78;
    } else if (uDebugMode == 12) {
        color = HeatColor(clamp(s.metrics.y / 320.0, 0.0, 1.0));
        alpha = 0.74;
    } else if (uDebugMode == 13) {
        color = HeatColor(clamp(s.localPositionAge.w / 900.0, 0.0, 1.0));
        alpha = 0.72;
    } else if (uDebugMode == 14) {
        uint framesSinceVisible = uFrameIndex - min(s.frames.y, uFrameIndex);
        color = HeatColor(clamp(float(framesSinceVisible) / 900.0, 0.0, 1.0));
        alpha = 0.78;
    } else if (uDebugMode == 15) {
        bool reused = valid && ageFrames > 45u;
        color = reused ? vec3(0.16, 0.42, 1.0) : vec3(0.05, 1.0, 0.35);
        alpha = reused ? 0.46 : 0.88;
    } else if (uDebugMode == 16) {
        vec3 irradiance = max(s.irradianceHistory.rgb, vec3(0.0));
        float luma = max(dot(irradiance, vec3(0.2126, 0.7152, 0.0722)), 0.0001);
        color = clamp(irradiance / max(luma, 0.15), vec3(0.0), vec3(1.0)) * clamp(luma * 2.0, 0.15, 1.0);
        alpha = clamp(s.irradianceHistory.w / 24.0, 0.22, 0.82);
    } else if (uDebugMode == 17) {
        float variance = max(s.depthMoments.y - s.depthMoments.x * s.depthMoments.x, 0.0);
        color = HeatColor(clamp(sqrt(variance) / max(s.depthMoments.w, 0.001), 0.0, 1.0));
        alpha = clamp(s.depthMoments.z / 24.0, 0.20, 0.82);
    }

    vColor = vec4(color, alpha);
}
