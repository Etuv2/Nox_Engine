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
layout(binding = 7) uniform usampler2D uWinnerSurfelID;
layout(binding = 8) uniform sampler2D uHistoryIrradiance;
layout(binding = 9) uniform sampler2D uVelocity;

layout(binding = 0, rgba16f) writeonly uniform image2D outIrradiance;
layout(binding = 1, rgba16f) writeonly uniform image2D outDebug;

layout(std430, binding = 20) readonly buffer PersistentSurfelBuffer { SurfelRecord persistentSurfels[]; };
layout(std430, binding = 21) readonly buffer PersistentSurfelHeaderBuffer { SurfelPoolHeader surfelHeader; };
layout(std430, binding = 25) readonly buffer PersistentSurfelGridHeaderBuffer { uvec4 persistentSurfelGridHeaders[]; };
layout(std430, binding = 26) readonly buffer PersistentSurfelGridEntryBuffer { uint persistentSurfelGridEntries[]; };
layout(std430, binding = 28) buffer SurfelIrradianceHeaderBuffer { SurfelIrradianceHeader irradianceHeader; };
layout(std430, binding = 30) readonly buffer SurfelRadialDepthBinsBuffer { vec4 radialDepthBins[]; };
layout(std430, binding = 33) readonly buffer SurfelGridCellAverageBuffer { SurfelGridCellAverage gridCellAverages[]; };

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
uniform int uDisableRadialDepthReject;
uniform int uDisableNormalReject;
uniform int uDisableConfidenceReject;
uniform int uDisableFallback;
uniform int uUseTemporal;
uniform int uTemporalReset;
uniform int uHasVelocityHistory;
uniform float uTemporalAlpha;

const float SURFEL_INDIRECT_RESPONSE_SCALE = 2.5;

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

    vec3 surfelNormal = SurfelStableNormal(s.worldNormalRecycle.xyz);
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
    if (!IsSurfelValid(s)) {
        rejectBits |= 1u;
        return 0.0;
    }
    if (s.irradianceHistory.w <= 0.0 && uDisableConfidenceReject == 0) {
        rejectBits |= 1u;
        return 0.0;
    }

    vec3 surfelPos = s.worldPositionRadius.xyz;
    vec3 surfelNormal = SurfelStableNormal(s.worldNormalRecycle.xyz);
    float normalAlign = dot(receiverNormal, surfelNormal);
    if (normalAlign < 0.35 && uDisableNormalReject == 0) {
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

    float historyWeight = uDisableConfidenceReject != 0 ? 1.0 : clamp(s.irradianceHistory.w / 32.0, 0.10, 1.0);
    float transformWeight = receiverTransformID == 0u || s.ids.x == 0u || receiverTransformID == s.ids.x ? 1.0 : 0.85;

    float radialWeight = uDisableRadialDepthReject != 0 ? 1.0 : RadialDepthValidity(surfelID, s, receiverWorldPos);
    if (radialWeight <= 0.001) {
        rejectBits |= 8u;
        return 0.0;
    }

    return tangentWeight * planeWeight * clamp(normalAlign, 0.0, 1.0) *
        orientationWeight * historyWeight * transformWeight * radialWeight;
}

