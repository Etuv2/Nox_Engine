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

layout(binding = 30, std430) buffer RadialDepthBinsBuffer {
    vec4 radialDepthBins[];
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
    if (!IsSurfelValid(s)) {
        return;
    }

    uint base = id * 16u;
    float mean = 0.0;
    float second = 0.0;
    float count = 0.0;
    for (uint bin = 0u; bin < 16u; ++bin) {
        vec4 m = radialDepthBins[base + bin];
        float c = max(m.z, 0.0);
        mean += m.x * c;
        second += m.y * c;
        count += c;
    }

    if (count > 0.0) {
        mean /= count;
        second /= count;
        float samples = min(max(s.depthMoments.z, 0.0) + 1.0, 4096.0);
        float alpha = samples <= 1.0 ? 1.0 : clamp(1.0 / samples, 0.015, 0.20);
        s.depthMoments.x = mix(s.depthMoments.x, mean, alpha);
        s.depthMoments.y = mix(s.depthMoments.y, second, alpha);
        s.depthMoments.z = min(max(s.depthMoments.z, 0.0) + 1.0, 4096.0);
        s.depthMoments.w = max(s.worldPositionRadius.w, 0.001);
        surfels[id] = s;
        atomicAdd(irradianceHeader.passStats.z, 1u);
    }
}
