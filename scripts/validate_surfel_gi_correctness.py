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
    build_grid_shader = read("shaders/surfel_gi/build_grid.comp")
    spatial_filter_shader = read("shaders/surfel_gi/spatial_filter.comp")
    temporal_filter_shader = read("shaders/surfel_gi/temporal_filter.comp")
    irradiance_snapshot_shader = read("shaders/surfel_gi/irradiance_snapshot.comp")
    irradiance_sharing_shader = read("shaders/surfel_gi/irradiance_sharing.comp")
    count_live_shader = read("shaders/surfel_gi/count_live_surfels.comp")
    common_shader = read("shaders/surfel_gi/common.glsl")
    debug_vert = read("shaders/surfel_gi/debug_surfels.vert")
    debug_frag = read("shaders/surfel_gi/debug_surfels.frag")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")
    modular_cpp = read("src/ModularRenderer.cpp")
    manager_h = read("src/passes/SurfelGIManager.h")
    resources_h = read("src/surfel_gi/SurfelGIResources.h")
    render_context_h = read("src/RenderContext.h")
    ui_cpp = read("src/ImGui/RenderingSettingsWindow.cpp")
    ui_h = read("src/ImGui/RenderingSettingsWindow.h")

    for relpath, shader in (
        ("shaders/surfel_gi/spawn.comp", spawn_shader),
        ("shaders/surfel_gi/apply_indirect.comp", apply_shader),
        ("shaders/surfel_gi/trace_rays.comp", trace_shader),
    ):
        require("materialID != 2u" not in shader and "materialID == 2u" not in shader,
                f"{relpath} must not treat deferred stable material ID 2 as transmissive")

    require("emitterFacing" in apply_shader and "receiverFacing" in apply_shader and "transferFacing" in apply_shader,
            "final gather must use transfer-facing weights so perpendicular colour bleeding is not rejected")
    require("coplanarFacing" in apply_shader,
            "final gather must still support same-surface smoothing without making it the only valid path")
    require("if (sumWeight <= 1e-5) {\n        imageStore(uOutIndirect, pixel, vec4(0.0));" in apply_shader,
            "final gather must return black when real surfel support is missing")
    require("mix(directIrradiance, boundedGridIrradiance, fallbackBlend)" in apply_shader and
            "BoundedFallbackBlend" in apply_shader and
            "ClampFallbackToDirectSupport" in apply_shader,
            "final gather must only use bounded cell-average fallback to smooth real surfel support")
    require("SURFEL_GRID_FALLBACK_KERNEL_RADIUS" in apply_shader,
            "grid fallback must sample a smooth cross-cell kernel, not a hard per-cell value")
    require("float directBlend = directCoverage;" in apply_shader,
            "raw surfel gather must not be damped by fallback during the correctness baseline")
    require("SURFEL_GATHER_MIN_SUPPORT_FRACTION" in apply_shader and
            "SURFEL_GATHER_MIN_SUPPORT_WORLD" in apply_shader and
            "smoothstep(1.15, 0.45, normalizedDistance)" not in apply_shader,
            "final gather support must be wide and smooth enough that individual surfel disks are not visible")
    require("SURFEL_APPLY_GATHER_CELLS_RADIUS_1 = 27u" in apply_shader and
            "SURFEL_APPLY_GATHER_CELLS_RADIUS_2 = 125u" in apply_shader and
            "cellSampleBudget" in apply_shader and
            "uint minSamplesPerCell = gatherBudget >= 96u ? 2u : 1u;" in apply_shader and
            "clamp(gatherBudget / max(gatherCellCount, 1u), minSamplesPerCell, neighborRadius == 1u ? 8u : 4u)" in apply_shader and
            "uFrameIndex ^ (cell * 747796405u)" not in apply_shader and
            "gl_GlobalInvocationID.x) * 2891336453u" not in apply_shader,
            "final gather must sample a stable spatially coherent bounded set from every neighbor cell")
    require("SurfelCoverageSupportRadiusAt" in common_shader and
            "SurfelCoverageSupportRadiusAt(" in build_grid_shader and
            "SurfelCoverageWeightAt(" in spawn_shader,
            "spawn coverage and grid insertion must use the surfel cache support radius, not the tiny debug disk radius")
    require("smoothstep(0.015, 0.22, sumWeight)" in apply_shader,
            "final gather confidence must trust stable direct surfel support before over-blending into cell averages")
    require("SurfelGatheredIrradianceForReceiver" in apply_shader and
            "surfel.albedo_life.rgb" in apply_shader and
            "sameSurfaceBlend" in apply_shader,
            "final gather must tint cross-surface transfer by source surfel albedo so Cornell color bleeding is visible")
    require("directIrradiance * directBlend" not in apply_shader and
            "directBlend > 0.0 ? directIrradiance : vec3(0.0)" not in apply_shader and
            "mix(directIrradiance, boundedGridIrradiance, fallbackBlend)" in apply_shader,
            "final gather must not darken already normalized direct surfel irradiance by coverage confidence")
    require("SURFEL_GRID_FALLBACK_MAX_BLEND" in apply_shader and
            "maxFallbackLum = directLum * mix" in apply_shader and
            "fallbackBlend = BoundedFallbackBlend" in apply_shader,
            "final gather must clamp fallback brightness and blend weight against real local surfel support")
    require("DEBUG_FINAL_GATHER_WEIGHT" in apply_shader,
            "apply pass must expose a per-pixel final gather weight debug view")
    require("float cellAverageFallbackStrength = 0.20f;" in manager_h,
            "cell-average fallback must remain a local hole filler instead of dominating the visible indirect image")
    for token in ("DEBUG_GBUFFER_WORLD_POSITION", "DEBUG_GBUFFER_NORMAL",
                  "DEBUG_GBUFFER_TRANSFORM_ID", "DEBUG_GBUFFER_MATERIAL_ID",
                  "DEBUG_GBUFFER_DEPTH", "DEBUG_SPAWN_CANDIDATES"):
        require(token in apply_shader, f"apply pass must expose {token} debug view")
    for token in ("StoredSurfelWorldPosition", "StoredSurfelRadius",
                  "StoredSurfelTransformID", "StoredSurfelFlags",
                  "DebugDrawPosition", "StoredSurfelAlbedo"):
        require(token in manager_h, f"SurfelGI debug enum must expose {token}")
    require("SetUniform1ui(program, \"uDebugView\", applyDebugView)" in pipeline_cpp,
            "pipeline must route fullscreen surfel debug views into the apply shader")
    require("SetUniform1f(program, \"uFallbackStrength\", m_settings.cellAverageFallbackStrength)" in pipeline_cpp and
            "cellAverageFallbackStrength = 0.20f" in manager_h,
            "surfel-only baseline must use a bounded confidence-weighted cell-average fallback")
    require("initialSkyVisibility" in spawn_shader and
            "initialDirectScale = 0.04" in spawn_shader and
            "initialEnvironmentEstimate = max(uSkyRadiance, vec3(0.0)) * initialSkyVisibility * 0.03" in spawn_shader,
            "new surfels may only receive a low-confidence directional/environment hint before real ray hits take over")

    require(uint_constant(trace_shader, "SURFEL_TRACE_MAX_CELL_ENTRIES") == 12,
            "default surfel fallback tracing must sample enough cell entries without blowing the real-time budget")
    require(uint_constant(trace_shader, "SURFEL_TRACE_MAX_CANDIDATES") == 128,
            "default surfel fallback tracing must use a bounded candidate budget for stable real-time colour-bleed hits")
    require("SURFEL_TRACE_HIT_RADIUS_SCALE" in trace_shader and
            "SURFEL_TRACE_MIN_HIT_RADIUS" in trace_shader and
            "SURFEL_TRACE_MAX_HIT_RADIUS" in trace_shader,
            "surfel ray tracing must use bounded cache-element support rather than huge volumetric disks")
    require("t < gather.closestT - clusterThickness" in trace_shader and
            "abs(t - gather.closestT) <= clusterThickness" in trace_shader,
            "surfel ray fallback must resolve a nearest-hit cluster instead of averaging unrelated surfaces along the ray")
    require("TraceMarkVisitedCell" in trace_shader and
            "visitedCells" in trace_shader and
            "SURFEL_TRACE_MAX_VISITED_CELLS" in trace_shader,
            "surfel ray fallback must not burn its candidate budget by revisiting the same grid cells near the ray origin")

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
    require("DEBUG_STORED_SURFEL_ALBEDO" in debug_vert and
            "surfel.albedo_life.rgb" in debug_vert and
            '"Stored Albedo"' in ui_cpp and
            "case SurfelGIDebugView::StoredSurfelAlbedo:" in read("src/passes/SurfelGIManager.cpp") and
            "std::clamp(m_context.cleanSurfelGIDebugView, 0, 35)" in modular_cpp and
            "std::clamp(GetEnvVarInt(\"NOX_SURFEL_GI_DEBUG_VIEW\", static_cast<int>(settings.debugView)), 0, 35)" in modular_cpp,
            "debug surfel draw must expose stored surfel albedo to validate material colour bleeding inputs")
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

    require("SurfelWorldToGridAddress(" in spawn_shader and "rejectedOutsideGrid" in spawn_shader,
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
    require("SurfelCachedIndirectIrradiance" in apply_shader and
            "SurfelGatheredIrradianceForReceiver(surfel, normal) * weight" in apply_shader and
            "surfel.albedo_life.rgb" not in apply_shader.split("vec3 SurfelCachedIndirectIrradiance")[1].split("}")[0],
            "final gather must interpolate same-surface irradiance while using source-tinted transfer only for cross-surface bounce")
    require("surfel.ids = uvec4(transformID, 0u, materialID, transform.metadata.z)" in spawn_shader and
            "SURFEL_EMISSIVE" in spawn_shader and
            ("SurfelPackIrradiance(vec3(0.02), 0.02)" in spawn_shader or
             "guideMap[guideBase + guideBin].packedRadiance = 0u" in spawn_shader),
            "spawning must store transform generation, mark real emissive surfels, and reset ray-guiding cells")
    update_shader = read("shaders/surfel_gi/update_surfels.comp")
    require("transform.metadata.z != surfel.ids.w" in update_shader and
            "motionReset" in update_shader and
            "transform.prevWorld" in update_shader,
            "surfel update must reject reused transform IDs and reduce confidence after rigid-object motion")
    require("confidence < 0.08 && luminance <= 1e-5" in apply_shader and
            "mix(0.22, 1.0, smoothstep(0.05, 0.65, confidence))" in apply_shader and
            "mix(0.25, 1.0, confidence)" not in apply_shader,
            "final gather must reject low-confidence black surfels without dropping valid low-confidence bounce samples")
    cell_average_shader = read("shaders/surfel_gi/cell_average.comp")
    require("SurfelCachedIndirectIrradiance(surfel) * weight" in cell_average_shader and
            "surfel.albedo_life.rgb" not in cell_average_shader.split("vec3 SurfelCachedIndirectIrradiance")[1].split("}")[0],
            "cell-average fallback must store irradiance, not source-tinted outgoing radiance")
    deferred_shader = read("shaders/deferred_lighting_frag.glsl")
    require("float indirectAttenuation = sourceIndex == 2 ? 1.0 : mix(0.35, 1.0, indirectAO);" in deferred_shader and
            "float sourceScale = 1.0;" in deferred_shader,
            "Clean Surfel GI alpha is confidence/debug data and must not attenuate linear irradiance in deferred lighting")
    require("return { 24576u, 1024u, 32u, 24u, 48u, 1u, 0.4375f, false, false, 1u, 1u, 2u, 6u };" not in modular_cpp,
            "legacy weak Medium Clean Surfel GI budget must not remain in place")
    require("return { 32768u, 640u, 16u, 20u, 24u, 1u, 0.25f, true, true, false, 1.0f, 1u, 1u, 3u, 8u };" in modular_cpp,
            "default Medium Clean Surfel GI preset must keep guided/radial quality while using the real-time gather budget")
    require("NOX_SURFEL_GI_QUALITY_TIER" in modular_cpp,
            "validation runs must be able to force Low/Medium/High/Ultra quality tiers without relying on the ImGui combo")
    require("NOX_SURFEL_GI_IRRADIANCE_SHARING" in modular_cpp,
            "validation runs must be able to A/B irradiance sharing without editing code")
    require("settings.rayUpdateInterval = std::max(settings.rayUpdateInterval, caps.minRayUpdateInterval);" in modular_cpp and
            "settings.useRadialDepth = settings.useRadialDepth && caps.allowRadialDepth;" in modular_cpp and
            "settings.spawnPasses = std::min(settings.spawnPasses, caps.maxSpawnPasses);" in modular_cpp,
            "real-time budget must cap unstable optional features, update cadence, and spawn passes, not only buffer sizes")
    require("NOX_SURFEL_GI_LOG_SUBPASS_METRICS" in pipeline_cpp and
            "[SurfelGISubpassMetrics]" in pipeline_cpp and
            ('"trace_rays"' in pipeline_cpp or '"tracing"' in pipeline_cpp) and
            (('"apply_indirect"' in pipeline_cpp and '"spatial_filter"' in pipeline_cpp) or
             ('"final_gather"' in pipeline_cpp and '"composite_filter"' in pipeline_cpp)),
            "SurfelGI must expose opt-in per-subpass GPU metrics for real-time budget debugging")
    require("const bool updateRaysThisFrame = true;" not in pipeline_cpp and
            "rayUpdateInterval" in pipeline_cpp and
            "m_frameIndex % rayUpdateInterval" in pipeline_cpp,
            "ray tracing must honor the configured ray update interval instead of tracing the full budget every frame")
    require("atomicMax(surfels[entry.surfelID].frameInfo.x, uFrameIndex)" in spawn_shader and
            "SURFEL_COVERAGE_VISIBLE_TOUCH_WEIGHT" in spawn_shader,
            "coverage gap detection must refresh lastVisible for existing surfels that cover current G-buffer candidates")
    require("uint strideSeed = SurfelHash((cell * 747796405u)" in spawn_shader and
            "uint strideSeed = SurfelHash(uFrameIndex ^ (cell * 747796405u))" not in spawn_shader,
            "coverage estimates must be stable for the same surface instead of changing on a timed frame interval")
    require("entryCount >= uMaxSurfelsPerCell" not in spawn_shader,
            "spawn must not reject under-covered candidates just because the grid cell entry list is capped")
    integrate_shader = read("shaders/surfel_gi/integrate.comp")
    require("vec3 sampleMean = batchSum / max(acceptedWeight, 1e-5);" in integrate_shader and
            "directBaseline + batchMean" not in integrate_shader,
            "surfel irradiance caches must store traced indirect irradiance, not the surfel's own direct lighting")
    require("SurfelLuminance(sampleMean) <= 1e-5 && SurfelLuminance(priorIrradiance) > 1e-5" not in integrate_shader and
            "bool emissiveSurfel = (SurfelFlags(surfel) & SURFEL_EMISSIVE) != 0u;" in integrate_shader and
            "vec3 emissiveBaseline = emissiveSurfel ? SurfelClampEmissive(priorIrradiance) : vec3(0.0);" in integrate_shader,
            "temporal integration must let valid dark hits decay stale GI and preserve only real emissive surfels")
    require("bool validSample = hit.normal_hitKind.w > 0.5;" in integrate_shader and
            "bool validSample" in integrate_shader and
            "if (!validSample)" in integrate_shader and
            "++accepted;" in integrate_shader,
            "surfel integration must accept real surface/environment samples only, including black hits, and reject invalid misses")
    require("HitKindReliability" in integrate_shader and
            "float temporalConfidence = SurfelSaturate(surfel.irradiance.a);" in integrate_shader and
            "float sampleStability = 1.0 - smoothstep(0.06, 0.75, varLum);" in integrate_shader and
            "float consistentChange = smoothstep(0.16, 1.35, diffLum) * sampleStability;" in integrate_shader and
            "float confidenceDecay = consistentChange * 0.035;" in integrate_shader and
            "float alphaMax = mix(0.065, 0.016, temporalConfidence);" in integrate_shader and
            "float alpha = mix(0.003, alphaMax, consistentChange);" in integrate_shader and
            "alpha = max(alpha, 0.30);" in integrate_shader and
            "TemporalClampIrradiance(previousIrradiance, integratedIrradiance)" in integrate_shader,
            "surfel integration must reliability-weight hit evidence, damp noisy swings as confidence rises, and bootstrap new surfels without flicker")
    trace_shader = read("shaders/surfel_gi/trace_rays.comp")
    require(12 <= uint_constant(trace_shader, "SURFEL_TRACE_STEPS") <= 20 and
            128 <= uint_constant(trace_shader, "SURFEL_TRACE_MAX_CANDIDATES") <= 256 and
            uint_constant(trace_shader, "SURFEL_TRACE_MAX_VISITED_CELLS") <= 40 and
            "localBounceRayLength" in pipeline_cpp,
            "default surfel-grid fallback rays must stay bounded while sampling a local GI segment densely enough to hit Cornell-scale geometry")
    require("layout(binding = B_LIGHTS, std430) readonly buffer LightDataBuffer" in trace_shader and
            "uniform uint uLightCount" in trace_shader and
            "EvaluateHitDirectIrradiance" in trace_shader and
            "context.lightManager->GetLightDataSSBO()" in pipeline_cpp and
            "uLightCount" in pipeline_cpp,
            "surfel ray-hit lighting must use the renderer LightManager SSBO, not a stale single directional fallback")
    require("TraceCachedIndirectAtSurface" in trace_shader and
            "directIrradiance + environmentIrradiance + cachedIndirectIrradiance" in trace_shader and
            "EvaluateHitEnvironmentIrradiance" in trace_shader and
            "TraceSkyVisibilityAtSurface" in trace_shader and
            "SkyOcclusionVisibilityFromHitDistance" in trace_shader and
            "uSkyIrradianceMap" in trace_shader and
            "TraceSoftwareBVH(origin, skyDir" in trace_shader and
            "return hemisphereAccess * SkyOcclusionVisibilityFromHitDistance(skyHit.t);" in trace_shader,
            "screen/BVH ray hits must shade real hit radiance from direct lighting, distance-weighted visible skybox irradiance, emissive, and cached indirect without fake sky miss ambient")
    require("TraceCachedAlbedoAtSurface" in trace_shader and
            "cachedAlbedoConfidence" in trace_shader and
            "cachedAlbedoBlend" in trace_shader and
            "mix(albedo, cachedAlbedo, cachedAlbedoBlend)" in trace_shader,
            "BVH ray hits must borrow nearby surfel albedo so textured red/green surfaces can produce colour bleeding")
    require("gather.radiance = vec3(0.0);" in trace_shader and
            "uSkyRadiance * max(rayDir.y, 0.0)" not in trace_shader and
            "EvaluateMissEnvironmentRadiance" in trace_shader and
            "SURFEL_TRACE_HIT_KIND_ENVIRONMENT" in trace_shader,
            "ray misses must use directional environment radiance only when they escape to sky, not a flat ambient fill")
    require("emissiveSurfelHit" in trace_shader and
            "emissiveRadiance" in trace_shader and
            "SURFEL_EMISSIVE" in trace_shader,
            "surfel-grid ray hits must preserve emissive source radiance instead of treating it as diffuse irradiance")
    request_shader = read("shaders/surfel_gi/request_rays.comp")
    require("SURFEL_REQUEST_MAX_RAYS_PER_SURFEL = 6u" in request_shader and
            "SURFEL_REQUEST_VISIBLE_UNRESOLVED_RAYS = 6u" in request_shader,
            "visible unresolved surfels must get enough rays per update to converge without persistent flicker")
    require("const uint SURFEL_SCREEN_TRACE_STEPS = 32u;" in trace_shader and
            "const uint SURFEL_TRACE_CACHE_MAX_CANDIDATES = 72u;" in trace_shader and
            "const uint SURFEL_TRACE_CACHE_PER_CELL_BUDGET = 2u;" in trace_shader and
            "const float SURFEL_TRACE_CACHE_SUPPORT_SPREAD = 1.45;" in trace_shader and
            "SurfelTraceSourceBounceTint" in trace_shader,
            "default trace shader must bound screen-walk while sampling enough cached multi-bounce surfel support for colour bleeding")
    require("layout(binding = B_SHADOW_MATRICES, std430) readonly buffer ShadowMatricesBuffer" in trace_shader and
            "uniform sampler2DArrayShadow uMultiLightShadowArray" in trace_shader and
            "EvaluateLightShadow" in trace_shader and
            "direct += max(light.color.rgb, vec3(0.0)) * attenuation * nDotL * shadow" in trace_shader and
            "context.lightManager->GetShadowMatricesSSBO()" in pipeline_cpp and
            "context.lightManager->GetShadowArrayTexture()" in pipeline_cpp,
            "surfel ray-hit direct lighting must use the renderer shadow maps so bounce sources are not unshadowed direct lighting")
    require("TraceScreenSpace" in trace_shader and
            "layout(binding = T_SURFEL_GBUFFER_DEPTH) uniform sampler2D uDepthTex" in trace_shader and
            "uUseScreenSpaceTrace" in trace_shader and
            "SurfelReconstructWorldPosition" in trace_shader and
            "ScreenSpaceHitRadiance" in trace_shader and
            "BindSurfelGIGBufferTextures(context)" in pipeline_cpp and
            "SetUniform1i(program, \"uUseScreenSpaceTrace\", m_settings.useScreenSpaceTrace ? 1 : 0)" in pipeline_cpp,
            "trace pass must use the visible G-buffer as the primary hybrid ray-hit path before surfel-grid fallback")
    require("bool useScreenSpaceTrace = false;" in manager_h and
            "bool cleanSurfelGIUseScreenTrace = false;" in render_context_h and
            "bool m_cleanSurfelGIUseScreenTrace = false;" in ui_h and
            "m_cleanSurfelGIUseScreenTrace = false;" in ui_cpp,
            "screen-space surfel ray hits must be an explicit opt-in because fixed-step screen traces create cache striping/flicker")
    require("TraceSoftwareBVH" in trace_shader and
            "TraceSoftwareBVHRadiance" in trace_shader and
            "SURFEL_TRACE_HIT_KIND_BVH" in trace_shader and
            "layout(std140, binding = B_BVH_TRIANGLES)" in trace_shader and
            "layout(std140, binding = B_BVH_NODES)" in trace_shader and
            "context.rtSceneResources->EnsureBuilt" in pipeline_cpp and
            "context.rtSceneResources->BindForTracing" in pipeline_cpp and
            "SetUniform1i(program, \"uUseSoftwareBVHTrace\"" in pipeline_cpp,
            "trace pass must use the shared software BVH between screen-space and surfel-grid fallback")
    allocate_shader = read("shaders/surfel_gi/allocate_rays.comp")
    require("uniform uint uFrameIndex;" in allocate_shader and
            "rotatedRequestID" in allocate_shader and
            "SurfelHash(uFrameIndex" in allocate_shader and
            "SetUniform1ui(program, \"uFrameIndex\", m_frameIndex)" in pipeline_cpp,
            "ray allocation must rotate request order per frame so a fixed low-ID prefix cannot monopolize the global ray budget")
    require("const float SURFEL_RADIAL_DEPTH_MIN_VISIBILITY" in common_shader and
            "mix(SURFEL_RADIAL_DEPTH_MIN_VISIBILITY, 1.0" in common_shader,
            "radial depth rejection must soften leakage without creating black splotches")
    require("float radialDefaultDepth = max(radius * 8.0, 1.25);" in spawn_shader,
            "new surfels must initialize radial depth with the same conservative default used by the radial update pass")
    generate_rays_shader = read("shaders/surfel_gi/generate_rays.comp")
    require("SurfelUnpackGuideRadiance" in generate_rays_shader and
            "SURFEL_GUIDE_MIX_PROBABILITY" in generate_rays_shader,
            "ray guiding must sample decoded learned radiance through a bounded mixture PDF")
    require("bool SurfelSelectGuideBin(" in generate_rays_shader and
            "if (!SurfelSelectGuideBin(surfelID, selectionTarget, totalWeight, selectedBin, selectedWeight))" in generate_rays_shader and
            "!(selectedWeight > 0.0)" not in generate_rays_shader,
            "ray guiding must select a positive learned-radiance bin before testing selected weight")
    require("irradiance_snapshot.comp" in pipeline_cpp and
            "m_irradianceSnapshotShader" in pipeline_cpp and
            '"irradiance_snapshot"' in pipeline_cpp and
            "B_IRRADIANCE_SNAPSHOT" in common_shader and
            "layout(binding = B_IRRADIANCE_SNAPSHOT, std430) buffer SurfelIrradianceSnapshotBuffer" in irradiance_snapshot_shader,
            "irradiance sharing must snapshot source irradiance before filtering so neighbor reads are frame-stable")
    require("irradiance_sharing.comp" in pipeline_cpp and
            "m_irradianceSharingShader" in pipeline_cpp and
            "m_settings.useIrradianceSharing" in pipeline_cpp and
            '"irradiance_sharing"' in pipeline_cpp,
            "pipeline must dispatch a surfel-to-surfel irradiance sharing pass before building cell averages")
    require("layout(binding = B_SURFELS, std430) buffer SurfelBuffer" in irradiance_sharing_shader and
            "layout(binding = B_IRRADIANCE_SNAPSHOT, std430) readonly buffer SurfelIrradianceSnapshotBuffer" in irradiance_sharing_shader and
            "SurfelWorldNeighborAddress" in irradiance_sharing_shader and
            "SURFEL_SHARING_MAX_NEIGHBOR_CELLS" in irradiance_sharing_shader and
            "SURFEL_SHARING_MAX_NEIGHBORS" in irradiance_sharing_shader and
            "RadialDepthTexel" in irradiance_sharing_shader and
            "SurfelRadialDepthVisibility" in irradiance_sharing_shader and
            "surfel.irradiance.rgb = SurfelClampSharedIrradiance(mix(" in irradiance_sharing_shader,
            "irradiance sharing must reuse compatible nearby surfels with normal/plane/radial-depth gates instead of image-space smoothing")
    require("surfel.frameInfo.y = max(surfel.frameInfo.y, uFrameIndex)" not in irradiance_sharing_shader,
            "irradiance sharing must not stamp the last ray-contribution frame or stable surfels never enter a lower-frequency update cadence")
    for token in (
        "SharingConnectedSurfaceWeight",
        "SURFEL_SHARING_MIN_NORMAL_DOT",
        "SURFEL_SHARING_MAX_PLANE_SEPARATION",
        "SURFEL_SHARING_MIN_RADIAL_VISIBILITY",
        "SharingPreserveGradientWeight",
        "SharingDarknessPreservationBlend",
    ):
        require(token in irradiance_sharing_shader,
                f"irradiance sharing must expose leak-safe compatibility contract token: {token}")
    require("if (normalDot < SURFEL_SHARING_MIN_NORMAL_DOT)" in irradiance_sharing_shader and
            "if (max(centerPlane, neighborPlane) > planeLimit)" in irradiance_sharing_shader and
            "if (visibility < SURFEL_SHARING_MIN_RADIAL_VISIBILITY)" in irradiance_sharing_shader and
            "if (connectedWeight <= 1e-5)" in irradiance_sharing_shader,
            "irradiance sharing must hard-reject incompatible normals, plane separation, radial-depth occlusion, and disconnected surfaces")
    require("const bool updateRaysThisFrame = true;" not in pipeline_cpp and
            "m_frameIndex < m_settings.fastFillFrameCount" in pipeline_cpp,
            "real-time ray tracing may be cadenced, but startup must fast-fill and coverage/spawn must stay independent")
    require("outsideGridScore" in recycle_shader and "outsideGrid ? 1.0" not in recycle_shader and "outsideGrid ||" not in recycle_shader,
            "recycling must treat outside-grid surfels as heuristic candidates, not immediately discard persistent cached surfels on camera movement")
    require("uTargetSurfelScreenRadiusPx * WorldUnitsPerPixel" in read("shaders/surfel_gi/update_surfels.comp") and
            "targetRadiusError" in read("shaders/surfel_gi/update_surfels.comp"),
            "surfel update must shrink/grow radius from current camera projection and expose target error")
    require("contributionAge > 720u" in recycle_shader and "confidence < 0.45" in recycle_shader,
            "stale recycling must be gradual and relevance-based, not a high-probability constant interval churn")
    require("SURFEL_CELL_OVERFLOWED" in build_grid_shader and "replaceHash" in build_grid_shader and "seenCount" in build_grid_shader,
            "overflowed grid cells must reservoir-replace entries instead of preserving only early surfel IDs")
    require("const int FILTER_RADIUS = 3;" in spatial_filter_shader and
            "if (centerConfidence <= 1e-4) {\n        imageStore(uOutIndirect, pixel, vec4(0.0));" in spatial_filter_shader and
            "neighborWeightSum" in spatial_filter_shader and
            "supportConfidence" in spatial_filter_shader and
            "colorWeight" in spatial_filter_shader and
            "sampleConfidence * w" in spatial_filter_shader,
            "realtime spatial resolve must filter supported samples without synthesizing unsupported neighbor-only GI")
    require("filtered *= supportConfidence" not in spatial_filter_shader and
            "confidence = max(centerConfidence * 0.75" in spatial_filter_shader,
            "spatial resolve must not darken valid indirect tiles just because neighbouring support is sparse")
    require("temporal_filter.comp" in pipeline_cpp and
            "m_temporalFilterShader" in pipeline_cpp and
            '"temporal_resolve"' in pipeline_cpp and
            "historyIndirectTexture[2]" in read("src/passes/SurfelGIPipeline.h") and
            "historyGeometryTexture[2]" in read("src/passes/SurfelGIPipeline.h"),
            "final surfel indirect output must include a velocity-reprojected temporal resolve after spatial filtering")
    require("uVelocityTex" in temporal_filter_shader and
            "uPreviousGeometryTex" in temporal_filter_shader and
            "uDepthReject" in temporal_filter_shader and
            "uNormalRejectCos" in temporal_filter_shader and
            "ClampHistoryToCurrentNeighborhood" in temporal_filter_shader and
            "StoreResolved" in temporal_filter_shader,
            "surfel temporal resolve must reject history by motion/depth/normal and clamp reused radiance to current support")
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
