from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_caps_tuple(source: str, tier: str) -> list[str]:
    pattern = rf"case\s+SurfelGIQualityTier::{tier}\s*:\s*(?:default\s*:\s*)?return\s*\{{([^}}]+)\}};"
    match = re.search(pattern, source, re.S)
    require(match is not None, f"missing quality caps for {tier}")
    return [part.strip() for part in match.group(1).split(",")]


def uint_value(token: str) -> int:
    match = re.search(r"(\d+)u?", token)
    require(match is not None, f"expected integer token, got: {token}")
    return int(match.group(1))


def float_value(token: str) -> float:
    match = re.search(r"([0-9]+(?:\.[0-9]+)?)f?", token)
    require(match is not None, f"expected float token, got: {token}")
    return float(match.group(1))


def glsl_float_constant(source: str, name: str) -> float:
    match = re.search(rf"const\s+float\s+{re.escape(name)}\s*=\s*([0-9]+(?:\.[0-9]+)?)\s*;", source)
    require(match is not None, f"missing GLSL float constant: {name}")
    return float(match.group(1))


def main() -> None:
    modular = read("src/ModularRenderer.cpp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    apply_shader = read("shaders/surfel_gi/apply_indirect.comp")
    trace_shader = read("shaders/surfel_gi/trace_rays.comp")
    generate_shader = read("shaders/surfel_gi/generate_rays.comp")
    integrate_shader = read("shaders/surfel_gi/integrate.comp")
    deferred_shader = read("shaders/deferred_lighting_frag.glsl")
    debug_present_shader = read("shaders/indirect_diffuse_debug_present_frag.glsl")

    medium = parse_caps_tuple(modular, "Medium")
    require(len(medium) >= 20, "surfel GI quality caps tuple layout changed unexpectedly")
    max_ray_budget = uint_value(medium[1])
    max_gather = uint_value(medium[4])
    final_gather_scale = float_value(medium[6])
    fallback_strength = float_value(medium[7])
    allow_software_bvh = medium[11] == "true"
    max_indirect_intensity = float_value(medium[12])
    max_spawns = uint_value(medium[17])

    require(max_ray_budget <= 256,
            "Medium surfel GI must use a bounded realtime ray budget; High/Ultra carry expensive transport budgets")
    require(32 <= max_gather <= 40,
            "Medium surfel GI final gather must use a bounded realtime candidate budget; High/Ultra carry the heavier quality settings")
    require(0.32 <= final_gather_scale <= 0.33,
            "Medium surfel GI must not run the full-resolution validation gather path during realtime use")
    require(fallback_strength <= 0.25,
            "Medium surfel GI fallback must be low-confidence support, not the dominant indirect signal")
    require("std::max(caps.fallbackStrength, 0.65f)" not in modular and
            "settings.cellAverageFallbackStrength = captureMode ? std::min(settings.cellAverageFallbackStrength, caps.fallbackStrength) : settings.cellAverageFallbackStrength;" in modular,
            "capture-mode ground truth comparisons must not force high cell-average fallback that washes the surfel result toward white")
    require(max_indirect_intensity <= 1.0,
            "Medium surfel GI must not rely on the UI/env GI Intensity slider to hide underpowered transport")
    require(max_spawns >= 64,
            "Medium surfel GI must fill persistent coverage fast enough that irradiance can converge in a stationary view")
    require(not allow_software_bvh,
            "Medium surfel GI must keep the expensive software BVH path out of the 4 ms realtime preset")
    require("uBVHTraceStride" in pipeline and
            "m_settings.maxRayBudget >= 8192u" in pipeline and
            "m_settings.maxRayBudget >= 2048u ? 2u : 4u" in pipeline and
            "useBVHThisRay" in trace_shader,
            "Medium must throttle expensive software BVH rays through an explicit stride while High/Ultra keep denser BVH tracing")
    require("uniform float uLightingChangeFactor" in generate_shader and
            "SurfelGuideMixProbability" in generate_shader and
            "mix(SURFEL_GUIDE_MIX_PROBABILITY, 0.18" in generate_shader and
            "retainedConfidence" in integrate_shader,
            "lighting changes must reduce stale ray-guiding influence and relearn guide bins quickly")

    require("denoiseFullscreenDebug" in pipeline and
            "SurfelGIDebugView::RawIndirectIrradiance" in pipeline and
            "(!fullscreenDebug || denoiseFullscreenDebug)" in pipeline,
            "Raw Irradiance debug must show the surfel GI irradiance term after spatial/temporal stabilization, not only the one-frame sparse gather buffer")
    require("const SurfelGISettings oldSettings = m_settings" in pipeline and
            "oldSettings.debugView != settings.debugView" in pipeline and
            "m_hasTemporalHistory = false" in pipeline,
            "changing between raw irradiance debug and production output must reset temporal history so debug tonemapping cannot contaminate composite GI")
    require("DEBUG_RAW_INDIRECT_IRRADIANCE" in apply_shader and
            "DebugHDRIrradianceColor(preliminaryIrradiance + colorBleed)" not in apply_shader and
            "imageStore(uOutIndirect, pixel, vec4(irradiance, confidence));" in apply_shader,
            "Raw Irradiance debug must preserve the same linear HDR texture contract used by production surfel GI")
    require("uToneMap" in debug_present_shader and "DebugTonemap" in debug_present_shader and
            "uToneMap" in pipeline,
            "Raw Irradiance debug must tonemap only in the presentation shader, never by contaminating the production indirect texture")
    require("const bool surfelValidationActive = m_surfelGIManager &&" in modular and
            "placementValidationMode" in modular and
            "m_surfelGIManager->GetSettings().enabled &&\n\t\t\t\t!surfelValidationActive" in modular,
            "Placement Validation Mode must not composite low-confidence spawn hints as final surfel GI")
    require("vec3 ComputeSurfelGIResponse" in deferred_shader and
            "sourceIndex == 2" in deferred_shader and
            "ComputeSurfelGIResponse(indirectIrradiance, indirectAO, N, V, surface, diffuseAO, specularAO)" in deferred_shader and
            "receiverChromaResponseAlbedo" in deferred_shader and
            "chromaPreservedResponse" in deferred_shader and
            "maxChromaPreservedLum" in deferred_shader and
            "return CompressIndirectContribution(indirectIrradiance * strength);" not in deferred_shader,
            "Surfel GI must use a dedicated final response so filtered colour bleed is not crushed by the legacy SSGI/LPV BRDF path")
    require("SurfelOutgoingDiffuseRadiance" not in integrate_shader and
            "vec3 sampleMean = batchSum / max(acceptedWeight, 1e-5);" in integrate_shader and
            "vec3 sampleSecondMoment = batchSumSq / max(acceptedWeight, 1e-5);" in integrate_shader,
            "surfel ray integration must cache incident irradiance; outgoing diffuse radiance is only for ray-hit shading")
    source_direct_body = trace_shader.split("vec3 EvaluateSourceDirectBounce")[1].split("float TraceSurfaceCacheWeight")[0]
    require("sourceAlbedo * SURFEL_INV_PI * directIrradiance" not in source_direct_body and
            "vec3 sourceDirectIrradiance = directIrradiance * SURFEL_SOURCE_DIRECT_BOUNCE_SCALE;" in source_direct_body,
            "source-direct injection must store incident irradiance; source albedo/PI is applied later when the surfel emits bounce")
    require("cachedBounce = albedo * SURFEL_INV_PI * cachedIrradiance" in trace_shader,
            "surfel-grid ray hits must convert cached irradiance to outgoing radiance with the hit surfel albedo exactly once")
    require("skyboxExposure * 0.75f" not in pipeline and
            "kSurfelGISkyDiffuseMaxScale" in pipeline and
            "context.iblIntensity * context.diffuseIBLScale" in pipeline,
            "surfel GI must not use skybox display exposure as a high constant indirect light floor")
    require("SURFEL_SKY_OPEN_MISS_VISIBILITY" in trace_shader and
            "return hemisphereAccess;" not in trace_shader,
            "sky visibility misses must be conservative so open-sky fallback cannot wash out occluded/local surfel irradiance")
    require(0.32 <= glsl_float_constant(apply_shader, "SURFEL_CROSS_SURFACE_MAX_LIFT") <= 0.42 and
            1.10 <= glsl_float_constant(apply_shader, "SURFEL_CROSS_SURFACE_BLEED_BOOST") <= 1.45 and
            1.45 <= glsl_float_constant(apply_shader, "SURFEL_CROSS_SURFACE_CHROMA_GAIN") <= 1.80 and
            "supportLum" in apply_shader and
            "geometricSupport" in apply_shader and
            "maxAddedBleedLum" in apply_shader,
            "cross-surface colour bleed must be visible before deferred shading while staying bounded by support")
    require("SURFEL_APPLY_BLEED_EARLY_WEIGHT" in apply_shader and
            "directGatherSaturated && bleedGatherSaturated" in apply_shader and
            "if (sumWeight >= SURFEL_APPLY_EARLY_WEIGHT) {\n            break;" not in apply_shader,
            "receiver gather must not stop scanning neighbor cells just because same-surface support is saturated; that starves color bounce")
    response_gain = glsl_float_constant(deferred_shader, "SURFEL_GI_RESPONSE_FORM_FACTOR_COMPENSATION")
    require(16.0 <= response_gain <= 22.0,
            "surfel GI response must compensate for normalized realtime gather form factors without relying on the global GI intensity slider")
    source_direct_body = trace_shader.split("void main()")[1]
    require("sourceDirectEstimatorScale" not in source_direct_body and
            "encodedSourceBounce" not in source_direct_body and
            "gather.radiance = sourceDirectBounce;" in source_direct_body and
            "sourceDirectHit ? SurfelClampRadiance(hit.radiance_pdf.rgb)" in integrate_shader,
            "source-direct bootstrap must be a deterministic irradiance sample, not a ray-count-scaled radiance contribution")
    source_direct_scale = glsl_float_constant(trace_shader, "SURFEL_SOURCE_DIRECT_BOUNCE_SCALE")
    require(1.0 <= source_direct_scale <= 2.5,
            "source direct bounce scale must be visible but bounded so bright direct surfels do not become white blotches")


if __name__ == "__main__":
    main()