float ComputeFallbackSurfelWeight(
    SurfelRecord s,
    uint surfelID,
    vec3 receiverWorldPos,
    vec3 receiverNormal,
    uint receiverTransformID)
{
    if (!IsSurfelValid(s)) {
        return 0.0;
    }
    if (s.irradianceHistory.w <= 0.0 && uDisableConfidenceReject == 0) {
        return 0.0;
    }

    vec3 surfelPos = s.worldPositionRadius.xyz;
    vec3 surfelNormal = SurfelStableNormal(s.worldNormalRecycle.xyz);
    float normalAlign = dot(receiverNormal, surfelNormal);
    if (normalAlign <= 0.05 && uDisableNormalReject == 0) {
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

    float radialWeight = uDisableRadialDepthReject != 0 ? 1.0 : RadialDepthValidity(surfelID, s, receiverWorldPos);
    if (radialWeight <= 0.001) {
        return 0.0;
    }

    float historyWeight = uDisableConfidenceReject != 0 ? 1.0 : clamp(s.irradianceHistory.w / 96.0, 0.0, 1.0);
    float transformWeight = receiverTransformID == 0u || s.ids.x == 0u || receiverTransformID == s.ids.x ? 1.0 : 0.65;
    float normalWeight = smoothstep(0.05, 0.65, normalAlign);

    return tangentWeight * planeWeight * normalWeight * radialWeight * historyWeight * transformWeight;
}

vec3 ResolveSurfelIrradiance(SurfelRecord s)
{
    return max(s.irradianceHistory.rgb, vec3(0.0));
}

void AccumulateSmoothCellAverage(
    ivec3 cellCoord,
    vec3 gridCoord,
    vec3 receiverNormal,
    inout vec3 cellAverageIrradiance,
    inout float cellAverageWeightSum)
{
    uvec3 dims = max(surfelHeader.gridDims.xyz, uvec3(1u));
    if (uDisableFallback != 0 ||
        any(lessThan(cellCoord, ivec3(0))) ||
        any(greaterThanEqual(cellCoord, ivec3(dims)))) {
        return;
    }

    uint cell = GridCellIndexFromCoord(uvec3(cellCoord), surfelHeader);
    SurfelGridCellAverage average = gridCellAverages[cell];
    float confidence = clamp(average.irradianceWeight.w, 0.0, 1.0);
    vec3 irradiance = max(average.irradianceWeight.rgb, vec3(0.0));
    if (confidence <= 0.001 || Luminance(irradiance) <= 0.000001) {
        return;
    }

    vec3 averageNormal = average.normalCount.w > 0.5 && length(average.normalCount.xyz) > 0.001
        ? normalize(average.normalCount.xyz)
        : receiverNormal;
    float normalAlign = clamp(dot(receiverNormal, averageNormal), 0.0, 1.0);
    float normalWeight = uDisableNormalReject != 0 ? 1.0 : smoothstep(0.04, 0.55, normalAlign);
    if (normalWeight <= 0.0001) {
        return;
    }

    vec3 cellCenter = vec3(cellCoord) + vec3(0.5);
    vec3 cellDelta = gridCoord - cellCenter;
    float localityWeight = exp(-dot(cellDelta, cellDelta) * 0.85);
    float populationWeight = clamp(average.normalCount.w / 8.0, 0.15, 1.0);
    float weight = confidence * normalWeight * localityWeight * populationWeight;
    if (weight <= 0.0001) {
        return;
    }

    cellAverageIrradiance += irradiance * weight;
    cellAverageWeightSum += weight;
}

bool ResolveSmoothCellAverageFallback(
    vec3 gridCoord,
    vec3 receiverNormal,
    out vec3 averageIrradiance,
    out float averageConfidence)
{
    averageIrradiance = vec3(0.0);
    averageConfidence = 0.0;
    if (uDisableFallback != 0) {
        return false;
    }

    ivec3 baseCell = ivec3(floor(gridCoord));
    vec3 sumIrradiance = vec3(0.0);
    float sumWeight = 0.0;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                AccumulateSmoothCellAverage(
                    baseCell + ivec3(x, y, z),
                    gridCoord,
                    receiverNormal,
                    sumIrradiance,
                    sumWeight);
            }
        }
    }

    if (sumWeight <= 0.0001) {
        return false;
    }

    averageIrradiance = sumIrradiance / sumWeight;
    averageConfidence = clamp(sumWeight / 2.5, 0.0, 1.0);
    return true;
}

vec4 ClampHistoryToCurrentEstimate(vec4 history, vec3 currentIrradiance, float currentConfidence)
{
    float currentLuma = Luminance(currentIrradiance);
    float allowance = mix(0.035, 0.35, clamp(currentConfidence, 0.0, 1.0));
    vec3 extent = max(vec3(allowance), max(currentIrradiance, vec3(0.0)) * mix(0.55, 1.15, currentConfidence));
    vec3 clamped = clamp(max(history.rgb, vec3(0.0)), max(currentIrradiance - extent, vec3(0.0)), currentIrradiance + extent);
    if (currentLuma <= 0.0005 && currentConfidence <= 0.03) {
        clamped *= 0.25;
    }
    return vec4(clamped, clamp(history.a, 0.0, 1.0));
}

