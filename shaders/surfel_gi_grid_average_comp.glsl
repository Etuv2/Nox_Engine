#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

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

layout(binding = 26, std430) readonly buffer GridEntryBuffer {
    uint gridEntries[];
};

layout(binding = 33, std430) buffer GridCellAverageBuffer {
    SurfelGridCellAverage gridCellAverages[];
};

void main()
{
    uint cell = gl_GlobalInvocationID.x;
    if (cell >= header.tiling.w) {
        return;
    }

    vec3 irradianceSum = vec3(0.0);
    vec3 normalSum = vec3(0.0);
    float weightSum = 0.0;
    float confidenceSum = 0.0;
    uint accepted = 0u;

    uint count = min(gridHeaders[cell].x, header.gridDims.w);
    for (uint i = 0u; i < count; ++i) {
        uint surfelID = gridEntries[cell * header.gridDims.w + i];
        if (surfelID >= header.counts.x) {
            continue;
        }

        SurfelRecord s = surfels[surfelID];
        if (!IsSurfelValid(s) || !SurfelHasReliableGatherLighting(s)) {
            continue;
        }

        vec3 irradiance = max(s.irradianceHistory.rgb, vec3(0.0));
        float confidence = clamp(SurfelHistoryConfidence(s), 0.0, 1.0);
        float sampleWeight = clamp(s.irradianceHistory.w / 32.0, 0.12, 1.0);
        float variance = max(max(s.shortTermStats.y, s.longTermStats.y), 0.0);
        float varianceWeight = 1.0 - smoothstep(0.04, 0.40, variance);
        float relevanceWeight = clamp(max(s.metrics.w, 0.25), 0.25, 1.0);
        float weight = sampleWeight * max(confidence, 0.10) * mix(0.45, 1.0, varianceWeight) * relevanceWeight;
        if (weight <= 0.0001) {
            continue;
        }

        vec3 normal = SurfelStableNormal(s.worldNormalRecycle.xyz);
        irradianceSum += irradiance * weight;
        normalSum += normal * weight;
        confidenceSum += confidence * weight;
        weightSum += weight;
        ++accepted;
    }

    if (weightSum <= 0.0001 || accepted == 0u) {
        gridCellAverages[cell].irradianceWeight = vec4(0.0);
        gridCellAverages[cell].normalCount = vec4(0.0);
        return;
    }

    vec3 averageIrradiance = irradianceSum / weightSum;
    vec3 averageNormal = length(normalSum) > 0.0001 ? normalize(normalSum) : vec3(0.0, 1.0, 0.0);
    float averageConfidence = confidenceSum / weightSum;
    float populationConfidence = smoothstep(1.0, 6.0, float(accepted));
    float cellConfidence = clamp(averageConfidence * populationConfidence * clamp(weightSum / 4.0, 0.15, 1.0), 0.0, 1.0);

    gridCellAverages[cell].irradianceWeight = vec4(averageIrradiance, cellConfidence);
    gridCellAverages[cell].normalCount = vec4(averageNormal, float(accepted));
}
