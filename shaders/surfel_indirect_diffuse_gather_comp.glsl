#version 460 core

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "includes/pbr_common.glsl"
#include "includes/surfel_gi_common.glsl"

layout(binding = 0) uniform sampler2D uDepth;
layout(binding = 1) uniform sampler2D uPackedNormalRM;
layout(binding = 2) uniform sampler2D uAlbedoAO;
layout(binding = 3) uniform sampler2D uSpecularF0;
layout(binding = 4) uniform sampler2D uEmissive;
layout(binding = 5) uniform usampler2D uTransformID;
layout(binding = 6) uniform sampler2D uPrincipledParams;

layout(binding = 0, rgba16f) writeonly uniform image2D outIrradiance;
layout(binding = 1, rgba16f) writeonly uniform image2D outDebug;

layout(std430, binding = 20) readonly buffer PersistentSurfelBuffer { SurfelRecord persistentSurfels[]; };
layout(std430, binding = 21) readonly buffer PersistentSurfelHeaderBuffer { SurfelPoolHeader surfelHeader; };
layout(std430, binding = 25) readonly buffer PersistentSurfelGridHeaderBuffer { uvec4 persistentSurfelGridHeaders[]; };
layout(std430, binding = 26) readonly buffer PersistentSurfelGridEntryBuffer { uint persistentSurfelGridEntries[]; };
layout(std430, binding = 30) readonly buffer SurfelRadialDepthBinsBuffer { vec4 radialDepthBins[]; };

uniform mat4 uInvProjection;
uniform mat4 uInvView;
uniform mat4 uView;
uniform vec3 uViewPos;
uniform vec2 uResolution;
uniform int uNeighborRadius;
uniform int uMaxCandidates;
uniform int uMaxAccepted;
uniform float uFallbackStrength;
uniform int uDebugMode;

vec3 ReconstructWorldPosition(vec2 uv, float depth)
{
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = uInvProjection * clip;
    viewPos /= max(abs(viewPos.w), 1e-6);
    vec4 world = uInvView * viewPos;
    return world.xyz / max(abs(world.w), 1e-6);
}

bool HasDiffuseResponse(vec2 uv)
{
    vec4 rm = textureLod(uPackedNormalRM, uv, 0.0);
    vec3 albedo = textureLod(uAlbedoAO, uv, 0.0).rgb;
    vec3 emissive = textureLod(uEmissive, uv, 0.0).rgb;
    float emissiveStrength = textureLod(uSpecularF0, uv, 0.0).a;
    float metallic = clamp(rm.a, 0.0, 1.0);
    float transmission = clamp(textureLod(uPrincipledParams, uv, 0.0).r, 0.0, 1.0);
    float diffuseMask = (1.0 - metallic) * (1.0 - transmission);
    if (diffuseMask <= 0.015) {
        return false;
    }
    if (Luminance(albedo) <= 1e-4 && Luminance(emissive) * max(emissiveStrength, 0.0) > 1e-3) {
        return false;
    }
    return true;
}

float RadialDepthValidity(uint surfelID, SurfelRecord s, vec3 receiverWorldPos)
{
    vec3 delta = receiverWorldPos - s.worldPositionRadius.xyz;
    float distanceToReceiver = length(delta);
    if (distanceToReceiver <= 1e-5) {
        return 1.0;
    }

    vec3 surfelNormal = normalize(s.worldNormalRecycle.xyz);
    vec3 hemi = SurfelWorldToHemi(delta / distanceToReceiver, surfelNormal);
    uint bin = SurfelRadialDepthBin(hemi);
    vec4 moments = radialDepthBins[surfelID * 16u + bin];
    if (moments.z <= 0.5) {
        return 1.0;
    }

    float mean = max(moments.x, 0.0);
    float variance = max(moments.y - mean * mean, 0.0);
    float sigma = sqrt(variance);
    float support = max(moments.w, s.worldPositionRadius.w);
    float permitted = mean + max(sigma * 2.0, support * 0.75);

    if (distanceToReceiver <= permitted) {
        return 1.0;
    }

    float fadeWidth = max(sigma * 2.0 + support, 0.001);
    return 1.0 - smoothstep(permitted, permitted + fadeWidth, distanceToReceiver);
}

