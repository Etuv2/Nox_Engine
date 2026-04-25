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

layout(binding = 29, std430) buffer GuidingBinsBuffer {
    float guidingBins[];
};

layout(binding = 30, std430) buffer RadialDepthBinsBuffer {
    vec4 radialDepthBins[];
};

uniform int uFrameIndex;
uniform int uSurfelStart;
uniform int uSurfelCount;
uniform vec3 uDirectionalLightDir;
uniform vec3 uDirectionalLightColor;
uniform vec3 uCameraPos;
uniform float uMaxRayDistance;

vec3 SampleNearbyCache(vec3 point, vec3 normal, uint selfID)
{
    uint cell = GridCellIndexForViewPosition(point - uCameraPos, header);
    uint count = min(gridHeaders[cell].x, header.gridDims.w);
    vec3 sumIrradiance = vec3(0.0);
    float sumWeight = 0.0;

    for (uint i = 0u; i < count; ++i) {
        uint surfelID = gridEntries[cell * header.gridDims.w + i];
        if (surfelID == selfID || surfelID >= header.counts.x) {
            continue;
        }

        SurfelRecord n = surfels[surfelID];
        if (!IsSurfelValid(n) || n.irradianceHistory.w <= 0.0) {
            continue;
        }

        vec3 neighborNormal = normalize(n.worldNormalRecycle.xyz);
        float normalWeight = clamp(dot(normal, neighborNormal), 0.0, 1.0);
        vec3 delta = point - n.worldPositionRadius.xyz;
        float radius = max(n.worldPositionRadius.w * 2.0, 0.001);
        float distanceWeight = 1.0 - smoothstep(radius * 0.25, radius, length(delta));
        float confidence = clamp(n.irradianceHistory.w / 32.0, 0.0, 1.0);
        float w = normalWeight * distanceWeight * confidence;
        sumIrradiance += max(n.irradianceHistory.rgb, vec3(0.0)) * w;
        sumWeight += w;
    }

    return sumWeight > 0.0001 ? sumIrradiance / sumWeight : vec3(0.0);
}

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

    uint allocated = uint(max(s.solveState.y, 0.0));
    if (allocated == 0u) {
        s.rawIrradiance = vec4(0.0);
        surfels[id] = s;
        return;
    }

    vec3 normal = normalize(s.worldNormalRecycle.xyz);
    vec3 accum = vec3(0.0);
    float accepted = 0.0;
    float maxDistance = max(uMaxRayDistance, s.worldPositionRadius.w * 2.0);

    for (uint ray = 0u; ray < allocated && ray < 32u; ++ray) {
        uint seed = HashUInt(s.ids.z ^ (uint(max(uFrameIndex, 0)) * 9781u) ^ (ray * 6271u));
        float u1 = Hash01(seed);
        float u2 = Hash01(seed ^ 0x68bc21ebu);

        vec3 hemi = SurfelCosineHemisphereSample(u1, u2);
        float guideConfidence = clamp(length(s.guidingState.xyz), 0.0, 1.0);
        if (guideConfidence > 0.08 && Hash01(seed ^ 0x9e3779b9u) < guideConfidence * 0.65) {
            uint guideBase = id * 36u;
            float sum = 0.0;
            for (uint bin = 0u; bin < 36u; ++bin) {
                sum += max(guidingBins[guideBase + bin], 0.0);
            }
            if (sum > 0.0001) {
                float target = Hash01(seed ^ 0xb5297a4du) * sum;
                float prefix = 0.0;
                uint chosen = 0u;
                for (uint bin = 0u; bin < 36u; ++bin) {
                    prefix += max(guidingBins[guideBase + bin], 0.0);
                    if (prefix >= target) {
                        chosen = bin;
                        break;
                    }
                }
                vec2 texel = vec2(float(chosen % 6u), float(chosen / 6u));
                vec2 uv = (texel + vec2(u1, u2)) / 6.0;
                vec2 xy = uv * 2.0 - 1.0;
                float z = sqrt(max(1.0 - dot(xy, xy) * 0.5, 0.0));
                hemi = normalize(vec3(xy, z));
            }
        }

        vec3 rayDir = SurfelHemiToWorld(hemi, normal);
        vec3 hitPoint = s.worldPositionRadius.xyz + normal * max(s.worldPositionRadius.w * 0.05, 0.002) + rayDir * maxDistance;
        vec3 hitNormal = normal;
        vec3 lightDir = normalize(-uDirectionalLightDir);
        float direct = max(dot(hitNormal, lightDir), 0.0);
        vec3 directIrradiance = max(uDirectionalLightColor, vec3(0.0)) * direct;
        vec3 bounced = SampleNearbyCache(hitPoint, hitNormal, id) * 0.65;
        vec3 incoming = directIrradiance + bounced + vec3(0.015);
        accum += incoming;
        accepted += 1.0;

        vec3 localDir = SurfelWorldToHemi(rayDir, normal);
        uint depthBin = id * 16u + SurfelRadialDepthBin(localDir);
        vec4 depth = radialDepthBins[depthBin];
        float sampleDepth = maxDistance;
        float samples = min(depth.z + 1.0, 1024.0);
        float alpha = samples <= 1.0 ? 1.0 : clamp(1.0 / samples, 0.02, 0.35);
        depth.x = mix(depth.x <= 0.0 ? sampleDepth : depth.x, sampleDepth, alpha);
        depth.y = mix(depth.y, sampleDepth * sampleDepth, alpha);
        depth.z = samples;
        depth.w = max(s.worldPositionRadius.w, 0.001);
        radialDepthBins[depthBin] = depth;

        uint guideBin = id * 36u + SurfelGuideBin(localDir);
        guidingBins[guideBin] = mix(guidingBins[guideBin], max(LumaSurfel(incoming), 0.0), 0.10);
    }

    vec3 raw = accepted > 0.0 ? accum / accepted : vec3(0.0);
    s.rawIrradiance = vec4(max(raw, vec3(0.0)), accepted);
    s.sharedIrradiance = vec4(max(raw, vec3(0.0)), 0.0);
    s.solveState.w = float(max(uFrameIndex, 0));
    surfels[id] = s;
    atomicAdd(irradianceHeader.passStats.x, 1u);
}
