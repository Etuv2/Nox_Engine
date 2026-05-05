from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    trace = read("shaders/surfel_gi/trace_rays.comp")
    integrate = read("shaders/surfel_gi/integrate.comp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")

    require("uniform samplerCube uSkyEnvironmentMap;" in trace and
            "uniform int uUseSkyEnvironmentMap" in trace,
            "trace shader must expose a directional sky environment map for ray misses")
    require("EvaluateMissEnvironmentRadiance" in trace and
            "textureLod(uSkyEnvironmentMap, rayDir, SURFEL_ENVIRONMENT_MISS_MIP).rgb" in trace,
            "ray misses must sample directional low-frequency environment radiance")
    require("SURFEL_ENVIRONMENT_MISS_MIP" in trace and
            "SURFEL_ENVIRONMENT_MISS_MAX_RADIANCE" in trace and
            "SURFEL_ENVIRONMENT_MISS_SCALE" in trace and
            "skyRadiance *= SURFEL_ENVIRONMENT_MISS_MAX_RADIANCE" in trace,
            "environment misses must be low-pass filtered and luminance-clamped before temporal integration")
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