float ComputeSurfelWeight(
    SurfelRecord s,
    uint surfelID,
    vec3 receiverWorldPos,
    vec3 receiverNormal,
    uint receiverTransformID,
    out uint rejectBits)
{
    rejectBits = 0u;
    if (!IsSurfelValid(s) || s.irradianceHistory.w <= 0.0) {
        rejectBits |= 1u;
        return 0.0;
    }

    vec3 surfelPos = s.worldPositionRadius.xyz;
    vec3 surfelNormal = normalize(s.worldNormalRecycle.xyz);
    float normalAlign = dot(receiverNormal, surfelNormal);
    if (normalAlign < 0.35) {
        rejectBits |= 2u;
        return 0.0;
    }

    vec3 delta = receiverWorldPos - surfelPos;
    float radius = max(s.worldPositionRadius.w, 0.001);
    float signedPlaneDistance = dot(delta, surfelNormal);
    float planeDistance = abs(signedPlaneDistance);
    vec3 tangentDelta = delta - surfelNormal * signedPlaneDistance;
    float tangentDistance = length(tangentDelta);
    float coverageRadius = radius * 1.85;

    float tangentWeight = 1.0 - smoothstep(coverageRadius * 0.30, coverageRadius, tangentDistance);
    float planeWeight = 1.0 - smoothstep(radius * 0.10, coverageRadius * 0.45, planeDistance);
    if (tangentWeight <= 0.0 || planeWeight <= 0.0) {
        rejectBits |= 4u;
        return 0.0;
    }

    vec3 surfelToReceiver = normalize(delta + surfelNormal * 1e-5);
    float surfelHemisphere = clamp(dot(surfelNormal, surfelToReceiver), 0.0, 1.0);
    float receiverHemisphere = clamp(dot(receiverNormal, -surfelToReceiver), 0.0, 1.0);
    float orientationWeight = mix(0.35, 1.0, max(surfelHemisphere, receiverHemisphere));

    float historyWeight = clamp(s.irradianceHistory.w / 32.0, 0.10, 1.0);
    float sharedWeight = s.sharedIrradiance.w > 0.0001 ? mix(1.0, 1.18, clamp(s.sharedIrradiance.w, 0.0, 1.0)) : 1.0;
    float transformWeight = receiverTransformID == 0u || s.ids.x == 0u || receiverTransformID == s.ids.x ? 1.0 : 0.85;

    float radialWeight = RadialDepthValidity(surfelID, s, receiverWorldPos);
    if (radialWeight <= 0.001) {
        rejectBits |= 8u;
        return 0.0;
    }

    return tangentWeight * planeWeight * clamp(normalAlign, 0.0, 1.0) *
        orientationWeight * historyWeight * sharedWeight * transformWeight * radialWeight;
}

float ComputeFallbackSurfelWeight(
    SurfelRecord s,
    uint surfelID,
    vec3 receiverWorldPos,
    vec3 receiverNormal,
    uint receiverTransformID)
{
    if (!IsSurfelValid(s) || s.irradianceHistory.w <= 0.0) {
        return 0.0;
    }

    vec3 surfelPos = s.worldPositionRadius.xyz;
    vec3 surfelNormal = normalize(s.worldNormalRecycle.xyz);
    float normalAlign = dot(receiverNormal, surfelNormal);
    if (normalAlign <= 0.05) {
        return 0.0;
    }

    vec3 delta = receiverWorldPos - surfelPos;
    float radius = max(s.worldPositionRadius.w, 0.001);
    float signedPlaneDistance = dot(delta, surfelNormal);
    vec3 tangentDelta = delta - surfelNormal * signedPlaneDistance;
    float tangentDistance = length(tangentDelta);
    float planeDistance = abs(signedPlaneDistance);
    float coverageRadius = radius * 4.0;

    float tangentWeight = 1.0 - smoothstep(coverageRadius * 0.35, coverageRadius, tangentDistance);
    float planeWeight = 1.0 - smoothstep(radius * 0.50, coverageRadius * 0.75, planeDistance);
    if (tangentWeight <= 0.0 || planeWeight <= 0.0) {
        return 0.0;
    }

    float radialWeight = RadialDepthValidity(surfelID, s, receiverWorldPos);
    if (radialWeight <= 0.001) {
        return 0.0;
    }

    float historyWeight = clamp(s.irradianceHistory.w / 96.0, 0.0, 1.0);
    float sharedWeight = s.sharedIrradiance.w > 0.0001 ? mix(1.0, 1.10, clamp(s.sharedIrradiance.w, 0.0, 1.0)) : 1.0;
    float transformWeight = receiverTransformID == 0u || s.ids.x == 0u || receiverTransformID == s.ids.x ? 1.0 : 0.65;
    float normalWeight = smoothstep(0.05, 0.65, normalAlign);

    return tangentWeight * planeWeight * normalWeight * radialWeight * historyWeight * sharedWeight * transformWeight;
}

