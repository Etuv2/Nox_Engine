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

layout(binding = 29, std430) buffer GuidingBinsBuffer {
    float guidingBins[];
};

uniform int uSurfelStart;
uniform int uSurfelCount;

void main()
{
    uint localIndex = gl_GlobalInvocationID.x;
    uint surfelCount = uint(max(uSurfelCount, 0));
    if (localIndex >= surfelCount || header.counts.x == 0u) {
        return;
    }

    uint id = (uint(max(uSurfelStart, 0)) + localIndex) % header.counts.x;
    SurfelRecord s = surfels[id];
    if (!IsSurfelValid(s) || s.rawIrradiance.w <= 0.0) {
        return;
    }

    uint base = id * 36u;
    float sum = 0.0;
    vec2 moment = vec2(0.0);
    for (uint bin = 0u; bin < 36u; ++bin) {
        float w = max(guidingBins[base + bin], 0.0);
        vec2 xy = (vec2(float(bin % 6u), float(bin / 6u)) + vec2(0.5)) / 6.0 * 2.0 - 1.0;
        moment += xy * w;
        sum += w;
    }

    if (sum > 0.0001) {
        vec2 meanXY = clamp(moment / sum, vec2(-0.95), vec2(0.95));
        float z = sqrt(max(1.0 - dot(meanXY, meanXY) * 0.5, 0.0));
        float confidence = clamp(sum / (sum + 4.0), 0.0, 1.0);
        vec3 guide = normalize(vec3(meanXY, z)) * confidence;
        s.guidingState.xyz = mix(s.guidingState.xyz, guide, 0.18);

        float invMax = 0.0;
        for (uint bin = 0u; bin < 36u; ++bin) {
            invMax = max(invMax, guidingBins[base + bin]);
        }
        invMax = invMax > 0.0001 ? 1.0 / invMax : 1.0;
        for (uint bin = 0u; bin < 36u; ++bin) {
            guidingBins[base + bin] = clamp(guidingBins[base + bin] * invMax, 0.0, 1.0);
        }

        atomicAdd(irradianceHeader.debugStats.z, 1u);
    }

    surfels[id] = s;
}
