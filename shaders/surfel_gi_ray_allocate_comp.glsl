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

uniform int uSurfelStart;
uniform int uSurfelCount;
uniform int uGlobalRayBudget;
uniform int uFrameIndex;

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

    uint requested = uint(max(s.solveState.x, 0.0));
    uint totalRequested = max(irradianceHeader.rayStats.x, 1u);
    uint globalBudget = uint(max(uGlobalRayBudget, 0));
    uint allocated = 0u;
    if (requested > 0u && globalBudget > 0u) {
        if (totalRequested <= globalBudget) {
            allocated = requested;
        } else {
            float exactShare = float(requested) * float(globalBudget) / float(totalRequested);
            uint baseShare = uint(floor(float(requested) * float(globalBudget) / float(totalRequested)));
            uint frameSalt = uint(max(uFrameIndex, 0)) * 1597334677u;
            float remainderScore = fract(exactShare) + Hash01(s.ids.z ^ (uint(id) * 747796405u) ^ frameSalt);
            uint desired = min(requested, baseShare + (remainderScore >= 1.0 ? 1u : 0u));

            for (uint ray = 0u; ray < desired; ++ray) {
                uint previous = atomicAdd(irradianceHeader.rayStats.y, 1u);
                if (previous < globalBudget) {
                    ++allocated;
                } else {
                    atomicAdd(irradianceHeader.rayStats.y, uint(-1));
                    break;
                }
            }
        }
    }

    if (allocated > requested) {
        allocated = requested;
    }

    s.solveState.y = float(allocated);
    surfels[id] = s;
    if (allocated > 0u && totalRequested <= globalBudget) {
        atomicAdd(irradianceHeader.rayStats.y, allocated);
    }
}
