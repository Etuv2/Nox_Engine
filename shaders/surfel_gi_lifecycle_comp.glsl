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

layout(binding = 22, std430) buffer FreeStackBuffer {
    uint freeStack[];
};

layout(binding = 23, std430) buffer RecycleStackBuffer {
    uint recycleStack[];
};

uniform int uFrameIndex;
uniform vec2 uResolution;
uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraPos;
uniform float uTargetRadiusPixels;
uniform float uRecyclePressure;

void RecycleSurfel(uint id, uint frameIndex)
{
    surfels[id].ids.y = SURFEL_FLAG_RECYCLED;
    surfels[id].frames.w = frameIndex;
    surfels[id].worldNormalRecycle.w = 1.0;
    uint slot = atomicAdd(header.counts.z, 1u);
    if (slot < header.counts.x) {
        freeStack[slot] = id;
    }
    atomicAdd(header.frameStats.y, 1u);
    atomicAdd(header.counts.y, uint(-1));
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
    uint transformID = s.ids.x;
    if (transformID == 0u) {
        RecycleSurfel(id, frameIndex);
        return;
    }

    GpuTransformRecord transformRecord = transforms[transformID];
    mat4 world = transformRecord.world;
    vec4 followedPos = world * vec4(s.localPositionAge.xyz, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(world)));
    vec3 followedNormal = normalize(normalMatrix * s.localNormalDebug.xyz);

    vec4 viewPos = uView * followedPos;
    float radius = ComputeWorldRadiusForProjectedPixels(viewPos.z, uTargetRadiusPixels, uResolution.y, uProjection);

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
        s.frames.z = frameIndex;
    }

    float cameraDistance = distance(followedPos.xyz, uCameraPos);
    float age = max(s.localPositionAge.w, 1.0);
    float framesSinceVisible = float(frameIndex - min(s.frames.y, frameIndex));
    float framesSinceContributing = float(frameIndex - min(s.frames.z, frameIndex));
    float freeFraction = float(header.counts.z) / max(float(header.counts.x), 1.0);
    float capacityPressure = clamp(1.0 - freeFraction * 2.0, 0.0, 1.0);
    float dormantScore = smoothstep(120.0, 720.0, framesSinceVisible + framesSinceContributing);
    float distanceScore = smoothstep(80.0, 260.0, cameraDistance);
    float oversubScore = smoothstep(1.35, 2.5, max(s.metrics.x, 0.0));
    float youngProtection = 1.0 - smoothstep(0.0, 90.0, age);
    float recycleScore = clamp(
        capacityPressure * uRecyclePressure +
        dormantScore * 0.45 +
        distanceScore * 0.25 +
        oversubScore * 0.35 -
        youngProtection * 0.5,
        0.0,
        1.0);

    s.metrics.y = cameraDistance;
    s.metrics.z = max(radius * uResolution.y * abs(uProjection[1][1]) / max(abs(viewPos.z), 0.05), 0.0);
    s.metrics.w = 1.0 - recycleScore;
    s.worldNormalRecycle.w = recycleScore;

    if (framesSinceVisible > 180.0) {
        s.ids.y |= SURFEL_FLAG_DORMANT;
        atomicAdd(header.frameStats.z, 1u);
    } else {
        s.ids.y &= ~SURFEL_FLAG_DORMANT;
    }

    float recycleRand = Hash01(HashUInt(id ^ (frameIndex * 747796405u)));
    bool recycle = recycleScore > recycleRand && (capacityPressure > 0.15 || framesSinceVisible > 300u || s.metrics.x > 2.25);
    if (recycle) {
        surfels[id] = s;
        RecycleSurfel(id, frameIndex);
        return;
    }

    surfels[id] = s;
}
