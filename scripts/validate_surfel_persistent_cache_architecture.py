from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    manager = read("src/passes/SurfelGIManager.h")
    modular = read("src/ModularRenderer.cpp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    resources_h = read("src/surfel_gi/SurfelGIResources.h")
    common = read("shaders/surfel_gi/common.glsl")
    coverage = read("shaders/surfel_gi/coverage.comp")
    spawn = read("shaders/surfel_gi/spawn.comp")
    recycle = read("shaders/surfel_gi/recycle_surfels.comp")

    for token in (
        "uint32_t maxSpawnsPerFrame",
        "uint32_t maxProjectedSurfelsPerFrame",
        "uint32_t maxRecycleCountPerFrame",
        "float coverageNormalCos",
        "float coverageDepthTolerance",
    ):
        require(token in manager, f"SurfelGISettings must expose bounded cache maintenance setting: {token}")

    for token in (
        "SurfelGIMaxSpawnsPerFrame",
        "SurfelGIMaxProjectedSurfelsPerFrame",
        "SurfelGIMaxRecycleCountPerFrame",
    ):
        require(token in read("src/RenderContext.h"), f"RenderContext must expose Surfel GI budget knob: {token}")
        require(token in modular, f"ModularRenderer must propagate Surfel GI budget knob: {token}")

    require("projectedSurfels" in resources_h and "coverageDepthRejected" in resources_h and
            "coverageNormalRejected" in resources_h and "coverageMaterialRejected" in resources_h and
            "coverageRadiusRejected" in resources_h,
            "SurfelCounters must report projection coverage accept/reject diagnostics")
    require("projectedSurfels" in common and "coverageDepthRejected" in common and
            "coverageNormalRejected" in common and "coverageMaterialRejected" in common and
            "coverageRadiusRejected" in common,
            "GLSL SurfelCounters must match C++ projection coverage diagnostics")

    require("SurfelCoverageTile" in resources_h and "glm::uvec4 projection" in resources_h,
            "C++ coverage tiles must store integer projection diagnostics for atomic updates")
    require("struct SurfelCoverageTile" in common and "uvec4 projection" in common,
            "GLSL coverage tiles must store integer projection diagnostics for atomic updates")

    require("ValidateProjectedSurfelCoverage" in coverage,
            "coverage pass must project persistent surfels back into screen-space coverage tiles")
    for token in (
        "radiusCompatible",
        "normalCompatible",
        "depthConsistent",
        "materialCompatible",
        "atomicAdd(coverageTiles[tileIndex].projection.x",
        "atomicAdd(counters.projectedSurfels",
    ):
        require(token in coverage, f"coverage projection must include {token}")

    require('"coverage_reset"' in pipeline and '"coverage_project"' in pipeline,
            "SurfelGIPipeline must run coverage reset and projection as dedicated compute subpasses")
    require('SetUniform1ui(program, "uMaxProjectedSurfelsPerFrame"' in pipeline,
            "pipeline must pass bounded projected surfel budget to coverage.comp")
    require('SetUniform1ui(program, "uMaxSpawnsPerFrame"' in pipeline,
            "pipeline must pass bounded spawn budget to spawn.comp")
    require('SetUniform1ui(program, "uMaxRecycleCount"' in pipeline,
            "pipeline must pass bounded recycle budget to recycle_surfels.comp")
    require('SetUniform1ui(program, "uBypassCoverageSearch", 0u)' in pipeline,
            "coverage search must not be bypassed during fast fill")

    require("uMaxSpawnsPerFrame" in spawn and "ReserveSpawnBudget" in spawn,
            "spawn.comp must reserve a bounded per-frame spawn budget before free-list allocation")
    require("tileState.projection" not in spawn and
            "HasCompatibleDuplicateSurfel" not in spawn,
            "spawn.comp must rely on reference coverage/removal thresholds instead of projected duplicate allocation heuristics")
    require("coverage <= uPlacementThreshold" in spawn and
            "coverage > uRemovalThreshold" in spawn,
            "spawn.comp must use reference placement/removal thresholds for duplicate control")

    require("uMaxRecycleCount" in recycle and "ReserveRecycleBudget" in recycle,
            "recycle_surfels.comp must enforce a strict per-frame recycle budget")


if __name__ == "__main__":
    main()
