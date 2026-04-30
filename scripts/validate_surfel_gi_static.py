from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_exists(relpath: str) -> None:
    require((ROOT / relpath).exists(), f"required file is missing: {relpath}")


def require_absent(relpath: str) -> None:
    require(not (ROOT / relpath).exists(), f"deprecated file must be removed: {relpath}")


def main() -> None:
    legacy_files = [
        "src/passes/SurfelGIPass.h",
        "src/passes/SurfelGIPass.cpp",
        "src/passes/SurfelIndirectDiffusePass.h",
        "src/passes/SurfelIndirectDiffusePass.cpp",
        "shaders/includes/surfel_gi_common.glsl",
        "shaders/includes/surfel_gi_projection_support.glsl",
        "shaders/includes/surfel_gi_visibility_compat.glsl",
        "shaders/surfel_indirect_diffuse_gather_comp.glsl",
        "shaders/surfel_gi_spawn_comp.glsl",
        "shaders/surfel_gi_ray_trace_comp.glsl",
        "shaders/surfel_gi_debug_frag.glsl",
        "shaders/surfel_gi_debug_vert.glsl",
    ]
    for relpath in legacy_files:
        require_absent(relpath)

    replacement_files = [
        "src/passes/SurfelGIManager.h",
        "src/passes/SurfelGIManager.cpp",
        "src/passes/SurfelGIPipeline.h",
        "src/passes/SurfelGIPipeline.cpp",
        "src/surfel_gi/SurfelGIResources.h",
        "src/surfel_gi/SurfelGIResources.cpp",
        "shaders/includes/surfel_gi_resources.glsl",
        "shaders/surfel_gi/common.glsl",
        "shaders/surfel_gi/begin_frame.comp",
        "shaders/surfel_gi/apply_indirect.comp",
        "shaders/surfel_gi/debug_surfels.vert",
        "shaders/surfel_gi/debug_surfels.frag",
        "shaders/surfel_gi/update_surfels.comp",
        "shaders/surfel_gi/recycle_surfels.comp",
        "shaders/surfel_gi/clear_grid.comp",
        "shaders/surfel_gi/build_grid.comp",
        "shaders/surfel_gi/cell_average.comp",
        "shaders/surfel_gi/spawn.comp",
        "shaders/surfel_gi/request_rays.comp",
        "shaders/surfel_gi/allocate_rays.comp",
        "shaders/surfel_gi/generate_rays.comp",
        "shaders/surfel_gi/trace_rays.comp",
        "shaders/surfel_gi/integrate.comp",
        "shaders/surfel_gi/radial_depth_update.comp",
    ]
    for relpath in replacement_files:
        require_exists(relpath)

    manager_h = read("src/passes/SurfelGIManager.h")
    render_context_h = read("src/RenderContext.h")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")
    modular_cpp = read("src/ModularRenderer.cpp")
    lighting_h = read("src/passes/LightingPass.h")
    lighting_cpp = read("src/passes/LightingPass.cpp")
    deferred = read("shaders/deferred_lighting_frag.glsl")
    project = read("Nox_Engine.vcxproj")
    filters = read("Nox_Engine.vcxproj.filters")
    common = read("shaders/surfel_gi/common.glsl")
    spawn = read("shaders/surfel_gi/spawn.comp")
    apply_indirect = read("shaders/surfel_gi/apply_indirect.comp")
    trace_rays = read("shaders/surfel_gi/trace_rays.comp")

    require("bool enabled = false" in manager_h, "clean Surfel GI must remain disabled by default")
    require("std::unique_ptr<SurfelGIManager> m_surfelGIManager" in read("src/ModularRenderer.h"),
            "ModularRenderer must own the clean SurfelGIManager")
    require("SurfelGIUpdatePass" in modular_cpp and '"GBuffer", "TransformHistory"' in modular_cpp,
            "clean Surfel GI must be staged after G-buffer and transform history")
    require("QuarantineDeprecatedSurfelGI" in modular_cpp,
            "deprecated RenderContext surfel controls must stay quarantined")

    forbidden_legacy_tokens = [
        "SetSurfelIndirectDiffuseTexture",
        "surfelIndirectDiffuseMap",
        "uEnableSurfelGI",
        "uSurfelGIStrength",
        "uUseLegacySurfelFragmentGather",
        "EvaluatePersistentSurfelGI",
        "includes/surfel_gi_common.glsl",
    ]
    for token in forbidden_legacy_tokens:
        require(token not in lighting_h, f"legacy lighting hook remains in LightingPass.h: {token}")
        require(token not in lighting_cpp, f"legacy lighting hook remains in LightingPass.cpp: {token}")
        require(token not in deferred, f"legacy lighting shader hook remains: {token}")

    for token in ("SurfelGIPass", "SurfelIndirectDiffusePass", "surfel_indirect_diffuse_gather_comp"):
        require(token not in project, f"VS project still references deprecated token: {token}")
        require(token not in filters, f"VS filters still reference deprecated token: {token}")
    for token in ("SurfelGIManager.cpp", "SurfelGIPipeline.cpp", "src\\surfel_gi\\SurfelGIResources.cpp"):
        require(token in project, f"VS project must include replacement file: {token}")

    require("transform_tracking_contract.glsl" in common,
            "clean surfel shaders must consume the engine transform contract")
    for binding in ("#define B_SURFELS 10", "#define B_SURFEL_FREELIST 11", "#define B_TRANSFORMS 22"):
        require(binding in common, f"clean surfel binding contract missing {binding}")
    require("PopFreeIndex" in spawn and "ReconstructWorldPosition" in spawn and "GpuTransformRecord" in spawn,
            "spawn shader must allocate persistent surfels from G-buffer and transform IDs")
    require("uSpawnPassCount" in spawn and "DeterministicTilePixel" in spawn,
            "spawn shader must use bounded deterministic per-tile pixel sweeps")
    require(("ReceiverSurfelGatherWeight" in apply_indirect or "SurfelCoverageWeight" in apply_indirect) and
            "imageStore(uOutIndirect" in apply_indirect,
            "apply shader must gather surfels into a final indirect texture")
    require("request_rays.comp" in pipeline_cpp and "trace_rays.comp" in pipeline_cpp and "integrate.comp" in pipeline_cpp,
            "pipeline must wire adaptive ray request, tracing, and temporal integration passes")
    require("SurfelGIIndirect" in modular_cpp and "SetIndirectDiffuseSource" in modular_cpp,
            "renderer must feed clean surfel indirect output through the generic lighting path")
    require("NOX_DISABLE_SURFEL_GI" in modular_cpp,
            "runtime validation must be able to force clean Surfel GI off despite saved UI state")
    require("RenderDebug" in pipeline_cpp and "debug_surfels.vert" in pipeline_cpp,
            "clean surfel pipeline must expose a debug draw path for spawned surfels")

    require("glGetBufferSubData" not in pipeline_cpp,
            "clean surfel pipeline must not use blocking counter readbacks")
    require("EnableTiming(" not in pipeline_cpp,
            "clean surfel pipeline must not use blocking ComputeShader timing")
    require("IndirectDiffusePass" in modular_cpp and "indirect_diffuse_gather_comp.glsl" in read("src/passes/IndirectDiffusePass.cpp"),
            "SSGI comparison path must remain wired")

    realtime_budget_tokens = [
        "cleanSurfelGIMaxSurfels = 131072",
        "cleanSurfelGIMaxRayBudget = 32768",
        "cleanSurfelGIMaxSurfelsPerCell = 64",
        "cleanSurfelGIMaxGatherSurfelsPerPixel = 512",
        "cleanSurfelGISpawnTileSize = 8",
    ]
    for token in realtime_budget_tokens:
        require(token in render_context_h, f"real-time clean Surfel GI default missing from RenderContext.h: {token}")

    manager_budget_tokens = [
        "uint32_t maxSurfels = 131072u",
        "uint32_t maxRayBudget = 32768u",
        "uint32_t spawnTileSize = 8u",
        "uint32_t maxSurfelsPerCell = 64u",
        "uint32_t maxGatherSurfelsPerPixel = 512u",
    ]
    for token in manager_budget_tokens:
        require(token in manager_h, f"real-time clean Surfel GI default missing from SurfelGIManager.h: {token}")

    trace_steps = re.search(r"const\s+uint\s+SURFEL_TRACE_STEPS\s*=\s*(\d+)u\s*;", trace_rays)
    require(trace_steps is not None and int(trace_steps.group(1)) >= 8,
            "correctness surfel trace must use enough steps to hit coloured bounce sources")
    require("const uint SURFEL_TRACE_NEIGHBOR_RADIUS = 1u" in trace_rays,
            "correctness surfel trace must scan neighboring cells")
    trace_cell_entries = re.search(r"const\s+uint\s+SURFEL_TRACE_MAX_CELL_ENTRIES\s*=\s*(\d+)u\s*;", trace_rays)
    require(trace_cell_entries is not None and int(trace_cell_entries.group(1)) >= 8,
            "correctness surfel trace must inspect enough entries per cell")
    require("uniform uint uGatherNeighborRadius" in apply_indirect,
            "final gather must expose a bounded neighbor radius uniform")
    require("Spawned surfels must be visible to same-frame ray tracing" in pipeline_cpp,
            "Surfel GI must rebuild the grid after spawning so final gather can see new surfels")
    require("ComputeSpawnPassCount" in pipeline_cpp and "stationaryFastFillFrames" in pipeline_cpp,
            "Surfel GI must expose bounded fast-fill spawn scheduling")


if __name__ == "__main__":
    main()
