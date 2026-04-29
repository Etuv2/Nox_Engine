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
uniform int uSurfelGIValidationMode;
uniform int uSurfelGIValidationSelectedSurfel;

void main()
{
    uint localIndex = gl_GlobalInvocationID.x;
    uint surfelCount = uint(max(uSurfelCount, 0));
    if (localIndex >= surfelCount || header.counts.x == 0u) {
        return;
    }

    uint id = (uint(max(uSurfelStart, 0)) + localIndex) % header.counts.x;
    if (uSurfelGIValidationSelectedSurfel >= 0 && id != uint(uSurfelGIValidationSelectedSurfel)) {
        return;
    }

    SurfelRecord s = surfels[id];
    if (!IsSurfelValid(s) || s.rawIrradiance.w <= 0.0) {
        return;
    }

    uint frameIndex = uint(max(uFrameIndex, 0));
    if (uint(max(s.solveState.w, 0.0)) != frameIndex) {
        return;
    }

    // Temporal history only integrates the newest ray sample; sharedIrradiance stays transient.
    vec3 sampleIrradiance = max(s.rawIrradiance.rgb, vec3(0.0));
    sampleIrradiance = max(sampleIrradiance, vec3(0.0));
    float sampleLuma = LumaSurfel(sampleIrradiance);

    float shortSamples = min(s.shortTermStats.w + 1.0, 256.0);
    float previousShortMean = s.shortTermStats.x;
    float shortAlpha = s.shortTermStats.w <= 1.0 ? 1.0 : 0.22;
    float shortResidual = sampleLuma - previousShortMean;
    float shortMean = mix(previousShortMean, sampleLuma, shortAlpha);
    float shortVariance = mix(max(s.shortTermStats.y, 0.0), shortResidual * shortResidual, shortAlpha);

    float longSamples = min(s.longTermStats.z + 1.0, 65535.0);
    float previousLongMean = s.longTermStats.x;
    float longResidual = sampleLuma - previousLongMean;
    float longAlphaStats = clamp(1.0 / max(longSamples, 1.0), 0.002, 0.030);
    float longMean = mix(previousLongMean, sampleLuma, longAlphaStats);
    float longVariance = mix(max(s.longTermStats.y, 0.0), longResidual * longResidual, longAlphaStats);

    float varianceScale = sqrt(max(shortVariance, 0.000001));
    float meanShift = abs(shortMean - previousLongMean) / max(varianceScale + sqrt(max(longVariance, 0.0)), 0.01);
    float rgbDelta = length(sampleIrradiance - s.irradianceHistory.rgb);
    float instability = clamp(meanShift * 0.22 + rgbDelta * 0.18, 0.0, 1.0);

    float historySamples = max(s.irradianceHistory.w, 0.0);
    if (uSurfelGIValidationMode == SURFEL_GI_VALIDATION_MODE_SINGLE_SURFEL) {
        float arithmeticAlpha = 1.0 / max(historySamples + 1.0, 1.0);
        s.irradianceHistory.rgb = mix(s.irradianceHistory.rgb, sampleIrradiance, arithmeticAlpha);
        s.irradianceHistory.w = min(historySamples + 1.0, 65535.0);
        s.shortTermStats = vec4(sampleLuma, 0.0, 0.0, min(s.shortTermStats.w + 1.0, 256.0));
        s.longTermStats = vec4(sampleLuma, 0.0, min(s.longTermStats.z + 1.0, 65535.0), min(s.longTermStats.w + 1.0, 65535.0));
        s.frames.z = frameIndex;
        s.lightingState = vec4(
            float(SurfelLightingStateFromHistory(s)),
            SurfelHistoryConfidence(s),
            0.0,
            0.0);
        surfels[id] = s;
        atomicAdd(irradianceHeader.irradianceDebugSumsFixed.x, SurfelEncodeDebugSum(sampleLuma));
        atomicAdd(irradianceHeader.irradianceDebugSumsFixed.y, SurfelEncodeDebugSum(LumaSurfel(max(s.irradianceHistory.rgb, vec3(0.0)))));
        atomicAdd(irradianceHeader.irradianceDebugSumsFixed.z, SurfelEncodeDebugSum(LumaSurfel(max(s.sharedIrradiance.rgb, vec3(0.0)))));
        atomicAdd(irradianceHeader.irradianceDebugSumsFixed.w, 1u);
        return;
    }

    float bootstrapAlpha = historySamples < 4.0 ? 1.0 / max(historySamples + 1.0, 1.0) : 0.0;
    float stableAlpha = mix(0.035, 0.012, clamp(historySamples / 96.0, 0.0, 1.0));
    float reactiveAlpha = mix(stableAlpha, 0.32, smoothstep(0.22, 0.95, instability));
    float noiseGuard = 1.0 - smoothstep(0.18, 0.85, sqrt(shortVariance));
    float alpha = bootstrapAlpha > 0.0 ? bootstrapAlpha : mix(stableAlpha, reactiveAlpha, max(instability, noiseGuard * 0.15));

    s.irradianceHistory.rgb = mix(s.irradianceHistory.rgb, sampleIrradiance, clamp(alpha, 0.01, 1.0));
    s.irradianceHistory.w = min(historySamples + 1.0, 65535.0);
    s.shortTermStats = vec4(shortMean, shortVariance, instability, shortSamples);
    s.longTermStats = vec4(longMean, longVariance, longSamples, min(s.longTermStats.w + 1.0, 65535.0));
    s.frames.z = frameIndex;
    s.lightingState = vec4(
        float(SurfelLightingStateFromHistory(s)),
        SurfelHistoryConfidence(s),
        max(shortVariance, longVariance),
        instability);
    surfels[id] = s;
    atomicAdd(irradianceHeader.irradianceDebugSumsFixed.x, SurfelEncodeDebugSum(sampleLuma));
    atomicAdd(irradianceHeader.irradianceDebugSumsFixed.y, SurfelEncodeDebugSum(LumaSurfel(max(s.irradianceHistory.rgb, vec3(0.0)))));
    atomicAdd(irradianceHeader.irradianceDebugSumsFixed.z, SurfelEncodeDebugSum(LumaSurfel(max(s.sharedIrradiance.rgb, vec3(0.0)))));
    atomicAdd(irradianceHeader.irradianceDebugSumsFixed.w, 1u);
}
