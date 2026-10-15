from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    trace = read("shaders/surfel_gi/trace_rays.comp")
    shared = read("shaders/surfel_gi/surfel_scene_trace.glsl")
    integrate = read("shaders/surfel_gi/integrate.comp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")

    require("uniform samplerCube uSkyEnvironmentMap;" in shared and
            "uniform int uUseSkyEnvironmentMap" in shared,
            "shared trace code must expose a directional sky environment map for ray misses")
    require("EvaluateMissEnvironmentRadiance" in shared and
            "textureLod(uSkyEnvironmentMap, rayDir, SURFEL_ENVIRONMENT_MISS_MIP).rgb" in shared and
            "SURFEL_TRACE_BVH_FAR_DISTANCE" in shared,
            "ray misses must sample directional environment radiance")
    # The prefiltered cube has five levels (0-4). Mip 5 does not exist and read black, so the sky was zero (plan 13).
    require("const float SURFEL_ENVIRONMENT_MISS_MIP = 0.0;" in shared,
            "environment misses must read the base level of the environment cube (the prefiltered cube has 5 mips)")
    require("SURFEL_ENVIRONMENT_MISS_MAX_RADIANCE" not in shared,
            "environment misses must not be clamped: a sky miss is a physical sample")
    require("SURFEL_TRACE_HIT_KIND_ENVIRONMENT" in trace and
            "gather.hitKind = SURFEL_TRACE_HIT_KIND_ENVIRONMENT;" in trace,
            "environment misses must be marked as valid environment samples")
    require("SURFEL_TRACE_HIT_KIND_ENVIRONMENT" in integrate and
            "return 1.0;" in integrate,
            "temporal integration must accept environment samples with explicit reliability")
    require("kSurfelGISkyEnvironmentUnit" in pipeline and
            "skybox->GetEnvironmentMap()" in pipeline and
            "uSkyEnvironmentMap" in pipeline,
            "pipeline must bind the skybox environment cubemap to the surfel trace pass")
    require("uSkyRadiance * max(rayDir.y, 0.0)" not in trace,
            "environment misses must not use the old flat horizon-weighted ambient expression")


if __name__ == "__main__":
    main()
