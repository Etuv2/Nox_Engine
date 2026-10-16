from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    # Plan section 14: sky light has one estimator. Hits with no cached irradiance continue their path, and the sky
    # enters only through the miss rays (EvaluateMissEnvironmentRadiance). The old sky-visibility term at hits, which
    # was a second estimate of the same light, is gone.
    trace = read("shaders/surfel_gi/trace_rays.comp")
    shared = read("shaders/surfel_gi/surfel_scene_trace.glsl")

    for removed in ("TraceSkyVisibilityDirection", "TraceSkyVisibilityAtSurface", "EvaluateHitEnvironmentIrradiance",
                    "SkyOcclusionVisibilityFromHitDistance", "DiffuseSkyHemisphereAccess"):
        require(removed not in trace and removed not in shared,
                f"{removed} is a second sky estimate and must not come back (plan section 14)")
    require("SurfelPathRadiance(" in trace and "ShadeHitRadiance(" in trace,
            "hits must shade through the shared path, which continues where the cache is empty")
    require("return radiance + throughput * EvaluateMissEnvironmentRadiance(direction);" in shared,
            "the sky must enter the path only at a miss, weighted by the path throughput")


if __name__ == "__main__":
    main()
