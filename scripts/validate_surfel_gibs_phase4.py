from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    common = read("shaders/surfel_gi/common.glsl")
    build_grid = read("shaders/surfel_gi/build_grid.comp")
    spawn = read("shaders/surfel_gi/spawn.comp")
    update = read("shaders/surfel_gi/update_surfels.comp")
    recycle = read("shaders/surfel_gi/recycle_surfels.comp")
    apply = read("shaders/surfel_gi/apply_indirect.comp")
    trace = read("shaders/surfel_gi/trace_rays.comp")
    debug_vert = read("shaders/surfel_gi/debug_surfels.vert")
    manager_h = read("src/passes/SurfelGIManager.h")
    pipeline_h = read("src/passes/SurfelGIPipeline.h")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")
    resources_h = read("src/surfel_gi/SurfelGIResources.h")
    resources_glsl = read("shaders/includes/surfel_gi_resources.glsl")
    ui_cpp = read("src/ImGui/RenderingSettingsWindow.cpp")

    for source_name, source in (
        ("common.glsl", common),
        ("build_grid.comp", build_grid),
        ("spawn.comp", spawn),
        ("apply_indirect.comp", apply),
        ("trace_rays.comp", trace),
        ("debug_surfels.vert", debug_vert),
    ):
        require("SurfelWorldToGridAddress" in source,
                f"{source_name} must use the shared non-linear grid address path")

    require("SurfelNonLinearGridCellCount" in common and
            "SurfelAxisRegionIndex" in common and
            "SurfelGridCellDebugBounds" in common,
            "common.glsl must implement central plus six axis non-linear grid mapping and debug bounds")
    require("centralResolution" in resources_h and
            "axisSliceCount" in resources_h and
            "axisLateralResolution" in resources_h and
            "CellCount() const" in resources_h,
            "SurfelGridSettings must size the non-linear grid separately from the old uniform grid")
    require("gridSettings.centralResolution" in pipeline_cpp and
            "SetUniform1ui(program, \"uUseNonLinearGrid\"" in pipeline_cpp and
            "SetUniform1f(program, \"uGridFarExtent\"" in pipeline_cpp,
            "pipeline must pass non-linear grid controls to every grid consumer")

    require("SurfelCoverageTile" in resources_h and
            "coverageTileBuffer" in pipeline_h and
            "B_COVERAGE_TILES" in resources_glsl and
            "layout(binding = B_COVERAGE_TILES" in spawn,
            "coverage scheduler must store persistent per-screen-tile state in an SSBO")
    for token in (
        "lowestCoverage",
        "lastSpawnFrame",
        "unresolvedAge",
        "SURFEL_COVERAGE_TILE_HIGH_PRIORITY",
        "uCameraMotionBoost",
        "underCoveredTileCount",
        "coverageSpawnedTileCount",
    ):
        require(token in spawn or token in resources_h or token in resources_glsl or token in pipeline_cpp,
                f"coverage scheduler token missing: {token}")
    require("cameraMoving ? fastPasses : steadyPasses" in pipeline_cpp or
            "cameraMotionBoost" in pipeline_cpp,
            "fast camera movement must boost coverage/spawn scheduling")

    require("uTargetSurfelScreenRadiusPx * WorldUnitsPerPixel" in update and
            "surfel.localPos_spawnRadius.w = mix(" in update and
            "targetRadiusError" in update,
            "surfel update must shrink/grow radius from screen-space projection and record target error")
    require("RadiusError" in manager_h and "GridAxisRegion" in manager_h and
            "GridOverflow" in manager_h and "IrradianceConfidence" in manager_h,
            "debug enum/UI must expose radius error, axis grid, overflow, and confidence diagnostics")
    require("Radius Error" in ui_cpp and "Grid Axis Region" in ui_cpp and
            "Grid Overflow" in ui_cpp and "Irradiance Confidence" in ui_cpp,
            "rendering settings UI must expose the new GIBS debug views")

    require("overCoverageRecycled" in resources_h and
            "staleRecycled" in resources_h and
            "pressureRecycled" in resources_h and
            "underCoveredTileCount" in resources_h,
            "counters must include coverage and recycling breakdowns")
    require("overCoverageCull" in recycle and
            "atomicAdd(counters.overCoverageRecycled" in recycle and
            "SurfelWorldToGridAddress" in recycle,
            "recycling must use grid density/radius scale to remove over-covered surfels")

    require("initialCellIrradiance" in spawn and
            "initialDirectIrradiance" in spawn and
            "initialSkyVisibility" in spawn and
            "initialConfidence" in spawn and
            "uSkyRadiance" in spawn,
            "new surfels must bootstrap from local surfels/cell/direct/skylight estimates, not black or global ambient")
    require("skyMissRadianceMultiplier = 1.0f" in manager_h,
            "sky/environment ray misses must be enabled by default so no-BVH large scenes can converge from physical sky misses")
    require("framesSinceVisible" in read("shaders/surfel_gi/request_rays.comp") and
            "SURFEL_REQUEST_VISIBLE_UNRESOLVED_RAYS" in read("shaders/surfel_gi/request_rays.comp"),
            "ray scheduling must prioritize visible unresolved surfels")
    require("SetUniform1f(program, \"uFallbackStrength\", m_settings.cellAverageFallbackStrength)" in pipeline_cpp and
            "cellAverageFallbackStrength" in manager_h and
            "SampleTrilinearGridIrradiance" in apply,
            "final gather must use a configurable confidence-weighted cell-average fallback")


if __name__ == "__main__":
    main()
