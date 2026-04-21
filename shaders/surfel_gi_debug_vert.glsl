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
    uint surfelID = uint(gl_InstanceID);
    SurfelRecord s = surfels[surfelID];

    vec2 corners[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0)
    );
    vec2 corner = corners[gl_VertexID & 3];
    vDiscUV = corner;

    if (!IsSurfelValid(s)) {
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

    vec3 worldPos = s.worldPositionRadius.xyz + (tangent * corner.x + bitangent * corner.y) * radius;
    vec4 clip = uViewProj * vec4(worldPos, 1.0);
    if (clip.w <= 0.0) {
        gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
        vColor = vec4(0.0);
        vWorldPos = worldPos;
        vWorldNormal = normal;
        return;
    }

    gl_Position = clip;
    vWorldPos = worldPos;
    vWorldNormal = normal;

    vec3 color = mix(IdColor(s.ids.z), vec3(1.0), 0.18);
    float alpha = 0.82;

    if (uDebugMode == 2) {
        color = normal * 0.5 + 0.5;
        alpha = 0.78;
    } else if (uDebugMode == 3) {
        color = HeatColor(clamp(s.worldPositionRadius.w / 2.0, 0.0, 1.0));
    } else if (uDebugMode == 4) {
        color = HeatColor(clamp(s.metrics.x / 2.0, 0.0, 1.0));
    } else if (uDebugMode == 5) {
        uint count = s.grid.x < header.tiling.w ? gridHeaders[s.grid.x].x : 0u;
        color = HeatColor(float(count) / max(float(header.gridDims.w), 1.0));
        alpha = 0.75;
    } else if (uDebugMode == 6) {
        color = (s.ids.y & SURFEL_FLAG_RECYCLED) != 0u ? vec3(1.0, 0.1, 0.05) : IdColor(s.ids.z);
        alpha = 0.80;
    } else if (uDebugMode == 7) {
        float movedCells = s.grid.x == s.grid.y ? 0.0 : 1.0;
        color = mix(vec3(0.1, 1.0, 0.35), vec3(1.0, 0.85, 0.05), movedCells);
        alpha = 0.78;
    } else if (uDebugMode == 8) {
        bool dormant = (s.ids.y & SURFEL_FLAG_DORMANT) != 0u;
        color = dormant ? vec3(0.45, 0.45, 0.55) : vec3(0.05, 0.95, 0.38);
        alpha = dormant ? 0.38 : 0.72;
    }

    vColor = vec4(color, alpha);
}
