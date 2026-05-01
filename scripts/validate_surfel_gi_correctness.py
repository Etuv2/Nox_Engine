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


def glsl_uint_constant(source: str, name: str) -> int:
    match = re.search(rf"const\s+uint\s+{re.escape(name)}\s*=\s*(\d+)u\s*;", source)
    require(match is not None, f"missing GLSL uint constant: {name}")
    return int(match.group(1))


def main() -> None:
    apply_shader = read("shaders/surfel_gi/apply_indirect.comp")
    trace_shader = read("shaders/surfel_gi/trace_rays.comp")
    spawn_shader = read("shaders/surfel_gi/spawn.comp")
    recycle_shader = read("shaders/surfel_gi/recycle_surfels.comp")
    count_live_shader = read("shaders/surfel_gi/count_live_surfels.comp")
    common_shader = read("shaders/surfel_gi/common.glsl")
    debug_vert = read("shaders/surfel_gi/debug_surfels.vert")
    debug_frag = read("shaders/surfel_gi/debug_surfels.frag")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")
    modular_cpp = read("src/ModularRenderer.cpp")
    manager_h = read("src/passes/SurfelGIManager.h")
    resources_h = read("src/surfel_gi/SurfelGIResources.h")
    ui_cpp = read("src/ImGui/RenderingSettingsWindow.cpp")

    require("emitterFacing" in apply_shader and "receiverFacing" in apply_shader and "transferFacing" in apply_shader,
            "final gather must use transfer-facing weights so perpendicular colour bleeding is not rejected")
    require("coplanarFacing" in apply_shader,
            "final gather must still support same-surface smoothing without making it the only valid path")
    require("sumWeight <= 1e-5 && fallbackConfidence <= 1e-5" in apply_shader,
            "final gather must return black when neither real surfels nor an explicitly enabled fallback contribute")
    require("mix(gridIrradiance, directIrradiance, directBlend)" in apply_shader,
            "final gather must blend individual surfels with optional grid fallback instead of switching hard")
    require("SURFEL_GRID_FALLBACK_KERNEL_RADIUS" in apply_shader,
            "grid fallback must sample a smooth cross-cell kernel, not a hard per-cell value")
    require("float directBlend = directCoverage;" in apply_shader,
            "raw surfel gather must not be damped by fallback during the correctness baseline")
    require("SURFEL_GATHER_MIN_SUPPORT_FRACTION" in apply_shader and
            "SURFEL_GATHER_MIN_SUPPORT_WORLD" in apply_shader and
            "smoothstep(1.15, 0.45, normalizedDistance)" not in apply_shader,
            "final gather support must be wide and smooth enough that individual surfel disks are not visible")
    require("smoothstep(0.02, 0.30, sumWeight)" in apply_shader,
            "final gather confidence must accept overlapping sparse surfels instead of dropping most pixels to black")
    require("DEBUG_FINAL_GATHER_WEIGHT" in apply_shader,
            "apply pass must expose a per-pixel final gather weight debug view")
    for token in ("DEBUG_GBUFFER_WORLD_POSITION", "DEBUG_GBUFFER_NORMAL",
                  "DEBUG_GBUFFER_TRANSFORM_ID", "DEBUG_GBUFFER_MATERIAL_ID",
                  "DEBUG_GBUFFER_DEPTH", "DEBUG_SPAWN_CANDIDATES"):
        require(token in apply_shader, f"apply pass must expose {token} debug view")
    for token in ("StoredSurfelWorldPosition", "StoredSurfelRadius",
                  "StoredSurfelTransformID", "StoredSurfelFlags",
                  "DebugDrawPosition"):
        require(token in manager_h, f"SurfelGI debug enum must expose {token}")
    require("SetUniform1ui(program, \"uDebugView\", applyDebugView)" in pipeline_cpp,
            "pipeline must route fullscreen surfel debug views into the apply shader")
    require("m_settings.useIrradianceSharing ? 0.35f : 0.0f" in pipeline_cpp,
            "pipeline must keep cell-average fallback bounded and sourced only from real surfels")

    require(uint_constant(trace_shader, "SURFEL_TRACE_MAX_CELL_ENTRIES") >= 32,
            "ray tracing must inspect enough cell entries to avoid arbitrary first-entry bias")
    require(uint_constant(trace_shader, "SURFEL_TRACE_MAX_CANDIDATES") >= 192,
            "ray tracing must use enough candidates for stable colour-bleed hits")

    require("RayHitRadiance" in manager_h and "GatherWeights" in manager_h and "SpawnCandidates" in manager_h,
            "SurfelGI debug enum must expose ray-hit radiance and gather-weight diagnostics")
    require("Ray Hit Radiance" in ui_cpp and "Gather Weights" in ui_cpp and "GBuffer Transform ID" in ui_cpp,
            "rendering settings UI must expose the new surfel GI correctness debug views")

    require("glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4" in pipeline_cpp,
            "surfel debug draw must render instanced oriented disk quads, not point sprites")
    require("gl_InstanceID" in debug_vert and "SurfelBuildBasis" in debug_vert and "vDiskUV" in debug_vert,
            "surfel debug vertex shader must build disk geometry from world position, normal, and radius")
    require("GIBS_PAPER_DISK_SCALE" in debug_vert and "GIBSPaperColor" in debug_vert and "vStyle.w = paperStyle ? 1.0 : 0.0" in debug_vert,
            "primary live surfel debug mode must render EA GIBS-style large colored surface disks")
    require("if (vStyle.w > 0.5)" in debug_frag and "paperAlpha" in debug_frag and "centerDot" in debug_frag,
            "GIBS-style live surfel disks must use flat colored disks while diagnostic modes keep glyph rings")
    require("DEBUG_DISK_GRAZING_RADIUS_SCALE" in debug_vert and "smoothstep(0.12, 0.65, viewFacing)" in debug_vert,
            "primary surfel debug disks must clamp grazing-angle footprint so attached disks do not appear as screen-space streaks")
    require("gl_PointCoord" not in debug_frag and "spriteR2" not in debug_frag,
            "surfel debug fragment shader must not fall back to billboard point-sprite disks")
    require("surfels[surfelID] =" not in apply_shader and "readonly buffer SurfelBuffer" in apply_shader,
            "final gather must not race by writing surfel records from per-pixel apply invocations")
    require("uSceneDepthTex" in debug_frag and "gl_FragCoord.z" in debug_frag,
            "surfel debug draw must depth-reject against the scene depth so hidden surfels do not appear detached")
    require("context.gbufferFBO->GetDepthTexture()" in pipeline_cpp and
            "SetUniform1i(m_debugProgram, \"uSceneDepthTex\"" in pipeline_cpp,
            "surfel debug pass must bind G-buffer depth for trustworthy surface attachment validation")
    require("ComputeCameraCenteredGridBounds" in pipeline_cpp and "const glm::vec3 gridMin = -halfExtent" not in pipeline_cpp,
            "active surfel grid must be camera-centered, not fixed at world origin")

    require("if (!SurfelWorldToGridCell(worldPos, uGridMin, uGridMax, uGridResolution, baseCell))" in spawn_shader,
            "spawn shader must reject G-buffer samples outside the active surfel grid")
    require("SURFEL_SPAWN_TILE_CANDIDATES" in spawn_shader and "bestCoverage" in spawn_shader,
            "spawn candidate selection must test multiple tile samples and choose the lowest-coverage valid pixel")
    require(re.search(r"\buint\s+sample\b", spawn_shader) is None,
            "spawn shader must not use GLSL reserved token 'sample' as a local identifier")
    require("IsFiniteVec3" in spawn_shader and "rejectedInvalidWorldPos" in spawn_shader and "rejectedInvalidNormal" in spawn_shader,
            "spawn shader must reject invalid reconstructed positions and normals with explicit counters")
    require("surfel.debug = vec4((vec2(candidate.pixelU) + vec2(0.5)) / vec2(uResolution)" in spawn_shader,
            "spawn shader must persist spawn pixel metadata for placement diagnostics")
    require("SpawnRadiusForWorldPosition" in spawn_shader and "uTargetSurfelScreenRadiusPx" in spawn_shader,
            "spawn shader must initialize surfels with the same projected-radius policy used by updates")
    require("uniform int uPlacementValidationMode" in spawn_shader and
            "SURFEL_PLACEMENT_VALIDATION_CANDIDATES" in spawn_shader and
            "SURFEL_PLACEMENT_VALIDATION_TILE_PHASES" in spawn_shader and
            "placementPhase" in spawn_shader and
            "SpawnSurfelFromCandidate" in spawn_shader,
            "placement validation must spawn a bounded multi-candidate surface sample per tile instead of visualizing one production amortization sample")
    require("SURFEL_PRODUCTION_TILE_PHASES" not in spawn_shader and
            "productionPhase" not in spawn_shader and
            "TileSpawnPhase(tileIndex, SURFEL_PRODUCTION_TILE_PHASES)" not in spawn_shader,
            "normal production coverage must evaluate visible tiles every frame instead of refreshing coverage on a timed hash phase")
    require("SetUniform1i(program, \"uPlacementValidationMode\", placementOnly ? 1 : 0)" in pipeline_cpp,
            "pipeline must tell spawn.comp when it is running placement validation")
    require(glsl_uint_constant(spawn_shader, "SURFEL_SPAWN_FREE_POP_ATTEMPTS") >= 1024,
            "spawn free-list pop must tolerate heavy per-tile allocation contention without producing screen-space stripes")
    require("surfelID = freeIndices[newTop];" in spawn_shader and
            "atomicAdd(counters.rejectedPoolFull, 1u);\n    return false;\n}" not in spawn_shader,
            "spawn allocator must not misreport CAS contention as pool exhaustion")
    require("surfel.ids = uvec4(transformID, 0u, materialID" in spawn_shader,
            "surfel ids must store transform/entity/material in stable slots")
    require("SurfelMaterialAllowsDiffuseGI(surfel.ids.z)" in apply_shader,
            "final gather material rejection must read material ID from surfel.ids.z")
    require("SurfelOutgoingDiffuseRadiance" in apply_shader and
            "SurfelOutgoingDiffuseRadiance(surfel) * weight" in apply_shader,
            "final gather must source-tint surfel irradiance by surfel albedo so raw caches can produce colour bleeding")
    require("SurfelOutgoingDiffuseRadiance(surfel) * weight" in read("shaders/surfel_gi/cell_average.comp"),
            "cell-average fallback must also use real source-tinted surfel radiance")
    deferred_shader = read("shaders/deferred_lighting_frag.glsl")
    require("float indirectAttenuation = mix(0.35, 1.0, indirectAO);" in deferred_shader and
            "sourceIndex == 2 ? indirectAO" not in deferred_shader and
            "float sourceScale = 2.5;" in deferred_shader,
            "surfel-only composite must not apply an extra confidence/scale penalty compared with SSGI")
    require("return { 12288u, 1536u, 32u, 20u, 36u, 1u, 0.5f, false, 8u, 1u, 1u, 3u };" in modular_cpp,
            "default Medium Clean Surfel GI preset must be a real-time budget, not the capture budget")
    require("settings.rayUpdateInterval = std::max(settings.rayUpdateInterval, caps.minRayUpdateInterval);" in modular_cpp and
            "settings.spawnPasses = std::min(settings.spawnPasses, caps.maxSpawnPasses);" in modular_cpp,
            "real-time budget must cap update cadence and spawn passes, not only buffer sizes")
    require("atomicMax(surfels[entry.surfelID].frameInfo.x, uFrameIndex)" in spawn_shader and
            "SURFEL_COVERAGE_VISIBLE_TOUCH_WEIGHT" in spawn_shader,
            "coverage gap detection must refresh lastVisible for existing surfels that cover current G-buffer candidates")
    require("outsideGridScore" in recycle_shader and "outsideGrid ? 1.0" not in recycle_shader and "outsideGrid ||" not in recycle_shader,
            "recycling must treat outside-grid surfels as heuristic candidates, not immediately discard persistent cached surfels on camera movement")
    require("bool cameraMoving" in pipeline_cpp and
            "productionPhase" not in spawn_shader,
            "camera/view movement must not depend on timed production tile phases for coverage refresh")
    require("rebuildGridAndAverages(false)" in pipeline_cpp and "rebuildGridAndAverages(true)" in pipeline_cpp,
            "multi-pass spawning must rebuild grid coverage between passes without rebuilding expensive cell averages every pass")
    require("T_SURFEL_GBUFFER_NORMAL_RM" in common_shader and
            "layout(binding = T_SURFEL_GBUFFER_DEPTH)" in spawn_shader and
            "layout(binding = T_SURFEL_GBUFFER_DEPTH)" in apply_shader,
            "surfel shaders must use shared explicit G-buffer sampler binding constants")
    require("glBindTextureUnit(kSurfelGIGBufferDepthUnit" in pipeline_cpp and
            "LogSurfelGIGBufferBindings" in pipeline_cpp,
            "pipeline must bind/log surfel G-buffer resources through a single explicit binding table")
    require("placementValidationMode" in manager_h and "placementOnly" in pipeline_cpp,
            "pipeline must expose a minimal placement validation mode before lighting work")
    require("if (!placementOnly && m_updateShader" in pipeline_cpp,
            "placement validation must bypass transform reattachment/update so stored spawn positions can be validated directly")
    require("if (placementOnly && m_updateShader" not in pipeline_cpp,
            "placement validation must not run a post-spawn transform update that can corrupt current-frame world positions")
    require("m_wasPlacementValidationMode" in pipeline_cpp and
            "if (placementOnly && !m_wasPlacementValidationMode)" in pipeline_cpp and
            "Placement validation resets on entry" in pipeline_cpp,
            "placement validation must reset on entry, then accumulate world-space surfels instead of showing only a sparse per-frame tile sample")
    require("m_countLiveShader" in pipeline_cpp and "count_live_surfels.comp" in pipeline_cpp,
            "pipeline must recompute live surfel count from the actual buffer before presenting debug stats")
    require("counters.liveCount = 0u;" in read("shaders/surfel_gi/begin_frame.comp"),
            "begin-frame pass must clear liveCount before the explicit live-count pass")
    require("atomicAdd(counters.liveCount" not in spawn_shader and "counters.liveCount" not in recycle_shader,
            "liveCount must be measured from live surfel flags, not incrementally drift through spawn/recycle paths")
    require("SurfelIsAlive(surfels[surfelID])" in count_live_shader and "atomicAdd(counters.liveCount, 1u)" in count_live_shader,
            "live-count pass must count actual live surfel records")
    for field in ("worldPos_radius", "worldNormal_age", "localPos_spawnRadius", "localNormal_flags",
                  "ids", "albedo_life", "irradiance", "shortMean", "shortM2", "frameInfo", "debug"):
        require(f"offsetof(Surfel, {field})" in resources_h,
                f"C++ Surfel layout must static_assert offset for {field}")


if __name__ == "__main__":
    main()
