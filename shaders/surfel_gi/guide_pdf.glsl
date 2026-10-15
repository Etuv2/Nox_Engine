#ifndef NOX_SURFEL_GUIDE_PDF_GLSL
#define NOX_SURFEL_GUIDE_PDF_GLSL

// Guided ray density of a surfel, shared by generate_rays.comp (sampling) and trace_rays.comp (the MIS weight of the
// next-event sample at the ray origin, plan section 14). The includer declares `guideCells` (B_GUIDE_MAP) and the
// uniform uLightingChangeFactor before including this file.

// Area of each guide bin inside the unit disk (the directions a hemisphere can take), in xy units. Computed
// offline on a 3000^2 grid; the 36 values sum to pi. A bin's guided density is P / area, so the pdf matches a
// sampler that is uniform over the bin's part of the disk.
const float SURFEL_GUIDE_BIN_AREA[36] = float[36](
    0.003211, 0.064040, 0.104831, 0.104831, 0.064040, 0.003211,
    0.064040, 0.111111, 0.111111, 0.111111, 0.111111, 0.064040,
    0.104831, 0.111111, 0.111111, 0.111111, 0.111111, 0.104831,
    0.104831, 0.111111, 0.111111, 0.111111, 0.111111, 0.104831,
    0.064040, 0.111111, 0.111111, 0.111111, 0.111111, 0.064040,
    0.003211, 0.064040, 0.104831, 0.104831, 0.064040, 0.003211);
const float SURFEL_GUIDE_MIX_PROBABILITY = 0.65;

float SurfelGuideMixProbability()
{
    return mix(SURFEL_GUIDE_MIX_PROBABILITY, 0.18, clamp(uLightingChangeFactor, 0.0, 1.0));
}

vec3 SurfelUnpackGuideRadiance(uint packedRadiance, out float confidence)
{
    vec4 guide = unpackUnorm4x8(packedRadiance);
    vec3 mapped = clamp(guide.rgb, vec3(0.0), vec3(0.999));
    confidence = clamp(guide.a, 0.0, 1.0);
    return mapped / max(vec3(1.0) - mapped, vec3(1e-4));
}

float SurfelGuideCellWeight(uint surfelID, uint guideBin)
{
    uint index = surfelID * SURFEL_GUIDE_CELLS + guideBin;
    uint packedRadiance = guideCells[index].packedRadiance;
    if (packedRadiance == 0u) {
        return 0.0;
    }

    float confidence;
    vec3 radiance = SurfelUnpackGuideRadiance(packedRadiance, confidence);
    return max(SurfelLuminance(radiance), 0.0) * max(confidence, 0.0);
}

float SurfelGuideTotalWeight(uint surfelID)
{
    float totalWeight = 0.0;
    for (uint guideBin = 0u; guideBin < SURFEL_GUIDE_CELLS; ++guideBin) {
        totalWeight += SurfelGuideCellWeight(surfelID, guideBin);
    }
    return totalWeight;
}

float SurfelGuidePdf(uint surfelID, vec3 hemiDir, out bool hasGuide)
{
    float totalWeight = SurfelGuideTotalWeight(surfelID);
    hasGuide = totalWeight > 1e-6 && !isnan(totalWeight) && !isinf(totalWeight);
    if (!hasGuide) {
        return 0.0;
    }

    uint guideBin = SurfelGuideBin(hemiDir);
    float selectedWeight = SurfelGuideCellWeight(surfelID, guideBin);
    return (selectedWeight / max(totalWeight, 1e-6)) /
        SURFEL_GUIDE_BIN_AREA[guideBin] *
        max(hemiDir.z, 0.0);
}

#endif // NOX_SURFEL_GUIDE_PDF_GLSL
