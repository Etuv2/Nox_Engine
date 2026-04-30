from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def uint_constant(source: str, name: str) -> int:
    match = re.search(rf"const\s+uint\s+{re.escape(name)}\s*=\s*(\d+)u\s*;", source)
    require(match is not None, f"missing GLSL uint constant: {name}")
    return int(match.group(1))


def main() -> None:
    apply_shader = read("shaders/surfel_gi/apply_indirect.comp")
    trace_shader = read("shaders/surfel_gi/trace_rays.comp")
    spawn_shader = read("shaders/surfel_gi/spawn.comp")
    recycle_shader = read("shaders/surfel_gi/recycle_surfels.comp")
    debug_vert = read("shaders/surfel_gi/debug_surfels.vert")
    debug_frag = read("shaders/surfel_gi/debug_surfels.frag")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")
    manager_h = read("src/passes/SurfelGIManager.h")
    ui_cpp = read("src/ImGui/RenderingSettingsWindow.cpp")

    require("SampleTrilinearGridIrradiance" in apply_shader,
            "final gather must keep a trilinear grid fallback for cross-cell blending")
    require("sumWeight <= 1e-5) {\n        imageStore(uOutIndirect, pixel, vec4(0.0));" not in apply_shader,
            "final gather must not return black before trying filtered/grid fallback irradiance")
    require("mix(gridIrradiance, directIrradiance, directBlend)" in apply_shader,
            "final gather must blend individual surfels with grid fallback instead of switching hard")
    require("SURFEL_GRID_FALLBACK_KERNEL_RADIUS" in apply_shader,
            "grid fallback must sample a smooth cross-cell kernel, not a hard per-cell value")
    require("fallbackConfidence > 1e-5 ? directCoverage * 0.45 : directCoverage" in apply_shader,
            "local surfel gather must be damped when smooth grid fallback is available")
    require("DEBUG_FINAL_GATHER_WEIGHT" in apply_shader,
            "apply pass must expose a per-pixel final gather weight debug view")
    for token in ("DEBUG_GBUFFER_WORLD_POSITION", "DEBUG_GBUFFER_NORMAL",
                  "DEBUG_GBUFFER_TRANSFORM_ID", "DEBUG_GBUFFER_MATERIAL_ID",
                  "DEBUG_SPAWN_CANDIDATES"):
        require(token in apply_shader, f"apply pass must expose {token} debug view")
    require("SetUniform1ui(program, \"uDebugView\", applyDebugView)" in pipeline_cpp,
            "pipeline must route fullscreen surfel debug views into the apply shader")
    require("SetUniform1f(program, \"uFallbackStrength\", 1.0f)" in pipeline_cpp,
            "pipeline must keep grid fallback enabled for correctness validation")

    require(uint_constant(trace_shader, "SURFEL_TRACE_MAX_CELL_ENTRIES") >= 32,
            "ray tracing must inspect enough cell entries to avoid arbitrary first-entry bias")
    require(uint_constant(trace_shader, "SURFEL_TRACE_MAX_CANDIDATES") >= 192,
            "ray tracing must use enough candidates for stable colour-bleed hits")

    require("RayHitRadiance" in manager_h and "GatherWeights" in manager_h and "SpawnCandidates" in manager_h,
            "SurfelGI debug enum must expose ray-hit radiance and gather-weight diagnostics")
    require("Ray Hit Radiance" in ui_cpp and "Gather Weights" in ui_cpp and "GBuffer Transform ID" in ui_cpp,
            "rendering settings UI must expose the new surfel GI correctness debug views")

    require("validDisk" in debug_frag and "diskR2" in debug_frag,
            "surfel debug draw must render projected surfel disks, not screen-facing sprite blobs")
    require("uSceneDepthTex" in debug_frag and "vClipDepth01" in debug_vert,
            "surfel debug draw must depth-reject against the scene depth so hidden surfels do not appear detached")
    require("context.gbufferFBO->GetDepthTexture()" in pipeline_cpp and
            "SetUniform1i(m_debugProgram, \"uSceneDepthTex\"" in pipeline_cpp,
            "surfel debug pass must bind G-buffer depth for trustworthy surface attachment validation")
    require("ComputeCameraCenteredGridBounds" in pipeline_cpp and "const glm::vec3 gridMin = -halfExtent" not in pipeline_cpp,
            "active surfel grid must be camera-centered, not fixed at world origin")

    require("if (!SurfelWorldToGridCell(worldPos, uGridMin, uGridMax, uGridResolution, baseCell))" in spawn_shader,
            "spawn shader must reject G-buffer samples outside the active surfel grid")
    require("sampleIndex = SurfelHash(seed)" in spawn_shader and "sweepIndex" not in spawn_shader,
            "spawn candidate selection must be hashed per frame/tile instead of a visible scanline sweep")
    require("surfel.ids = uvec4(transformID, 0u, materialID" in spawn_shader,
            "surfel ids must store transform/entity/material in stable slots")
    require("SurfelMaterialAllowsDiffuseGI(surfel.ids.z)" in apply_shader,
            "final gather material rejection must read material ID from surfel.ids.z")
    require("outsideGrid" in recycle_shader and "uGridMin" in recycle_shader and "uGridMax" in recycle_shader,
            "recycling must remove surfels that leave the active camera-centered grid")


if __name__ == "__main__":
    main()
