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
            trace.find("TraceSoftwareBVHRadiance(ray, gather)") < trace.find("TraceScreenSpace(ray, gather)"),
            "stable BVH/surfel evidence must be preferred over view-dependent screen-space hits")
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
            "settings.useScreenSpaceTrace = settings.useScreenSpaceTrace && caps.allowScreenTrace" in modular,
            "screen-space trace must be disabled by the real-time preset unless a capture/diagnostic preset explicitly allows it")
    require("ReadSurfelCounters" in pipeline and
            "m_frameIndex % kSurfelStatsReadbackInterval" in pipeline,
            "surfel UI counters must be refreshed periodically even when the debug overlay is off")


if __name__ == "__main__":
    main()
