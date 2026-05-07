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
    spawn = read("shaders/surfel_gi/spawn.comp")
    modular = read("src/ModularRenderer.cpp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")

    require("SURFEL_TRACE_SCREEN_RELIABILITY" in trace and
            "gather.weight = SURFEL_TRACE_SCREEN_RELIABILITY" in trace,
            "screen-space trace hits must enter the temporal cache as low-reliability samples")
    require("TraceSoftwareBVHRadiance(ray, gather)" in trace and
            "TraceScreenSpace(ray, gather)" in trace and
            trace.find("TraceScreenSpace(ray, gather)") < trace.find("TraceSoftwareBVHRadiance(ray, gather)"),
            "screen-space hits should be tried first when explicitly enabled, with BVH still ahead of surfel fallback")
    require("HitKindReliability" in integrate and
            "TemporalClampIrradiance" in integrate and
            "SURFEL_MAX_TEMPORAL_LIFT_PER_UPDATE" in integrate,
            "surfel temporal integration must reliability-weight hit kinds and clamp per-update luminance jumps")
    require("mix(surfel.irradiance.rgb, vec3(0.0), 0.045)" not in integrate,
            "missed ray batches must not visibly decay cached irradiance in one or two frames")
    require("initialDirectScale = 0.04" in spawn and
            "initialEnvironmentEstimate = max(uSkyRadiance, vec3(0.0)) * initialSkyVisibility * 0.03" in spawn and
            "SurfelLuminance(initialIrradiance) > 0.0 ? 0.04 : 0.02" in spawn,
            "new surfels may only receive a low-confidence seed; real bounce must come from ray integration")
    require("allowScreenTrace" in modular and
            "settings.useScreenSpaceTrace =" in modular and
            "settings.qualityTier == SurfelGIQualityTier::Ultra" in modular and
            "caps.allowScreenTrace" in modular,
            "screen-space trace must stay tier-gated and only become default in Ultra/capture-style quality")
    require("ReadSurfelCounters" in pipeline and
            "m_frameIndex % kSurfelStatsReadbackInterval" in pipeline,
            "surfel UI counters must be refreshed periodically even when the debug overlay is off")


if __name__ == "__main__":
    main()
