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
    uint ageFrames = frameIndex - min(s.frames.x, frameIndex);
    uint framesSinceContribution = frameIndex - min(s.frames.z, frameIndex);
    bool dormant = (s.ids.y & SURFEL_FLAG_DORMANT) != 0u;

    float shortVariance = max(s.shortTermStats.y, 0.0);
    float longVariance = max(s.longTermStats.y, 0.0);
    float varianceSignal = clamp(sqrt(max(shortVariance, longVariance)) * 3.5, 0.0, 1.0);
    float instability = clamp(s.shortTermStats.z * 2.0, 0.0, 1.0);
    float newness = 1.0 - smoothstep(8.0, 64.0, float(ageFrames));
    float staleSolve = smoothstep(16.0, 180.0, float(framesSinceContribution));
    float visibleRelevance = clamp(s.metrics.w, 0.0, 1.0);

    float priority = newness * 0.45 + varianceSignal * 0.30 + instability * 0.20 + staleSolve * 0.10 + visibleRelevance * 0.15;
    if (dormant) {
        priority *= mix(0.08, 0.45, max(varianceSignal, newness));
    }

    uint maxRays = uint(clamp(uMaxRaysPerSurfel, 1, 32));
    uint requested = priority > 0.025 ? uint(clamp(ceil(priority * float(maxRays)), 1.0, float(maxRays))) : 0u;
    s.solveState = vec4(float(requested), 0.0, dormant ? 0.0 : max(priority, 0.001), s.solveState.w);
    surfels[id] = s;

    if (requested > 0u) {
        atomicAdd(irradianceHeader.rayStats.x, requested);
        atomicAdd(irradianceHeader.rayStats.z, 1u);
    }
    if (!dormant) {
        atomicAdd(irradianceHeader.rayStats.w, 1u);
    } else {
        atomicAdd(irradianceHeader.debugStats.y, 1u);
    }
}
