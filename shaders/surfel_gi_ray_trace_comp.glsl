#version 460 core

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "includes/surfel_gi_common.glsl"

uniform int u_triangleCount;
uniform int u_bvhNodeCount;
uniform int u_rtInstanceCount;
uniform int u_rtInstanceNodeCount;

#define RT_SCENE_EXTERNAL_COUNTS
#include "includes/rt_scene_common.glsl"

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

struct RTLightData {
    vec4 position;
    vec4 direction;
    vec4 color;
    vec4 attenuation;
    vec4 shadowData;
    vec4 spotData;
    vec4 areaData;
    vec4 sampling;
};

layout(binding = 2, std430) readonly buffer LightBuffer {
    RTLightData lights[];
};

uniform int uFrameIndex;
uniform int uSurfelStart;
uniform int uSurfelCount;
uniform int u_lightCount;
uniform vec3 uDirectionalLightDir;
uniform vec3 uDirectionalLightColor;
uniform vec3 uCameraPos;
uniform float uMaxRayDistance;

vec3 SampleSurfelSky(vec3 direction)
{
    float t = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.55, 0.62, 0.72), vec3(0.18, 0.32, 0.58), t) * 0.18;
}

float TraceLightVisibility(vec3 point, vec3 normal, vec3 lightDir, float maxDistance)
{
    Ray shadowRay;
    shadowRay.origin = point + normal * max(RT_SCENE_EPSILON * 4.0, 0.002);
    shadowRay.direction = lightDir;
    shadowRay.tMin = RT_SCENE_EPSILON;
    shadowRay.tMax = max(maxDistance - RT_SCENE_EPSILON, RT_SCENE_EPSILON);

    HitInfo shadowHit = traceBVH(shadowRay);
    return shadowHit.hit ? 0.0 : 1.0;
}

vec3 EvaluateSharedSceneLights(vec3 point, vec3 normal)
{
    vec3 result = vec3(0.0);
    uint lightCount = uint(clamp(u_lightCount, 0, 16));

    for (uint i = 0u; i < lightCount; ++i) {
        RTLightData light = lights[i];
        int lightType = int(light.position.w);
        vec3 lightDir = vec3(0.0);
        vec3 radiance = vec3(0.0);
        float maxDistance = RT_SCENE_MAX_FLOAT;

        if (lightType == 0) {
            lightDir = normalize(-light.direction.xyz);
            radiance = max(light.color.xyz, vec3(0.0)) * max(light.color.w, 0.0);
            maxDistance = 10000.0;
        } else if (lightType == 1 || lightType == 2) {
            vec3 toLight = light.position.xyz - point;
            float distance = length(toLight);
            if (distance <= RT_SCENE_EPSILON) {
                continue;
            }
            lightDir = toLight / distance;
            maxDistance = distance;

            float range = max(light.attenuation.w, 0.001);
            if (distance > range) {
                continue;
            }

            vec3 att = light.attenuation.xyz;
            float denom = max(att.x + att.y * distance + att.z * distance * distance, 1.0);
            float rangeFalloff = max(1.0 - pow(distance / range, 4.0), 0.0);
            rangeFalloff *= rangeFalloff;
            float cone = 1.0;
            if (lightType == 2) {
                vec3 spotDir = normalize(light.direction.xyz);
                float cosTheta = dot(-lightDir, spotDir);
                float innerCos = light.spotData.x;
                float outerCos = min(light.spotData.y, innerCos - 0.001);
                float width = max(innerCos - outerCos, 0.001);
                cone = clamp((cosTheta - outerCos) / width, 0.0, 1.0);
                cone = cone * cone * (3.0 - 2.0 * cone);
            }
            radiance = max(light.color.xyz, vec3(0.0)) * max(light.color.w, 0.0) * rangeFalloff * cone / denom;
        } else {
            continue;
        }

        float ndotl = max(dot(normal, lightDir), 0.0);
        if (ndotl <= 0.0) {
            continue;
        }

        float visibility = TraceLightVisibility(point, normal, lightDir, maxDistance);
        result += radiance * ndotl * visibility;
    }

    return result;
}

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
        Ray sceneRay;
        sceneRay.origin = s.worldPositionRadius.xyz + normal * max(s.worldPositionRadius.w * 0.05, 0.002);
        sceneRay.direction = rayDir;
        sceneRay.tMin = RT_SCENE_EPSILON;
        sceneRay.tMax = maxDistance;

        HitInfo hit = traceBVH(sceneRay);
        vec3 incoming;
        float sampleDepth = maxDistance;

        if (hit.hit) {
            vec3 hitNormal = normalize(hit.normal);
            if (dot(hitNormal, -rayDir) < 0.0) {
                hitNormal = -hitNormal;
            }

            vec3 lightDir = normalize(-uDirectionalLightDir);
            float direct = max(dot(hitNormal, lightDir), 0.0);
            float visibility = direct > 0.0 ? TraceLightVisibility(hit.position, hitNormal, lightDir, 10000.0) : 0.0;
            vec3 directIrradiance = max(uDirectionalLightColor, vec3(0.0)) * direct * visibility;
            directIrradiance += EvaluateSharedSceneLights(hit.position, hitNormal);
            vec3 bounced = SampleNearbyCache(hit.position, hitNormal, id) * 0.65;
            vec3 albedo = max(hit.material.albedo, vec3(0.0));
            vec3 emission = max(hit.material.emissive, vec3(0.0)) * max(hit.material.emissiveStrength, 0.0);
            incoming = emission + albedo * (directIrradiance + bounced) + vec3(0.015);
            sampleDepth = hit.t;
        } else {
            incoming = SampleSurfelSky(rayDir);
        }

        accum += incoming;
        accepted += 1.0;

        vec3 localDir = SurfelWorldToHemi(rayDir, normal);
        uint depthBin = id * 16u + SurfelRadialDepthBin(localDir);
        vec4 depth = radialDepthBins[depthBin];
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
