from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    apply_shader = read("shaders/surfel_gi/apply_indirect.comp")
    upscale_shader = read("shaders/surfel_gi/upscale_filter.comp")
    cell_average_shader = read("shaders/surfel_gi/common.glsl")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")

    # Cell averages (common.glsl): support and signal gates on the fallback confidence.
    require("vec3 SurfelCellAverage(" in cell_average_shader and
            "supportConfidence = smoothstep(1.0, 8.0, validCount)" in cell_average_shader,
            "cell averages must scale fallback confidence by surfel support")
    require("signalConfidence = smoothstep(1e-4, 0.03, SurfelLuminance(average))" in cell_average_shader,
            "cell averages must not advertise confident fallback for black/no-signal cells")

    # Final gather (GIBS p.40, spec 5.14): unnormalised sum plus the receiver cell's average for the
    # uncovered share of the weight, with coverage = min(sum w, 1) kept for the spawn pass.
    require("float cellShare = clamp(uFallbackStrength, 0.0, 1.0) * max(0.0, 1.0 - sumWeight);" in apply_shader,
            "final gather must weight the cell average by the uncovered share 1 - sum(w)")
    require("vec3 surfelIrradiance = sumIrradiance / max(1.0, sumRampWeight);" in apply_shader and
            "vec3 irradiance = max(surfelIrradiance + cellIrradiance * cellShare, vec3(0.0));" in apply_shader,
            "final gather must be sum(w E) / max(1, sum(w)) + E_cell * max(0, 1 - sum(w)): unnormalised below one")
    require("float coverage = clamp(sumWeight, 0.0, 1.0);" in apply_shader and
            "imageStore(uOutCoverage, pixel, vec4(coverage, 0.0, 0.0, 1.0));" in apply_shader,
            "gather coverage must stay min(sum w, 1) in the coverage texture for the spawn pass")
    require("sumIrradiance / sumRampWeight" not in apply_shader and
            "SampleTrilinearGridIrradiance" not in apply_shader and
            "BoundedFallbackBlend" not in apply_shader,
            "the normalised average and the trilinear fallback must not come back")
    require("acceptedCount >= maxAccepted" in apply_shader and
            "uMaxGatherSurfelsPerPixel" in apply_shader,
            "final gather must take the first N accepted surfels (bounded by N)")
    require("uMaxGatherCandidates" in apply_shader and "examinedCount >= maxExamined" in apply_shader,
            "final gather must bound the entries examined per texel")

    # Upscale (the only screen-space pass after the gather): depth- and normal-aware, four taps.
    require("for (int k = 0; k < 4; ++k)" in upscale_shader and
            "SurfelUpscaleDepthWeight(centerDepth, sampleDepth)" in upscale_shader and
            "SurfelUpscaleNormalWeight(centerNormal, sampleNormal)" in upscale_shader,
            "upscale must be a four-tap depth and normal joint bilateral")
    require("upscale_filter" in pipeline_cpp and "spatial_filter" not in pipeline_cpp and
            "temporal_filter" not in pipeline_cpp,
            "pipeline must run the upscale only, with no spatial or temporal screen filter")


if __name__ == "__main__":
    main()