vec3 ResolveSurfelIrradiance(SurfelRecord s)
{
    vec3 history = max(s.irradianceHistory.rgb, vec3(0.0));
    if (s.sharedIrradiance.w <= 0.0001) {
        return history;
    }
    return mix(history, max(s.sharedIrradiance.rgb, vec3(0.0)), clamp(s.sharedIrradiance.w, 0.0, 0.45));
}

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(outIrradiance);
    if (any(greaterThanEqual(pixel, size))) {
        return;
    }

    vec2 uv = (vec2(pixel) + 0.5) / uResolution;
    float depth = textureLod(uDepth, uv, 0.0).r;
    if (depth >= 0.999999 || surfelHeader.counts.y == 0u || !HasDiffuseResponse(uv)) {
        imageStore(outIrradiance, pixel, vec4(0.0));
        imageStore(outDebug, pixel, vec4(0.0));
        return;
    }

    vec4 packedNormalRM = textureLod(uPackedNormalRM, uv, 0.0);
    vec3 receiverNormal = DecodeNormalOct(packedNormalRM.rg);
    if (length(receiverNormal) < 0.5 || any(isnan(receiverNormal))) {
        receiverNormal = vec3(0.0, 1.0, 0.0);
    }
    receiverNormal = normalize(receiverNormal);

    vec3 receiverWorldPos = ReconstructWorldPosition(uv, depth);
    vec3 receiverViewRelative = receiverWorldPos - uViewPos;
    vec3 gridCoord = NonLinearGridCoord(receiverViewRelative, surfelHeader);
    uvec3 dims = max(surfelHeader.gridDims.xyz, uvec3(1u));
    ivec3 baseCell = ivec3(floor(gridCoord));
    int neighborRadius = clamp(uNeighborRadius, 0, 2);
    int maxCandidates = clamp(uMaxCandidates, 1, 256);
    int maxAccepted = clamp(uMaxAccepted, 1, 64);
    uint receiverTransformID = texelFetch(uTransformID, pixel, 0).r;

    vec3 weightedIrradiance = vec3(0.0);
    float weightSum = 0.0;
    vec3 fallbackIrradiance = vec3(0.0);
    float fallbackWeightSum = 0.0;
    int candidateCount = 0;
    int acceptedCount = 0;
    uint rejectMask = 0u;
    int queryCellCount = 0;

    int queryWidth = neighborRadius * 2 + 1;
    int queryCellBudget = max(queryWidth * queryWidth * queryWidth, 1);
    int perCellBudget = max(1, int(ceil(float(maxCandidates) / float(queryCellBudget))));

    for (int shell = 0; shell <= neighborRadius; ++shell) {
        for (int z = -shell; z <= shell; ++z) {
            for (int y = -shell; y <= shell; ++y) {
                for (int x = -shell; x <= shell; ++x) {
                    ivec3 offset = ivec3(x, y, z);
                    if (max(max(abs(offset.x), abs(offset.y)), abs(offset.z)) != shell) {
                        continue;
                    }

                    ivec3 cellI = baseCell + offset;
                    if (any(lessThan(cellI, ivec3(0))) || any(greaterThanEqual(cellI, ivec3(dims)))) {
                        continue;
                    }
                    ++queryCellCount;

                    uint cell = GridCellIndexFromCoord(uvec3(cellI), surfelHeader);
                    uint count = min(persistentSurfelGridHeaders[cell].x, surfelHeader.gridDims.w);
                    uint remainingBudget = uint(max(maxCandidates - candidateCount, 0));
                    uint sampleCount = min(min(count, remainingBudget), uint(perCellBudget));
                    for (uint j = 0u; j < sampleCount; ++j) {
                        if (candidateCount >= maxCandidates) {
                            break;
                        }
                        ++candidateCount;

                        uint i = count <= sampleCount ? j : min((j * count) / max(sampleCount, 1u), count - 1u);
                        uint surfelID = persistentSurfelGridEntries[cell * surfelHeader.gridDims.w + i];
                        if (surfelID >= surfelHeader.counts.x) {
                            rejectMask |= 1u;
                            continue;
                        }

                        SurfelRecord s = persistentSurfels[surfelID];
                        float fallbackW = ComputeFallbackSurfelWeight(s, surfelID, receiverWorldPos, receiverNormal, receiverTransformID);
                        if (fallbackW > 0.0001) {
                            fallbackIrradiance += ResolveSurfelIrradiance(s) * fallbackW;
                            fallbackWeightSum += fallbackW;
                        }

                        uint localReject = 0u;
                        float w = ComputeSurfelWeight(s, surfelID, receiverWorldPos, receiverNormal, receiverTransformID, localReject);
                        rejectMask |= localReject;
                        if (w <= 0.0001) {
                            continue;
                        }

                        weightedIrradiance += ResolveSurfelIrradiance(s) * w;
                        weightSum += w;
                        ++acceptedCount;
                    }
                    if (candidateCount >= maxCandidates) {
                        break;
                    }
                }
                if (candidateCount >= maxCandidates) {
                    break;
                }
            }
            if (candidateCount >= maxCandidates) {
                break;
            }
        }
        if (candidateCount >= maxCandidates) {
            break;
        }
    }

    vec3 irradiance = vec3(0.0);
    float confidence = 0.0;
    float fallbackUsed = 0.0;
    if (weightSum > 0.0001) {
        irradiance = weightedIrradiance / weightSum;
        confidence = clamp(weightSum / 2.0, 0.0, 1.0) * clamp(float(acceptedCount) / float(max(maxAccepted / 2, 1)), 0.0, 1.0);
    }

    float fallbackNeed = 1.0 - smoothstep(0.20, 0.70, confidence);
    if (fallbackNeed > 0.0001 && fallbackWeightSum > 0.0001) {
        vec3 fallback = fallbackIrradiance / fallbackWeightSum;
        float fallbackBlend = fallbackNeed * clamp(uFallbackStrength, 0.0, 1.0) * 0.65;
        irradiance = mix(irradiance, fallback, fallbackBlend);
        confidence = max(confidence, fallbackBlend * 0.55);
        fallbackUsed = fallbackBlend;
    }

    irradiance = max(irradiance, vec3(0.0));
    imageStore(outIrradiance, pixel, vec4(irradiance, clamp(confidence, 0.0, 1.0)));

    vec3 debug = irradiance;
    if (uDebugMode == 1) {
        debug = vec3(float(candidateCount) / float(max(maxCandidates, 1)));
    } else if (uDebugMode == 2) {
        debug = vec3(float(acceptedCount) / float(max(maxAccepted, 1)));
    } else if (uDebugMode == 3) {
        debug = vec3(clamp(weightSum / 3.0, 0.0, 1.0));
    } else if (uDebugMode == 4) {
        debug = vec3(confidence);
    } else if (uDebugMode == 5) {
        debug = vec3(fallbackUsed);
    } else if (uDebugMode == 6) {
        debug = vec3((rejectMask & 4u) != 0u ? 1.0 : 0.0, (rejectMask & 2u) != 0u ? 1.0 : 0.0, (rejectMask & 8u) != 0u ? 1.0 : 0.0);
    }
    imageStore(outDebug, pixel, vec4(max(debug, vec3(0.0)), clamp(confidence, 0.0, 1.0)));
}
