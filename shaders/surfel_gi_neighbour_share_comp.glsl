#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

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

layout(binding = 28, std430) buffer IrradianceHeaderBuffer {
    SurfelIrradianceHeader irradianceHeader;
};

uniform int uSurfelStart;
uniform int uSurfelCount;
uniform int uFrameIndex;
uniform vec3 uCameraPos;

const uint kMaxSharedNeighbors = 8u;

void main()
{
    uint localIndex = gl_GlobalInvocationID.x;
    uint surfelCount = uint(max(uSurfelCount, 0));
    if (localIndex >= surfelCount || header.counts.x == 0u) {
        return;
    }

    uint id = (uint(max(uSurfelStart, 0)) + localIndex) % header.counts.x;
    SurfelRecord s = surfels[id];
    uint frameIndex = uint(max(uFrameIndex, 0));
    if (!IsSurfelValid(s) || !SurfelHasCurrentRawSample(s, frameIndex) || s.grid.x >= header.tiling.w) {
        return;
    }

    float receiverVariance = max(max(s.shortTermStats.y, s.longTermStats.y), 0.0);
    float receiverNeed = max(1.0 - clamp(s.irradianceHistory.w / 24.0, 0.0, 1.0), smoothstep(0.02, 0.45, receiverVariance));
    if (receiverNeed <= 0.02) {
        return;
    }

    uint count = min(gridHeaders[s.grid.x].x, header.gridDims.w);
    vec3 normal = SurfelStableNormal(s.worldNormalRecycle.xyz);
    vec3 sharedAccum = s.rawIrradiance.rgb;
    float sharedWeight = 1.0;
    uint accepted = 0u;

    for (uint i = 0u; i < count && accepted < kMaxSharedNeighbors; ++i) {
        uint otherID = gridEntries[s.grid.x * header.gridDims.w + i];
        if (otherID == id || otherID >= header.counts.x) {
            continue;
        }

        SurfelRecord other = surfels[otherID];
        if (!IsSurfelValid(other) || other.irradianceHistory.w <= 0.0) {
            continue;
        }

        vec3 otherNormal = SurfelStableNormal(other.worldNormalRecycle.xyz);
        float normalAlign = dot(normal, otherNormal);
        if (normalAlign < 0.75) {
            continue;
        }

        vec3 delta = s.worldPositionRadius.xyz - other.worldPositionRadius.xyz;
        float planeDistance = abs(dot(delta, otherNormal));
        vec3 tangentDelta = delta - otherNormal * dot(delta, otherNormal);
        float tangentDistance = length(tangentDelta);
        float radius = max(max(s.worldPositionRadius.w, other.worldPositionRadius.w), 0.001);
        float tangentWeight = 1.0 - smoothstep(radius * 0.35, radius * 2.0, tangentDistance);
        float planeWeight = 1.0 - smoothstep(radius * 0.05, radius * 0.65, planeDistance);
        float depthWeight = SurfelDepthValidityWeight(other, s.worldPositionRadius.xyz, normal);
        float donorConfidence = clamp(other.irradianceHistory.w / 48.0, 0.0, 1.0) * (1.0 - smoothstep(0.10, 0.75, max(other.longTermStats.y, 0.0)));
        float w = tangentWeight * planeWeight * clamp(normalAlign, 0.0, 1.0) * depthWeight * donorConfidence * receiverNeed;
        if (w <= 0.0001) {
            if (depthWeight <= 0.05) {
                atomicAdd(irradianceHeader.passStats.w, 1u);
            }
            continue;
        }

        sharedAccum += max(other.irradianceHistory.rgb, vec3(0.0)) * w;
        sharedWeight += w;
        ++accepted;
    }

    if (accepted > 0u) {
        vec3 sharedMean = sharedAccum / max(sharedWeight, 0.0001);
        float alpha = clamp(receiverNeed * 0.16, 0.02, 0.16);
        s.sharedIrradiance = vec4(mix(s.rawIrradiance.rgb, sharedMean, alpha), clamp(sharedWeight - 1.0, 0.0, 1.0));
        surfels[id] = s;
        atomicAdd(irradianceHeader.passStats.y, 1u);
    }
}