vec3 ComputeFinalDiffuseIndirect(vec3 indirectIrradiance, vec2 uv, vec3 receiverWorldPos, vec3 receiverNormal)
{
    vec4 rm = textureLod(uPackedNormalRM, uv, 0.0);
    vec4 albedoAO = textureLod(uAlbedoAO, uv, 0.0);
    vec3 albedo = albedoAO.rgb;
    float metallic = clamp(rm.a, 0.0, 1.0);
    float roughness = ClampPerceptualRoughness(rm.b);
    vec3 specularF0 = clamp(textureLod(uSpecularF0, uv, 0.0).rgb, vec3(0.0), vec3(1.0));
    float transmission = clamp(textureLod(uPrincipledParams, uv, 0.0).r, 0.0, 1.0);
    vec3 V = normalize(uViewPos - receiverWorldPos);
    float NdotV = Saturate(dot(receiverNormal, V));
    vec3 F = FresnelSchlickRoughness(NdotV, specularF0, roughness);
    vec3 kD = clamp(vec3(1.0) - F, vec3(0.0), vec3(1.0));
    float transmissionWeight = ComputeTransmissionWeight(transmission, NdotV, specularF0);
    vec3 diffuseAlbedo = albedo * (1.0 - metallic) * (1.0 - transmissionWeight);
    float aoPolicy = mix(0.55, 1.0, clamp(albedoAO.a, 0.0, 1.0));
    return max(indirectIrradiance * kD * diffuseAlbedo * INV_PI * aoPolicy * SURFEL_INDIRECT_RESPONSE_SCALE, vec3(0.0));
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

    vec3 receiverWorldPos = ReconstructWorldPosition(uv, depth);
    vec4 packedNormalRM = textureLod(uPackedNormalRM, uv, 0.0);
    vec3 receiverNormal = DecodeNormalOct(packedNormalRM.rg);
    if (length(receiverNormal) < 0.5 || any(isnan(receiverNormal))) {
        receiverNormal = vec3(0.0, 1.0, 0.0);
    }
    receiverNormal = SurfelStableNormal(receiverNormal);

    vec3 receiverViewRelative = receiverWorldPos - uViewPos;
    vec3 gridCoord = NonLinearGridCoord(receiverViewRelative, surfelHeader);
    uvec3 dims = max(surfelHeader.gridDims.xyz, uvec3(1u));
    ivec3 baseCell = ivec3(floor(gridCoord));
    int neighborRadius = clamp(uNeighborRadius, 0, 2);
    int maxCandidates = clamp(uMaxCandidates, 1, 256);
    int maxAccepted = clamp(uMaxAccepted, 1, 64);
    ivec2 transformSize = textureSize(uTransformID, 0);
    ivec2 transformPixel = transformSize.x > 0 && transformSize.y > 0
        ? clamp(ivec2(uv * vec2(transformSize)), ivec2(0), transformSize - ivec2(1))
        : pixel;
    uint receiverTransformID = texelFetch(uTransformID, transformPixel, 0).r;

    vec3 weightedIrradiance = vec3(0.0);
    float weightSum = 0.0;
    vec3 fallbackIrradiance = vec3(0.0);
    float fallbackWeightSum = 0.0;
    vec3 cellAverageIrradiance = vec3(0.0);
    float cellAverageConfidence = 0.0;
    bool hasCellAverageFallback = ResolveSmoothCellAverageFallback(
        gridCoord,
        receiverNormal,
        cellAverageIrradiance,
        cellAverageConfidence);
    int candidateCount = 0;
    int acceptedCount = 0;
    uint rejectMask = 0u;
    int rejectDistanceCount = 0;
    int rejectNormalCount = 0;
    int rejectRadialDepthCount = 0;
    int rejectConfidenceHistoryCount = 0;
    int queryCellCount = 0;

    int queryWidth = neighborRadius * 2 + 1;
    int queryCellBudget = max(queryWidth * queryWidth * queryWidth, 1);
    int perCellBudget = max(1, int(ceil(float(maxCandidates) / float(queryCellBudget))));
    bool gatherComplete = false;

    uint winnerSurfelID = 0xffffffffu;
    ivec2 winnerSize = textureSize(uWinnerSurfelID, 0);
    if (winnerSize.x > 0 && winnerSize.y > 0) {
        ivec2 winnerPixel = clamp(ivec2(uv * vec2(winnerSize)), ivec2(0), winnerSize - ivec2(1));
        winnerSurfelID = SurfelUnpackWinnerID(texelFetch(uWinnerSurfelID, winnerPixel, 0).r);
    }

    if (winnerSurfelID < surfelHeader.counts.x) {
        ++candidateCount;
        SurfelRecord s = persistentSurfels[winnerSurfelID];
        if (uDisableFallback == 0) {
            float fallbackW = ComputeFallbackSurfelWeight(s, winnerSurfelID, receiverWorldPos, receiverNormal, receiverTransformID);
            if (fallbackW > 0.0001) {
                fallbackIrradiance += ResolveSurfelIrradiance(s) * fallbackW;
                fallbackWeightSum += fallbackW;
            }
        }

        uint localReject = 0u;
        float w = ComputeSurfelWeight(s, winnerSurfelID, receiverWorldPos, receiverNormal, receiverTransformID, localReject);
        rejectMask |= localReject;
        if (w > 0.0001) {
            weightedIrradiance += ResolveSurfelIrradiance(s) * w;
            weightSum += w;
            ++acceptedCount;
            gatherComplete = acceptedCount >= maxAccepted && weightSum >= 0.45;
        } else {
            rejectConfidenceHistoryCount += int((localReject & 1u) != 0u);
            rejectNormalCount += int((localReject & 2u) != 0u);
            rejectDistanceCount += int((localReject & 4u) != 0u);
            rejectRadialDepthCount += int((localReject & 8u) != 0u);
        }
    }

    for (int shell = 0; shell <= neighborRadius && !gatherComplete; ++shell) {
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
                        if (surfelID == winnerSurfelID) {
                            continue;
                        }
                        if (surfelID >= surfelHeader.counts.x) {
                            rejectMask |= 1u;
                            ++rejectConfidenceHistoryCount;
                            continue;
                        }

                        SurfelRecord s = persistentSurfels[surfelID];
                        if (uDisableFallback == 0) {
                            float fallbackW = ComputeFallbackSurfelWeight(s, surfelID, receiverWorldPos, receiverNormal, receiverTransformID);
                            if (fallbackW > 0.0001) {
                                fallbackIrradiance += ResolveSurfelIrradiance(s) * fallbackW;
                                fallbackWeightSum += fallbackW;
                            }
                        }

                        uint localReject = 0u;
                        float w = ComputeSurfelWeight(s, surfelID, receiverWorldPos, receiverNormal, receiverTransformID, localReject);
                        rejectMask |= localReject;
                        if (w <= 0.0001) {
                            rejectConfidenceHistoryCount += int((localReject & 1u) != 0u);
                            rejectNormalCount += int((localReject & 2u) != 0u);
                            rejectDistanceCount += int((localReject & 4u) != 0u);
                            rejectRadialDepthCount += int((localReject & 8u) != 0u);
                            continue;
                        }

                        weightedIrradiance += ResolveSurfelIrradiance(s) * w;
                        weightSum += w;
                        ++acceptedCount;
                        gatherComplete = acceptedCount >= maxAccepted && weightSum >= 0.75;
                        if (gatherComplete) {
                            break;
                        }
                    }
                    if (candidateCount >= maxCandidates || gatherComplete) {
                        break;
                    }
                }
                if (candidateCount >= maxCandidates || gatherComplete) {
                    break;
                }
            }
            if (candidateCount >= maxCandidates || gatherComplete) {
                break;
            }
        }
        if (candidateCount >= maxCandidates || gatherComplete) {
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

    if (uDisableFallback == 0 && hasCellAverageFallback) {
        float directWeight = clamp(weightSum, 0.0, 1.0);
        float cellFillWeight = (1.0 - directWeight) * cellAverageConfidence *
            clamp(uFallbackStrength, 0.0, 1.0) * 0.75;
        if (cellFillWeight > 0.0001) {
            float combinedWeight = max(directWeight + cellFillWeight, 0.0001);
            irradiance = (irradiance * directWeight + cellAverageIrradiance * cellFillWeight) / combinedWeight;
            confidence = max(confidence, clamp(cellFillWeight * 0.85, 0.0, 1.0));
            fallbackUsed = max(fallbackUsed, cellFillWeight);
        }
    }

    float fallbackNeed = uDisableFallback != 0 ? 0.0 : 1.0 - smoothstep(0.20, 0.70, confidence);
    if (fallbackNeed > 0.0001 && fallbackWeightSum > 0.0001) {
        vec3 fallback = fallbackIrradiance / fallbackWeightSum;
        float fallbackBlend = fallbackNeed * clamp(uFallbackStrength, 0.0, 1.0) * 0.65;
        irradiance = mix(irradiance, fallback, fallbackBlend);
        confidence = max(confidence, fallbackBlend * 0.55);
        fallbackUsed = fallbackBlend;
    }

    irradiance = max(irradiance, vec3(0.0));
    confidence = clamp(confidence, 0.0, 1.0);

    if (uUseTemporal != 0 && uTemporalReset == 0) {
        vec2 historyUV = (vec2(pixel) + vec2(0.5)) / uResolution;
        float velocityPixels = 0.0;
        float reprojectionValid = 1.0;
        if (uHasVelocityHistory != 0) {
            vec2 velocity = textureLod(uVelocity, uv, 0.0).rg;
            historyUV -= velocity;
            velocityPixels = length(velocity * uResolution);
            vec2 margin = 1.5 / uResolution;
            reprojectionValid = (all(greaterThanEqual(historyUV, margin)) &&
                all(lessThanEqual(historyUV, vec2(1.0) - margin))) ? 1.0 : 0.0;
        }

        vec4 history = reprojectionValid > 0.5
            ? ClampHistoryToCurrentEstimate(textureLod(uHistoryIrradiance, historyUV, 0.0), irradiance, confidence)
            : vec4(0.0);
        vec3 historyIrradiance = max(history.rgb, vec3(0.0));
        float historyConfidence = clamp(history.a, 0.0, 1.0);
        float currentLuma = Luminance(irradiance);
        float historyLuma = Luminance(historyIrradiance);
        float currentValid = confidence > 0.025 || currentLuma > 0.001 ? 1.0 : 0.0;
        float motionTrust = uHasVelocityHistory != 0 ? exp(-velocityPixels * 0.075) : 1.0;
        float historyTrust = smoothstep(0.02, 0.45, historyConfidence) * motionTrust * reprojectionValid;
        float baseAlpha = clamp(uTemporalAlpha, 0.04, 1.0);
        float sparseAlpha = clamp(baseAlpha * 0.35, 0.02, 0.20);
        float reactive = smoothstep(0.20, 1.25, abs(currentLuma - historyLuma));
        float motionReactive = smoothstep(4.0, 28.0, velocityPixels);
        float blendAlpha = mix(sparseAlpha, baseAlpha, currentValid);
        blendAlpha = mix(blendAlpha, max(blendAlpha, 0.45), reactive);
        blendAlpha = mix(blendAlpha, max(blendAlpha, 0.88), motionReactive);
        blendAlpha = mix(1.0, blendAlpha, historyTrust);
        irradiance = mix(historyIrradiance, irradiance, blendAlpha);
        confidence = max(mix(historyConfidence * 0.985, confidence, blendAlpha), confidence * currentValid);
    }

    imageStore(outIrradiance, pixel, vec4(irradiance, confidence));

    float candidateDenom = float(max(candidateCount, 1));
    vec3 finalBRDFScaledIndirect = ComputeFinalDiffuseIndirect(
        irradiance,
        uv,
        receiverWorldPos,
        receiverNormal);
    atomicAdd(irradianceHeader.gatherStats.x, uint(max(candidateCount, 0)));
    atomicAdd(irradianceHeader.gatherStats.y, uint(max(acceptedCount, 0)));
    if (fallbackUsed > 0.0001) {
        atomicAdd(irradianceHeader.gatherStats.z, 1u);
    }
    atomicAdd(irradianceHeader.gatherDebugSumsFixed.x, SurfelEncodeDebugSum(Luminance(irradiance)));
    atomicAdd(irradianceHeader.gatherDebugSumsFixed.y, SurfelEncodeDebugSum(Luminance(finalBRDFScaledIndirect)));
    atomicAdd(irradianceHeader.gatherDebugSumsFixed.z, SurfelEncodeDebugSum(weightSum));
    atomicAdd(irradianceHeader.gatherDebugSumsFixed.w, 1u);

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
    } else if (uDebugMode == 7) {
        debug = vec3(float(rejectDistanceCount) / candidateDenom);
    } else if (uDebugMode == 8) {
        debug = vec3(float(rejectNormalCount) / candidateDenom);
    } else if (uDebugMode == 9) {
        debug = vec3(float(rejectRadialDepthCount) / candidateDenom);
    } else if (uDebugMode == 10) {
        debug = vec3(float(rejectConfidenceHistoryCount) / candidateDenom);
    } else if (uDebugMode == 11) {
        debug = finalBRDFScaledIndirect;
    }
    imageStore(outDebug, pixel, vec4(max(debug, vec3(0.0)), clamp(confidence, 0.0, 1.0)));
}
