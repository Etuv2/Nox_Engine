from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    trace = read("shaders/surfel_gi/trace_rays.comp")
    integrate = read("shaders/surfel_gi/integrate.comp")
    apply = read("shaders/surfel_gi/apply_indirect.comp")
    upscale = read("shaders/surfel_gi/upscale_filter.comp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")

    source_direct_scale = re.search(r"SURFEL_SOURCE_DIRECT_BOUNCE_SCALE\s*=\s*([0-9.]+)", trace)
    require(source_direct_scale is not None and float(source_direct_scale.group(1)) >= 3.5,
            "directly lit surfels must emit a visible first-bounce term")
    require("sourceAlbedo * SURFEL_INV_PI * directIrradiance * SURFEL_SOURCE_DIRECT_BOUNCE_SCALE" not in trace and
            "vec3 sourceDirectIrradiance = directIrradiance * SURFEL_SOURCE_DIRECT_BOUNCE_SCALE;" in trace,
            "source-direct bootstrap must store incident irradiance; BRDF conversion happens when surfels emit bounce")
    require("SURFEL_ENVIRONMENT_MISS_SCALE = 0.16" in trace and
            "SURFEL_ENVIRONMENT_MISS_MAX_RADIANCE = 1.6" in trace,
            "ray misses must use conservative directional sky, not a broad ambient lift")
    require("SURFEL_MAX_SAMPLE_IRRADIANCE = 32.0" in integrate and
            "SurfelMSME(estimatorSample, msmeData, shortWindowBlend)" in integrate and
            "TemporalClampIrradianceWithConfidence" not in integrate,
            "surfel irradiance integration must allow real local bounce to rise through MSME instead of clamping toward zero")
    require("vec3 irradiance = ComposeColorBleedWithIrradiance(preliminaryIrradiance, colorBleed, bleedSupport);" in apply and
            "preliminaryIrradiance * 0.72" not in apply and
            "ComposeColorBleedWithIrradiance" in apply,
            "final gather must preserve raw surfel irradiance while adding supported colour bleed before deferred material response")
    require("if (uDebugView != 0u)" in upscale and
            "textureLod(uLowResIndirectTex, fullUV, 0.0)" in upscale,
            "fullscreen surfel debug views must bypass irradiance upsample filtering")
    require("std::clamp(skyboxBounceScale, 0.0f, kSurfelGISkyDiffuseMaxScale)" in pipeline,
            "surfel trace sky contribution must be capped separately from debug/composite intensity")


if __name__ == "__main__":
    main()
