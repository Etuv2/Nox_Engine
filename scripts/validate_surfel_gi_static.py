from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    common = read("shaders/includes/surfel_gi_common.glsl")
    init = read("shaders/surfel_gi_init_comp.glsl")
    spawn = read("shaders/surfel_gi_spawn_comp.glsl")
    lifecycle = read("shaders/surfel_gi_lifecycle_comp.glsl")
    coverage_project = read("shaders/surfel_gi_coverage_project_comp.glsl")
    integrate = read("shaders/surfel_gi_integrate_comp.glsl")
    request = read("shaders/surfel_gi_ray_request_comp.glsl")
    ray_trace = read("shaders/surfel_gi_ray_trace_comp.glsl")
    neighbour_share = read("shaders/surfel_gi_neighbour_share_comp.glsl")
    grid_average = read("shaders/surfel_gi_grid_average_comp.glsl")
    gather = read("shaders/surfel_indirect_diffuse_gather_comp.glsl")
    lighting = read("shaders/deferred_lighting_frag.glsl")
    allocate = read("shaders/surfel_gi_ray_allocate_comp.glsl")
    indirect_pass = read("src/passes/SurfelIndirectDiffusePass.cpp")
    indirect_header = read("src/passes/SurfelIndirectDiffusePass.h")
    context = read("src/RenderContext.h")
    pass_cpp = read("src/passes/SurfelGIPass.cpp")

    require(
        "SurfelLightingStateFromHistory" in common,
        "common shader must expose lighting-state derivation from irradiance history",
    )
    require(
        "SurfelHasCurrentRawSample" in common,
        "common shader must expose a current-frame raw-sample validity helper",
    )
    require(
        "SurfelHasLightingSamples" in common and "SurfelHasReliableGatherLighting" in common,
        "common shader must distinguish sampled lighting history from reliable gather contributors",
    )
    require(
        "!SurfelHasAccumulatedIrradiance" not in common.partition("bool SurfelHasInitializedLighting")[2].partition("}")[0],
        "initialized lighting must not require nonzero RGB; valid black irradiance is still initialized after samples",
    )
    require(
        "lightingState = vec4(float(SURFEL_LIGHTING_STATE_UNINITIALIZED)" in init,
        "pool init must explicitly initialize lightingState",
    )
    require(
        "s.lightingState = vec4(float(SURFEL_LIGHTING_STATE_BOOTSTRAP)" in spawn,
        "spawned surfels must explicitly enter bootstrap lighting state",
    )
    require(
        "s.frames = uvec4(frameIndex, frameIndex, 0u, 0u)" in spawn,
        "spawn must not mark zero-history surfels as recently contributing; only temporal accumulation may write frames.z",
    )
    persistent_normal_users = [
        ("spawn", spawn),
        ("lifecycle", lifecycle),
        ("coverage_project", coverage_project),
        ("integrate", integrate),
        ("ray_trace", ray_trace),
        ("neighbour_share", neighbour_share),
        ("grid_average", grid_average),
        ("gather", gather),
        ("deferred_lighting", lighting),
    ]
    require(
        "SurfelStableNormal" in common and
        all("SurfelStableNormal" in source for _, source in persistent_normal_users) and
        all("SurfelFaceForwardToView" not in source for _, source in persistent_normal_users),
        "persistent Surfel GI normals must be stable geometric normals, not camera-facing normals",
    )
    require(
        "SurfelHasInitializedLighting(s)" in lifecycle and "canEnterDormancy" in lifecycle,
        "lifecycle dormancy must be gated by initialized lighting history",
    )
    require(
        "s.metrics.w = 1.0 - recycleScore" not in lifecycle and
        "rayRelevance" in lifecycle,
        "lifecycle must store actual ray relevance in metrics.w, not inverse recycle score",
    )
    require(
        "eligibilityReject0" in request and "eligibilityReject4" in request,
        "ray request shader must count detailed eligibility rejection reasons",
    )
    require(
        "isLightingUninitialized" in request and "bootstrapRayFloor" in request,
        "ray request shader must force bootstrap requests for uninitialized lighting",
    )
    require(
        "priorityBypassesSelectionCap" in request and
        "outsideSelectionWindow && !priorityBypassesSelectionCap" in request,
        "ray request selection cap must not reject visible, recently contributing, or lighting-uninitialized surfels",
    )
    require(
        "rayBudgetPressure" in request and
        "storedRelevance > 0.60" in request and
        "projectedThisFrame" in request,
        "ray request must not let generic in-frustum or recycle-protected surfels flood the budget",
    )
    require(
        "s.metrics.w = clamp(rayRelevance, 0.0, 0.65)" in lifecycle and
        "s.metrics.w = max(s.metrics.w, 1.0)" in coverage_project,
        "lifecycle relevance must stay below coverage-hit relevance so request scheduling can identify current gather surfels",
    )
    require(
        "bootstrapCritical" in request and
        "bootstrapMaintenance" in request and
        "if (!controlledValidation && bootstrapCritical)" in request and
        "historyDeficit > 0.35 && (screenRelevant || projectedThisFrame || storedRelevance > 0.35" in request and
        "historyDeficit > 0.35 && (screenRelevant || framesSinceVisible <= 30u" not in request and
        "controlledValidation ||\n        isLightingUninitialized ||" not in request,
        "ray request must prioritize current-view bootstrap surfels instead of letting every uninitialized surfel bypass the cap",
    )
    require(
        "allocationPriority" in allocate and
        "s.solveState.z" in allocate and
        "priorityShare" in allocate,
        "ray allocation must prioritize high-priority surfels when requests exceed the global ray cap",
    )
    require(
        'm_rayAllocateShader->SetUniform("uSurfelStart", static_cast<int>(m_rayCursor))' in pass_cpp,
        "ray allocation must rotate allocation order with the request cursor instead of always starting at surfel ID 0",
    )
    require(
        "dormant ? 0.0 : max(priority" not in request and
        "allocationPriorityFloor" in request,
        "dormant/maintenance surfels that request rays must keep a nonzero allocation priority",
    )
    require(
        "budget.maxRayTracedSurfels" in pass_cpp and "m_rayCursor" in pass_cpp,
        "ray request/allocation/trace dispatch must honor maxRayTracedSurfels with cursoring",
    )
    require(
        "kProductionMaxIrradianceRayBudget" in pass_cpp and
        "kProductionMaxRayTracedSurfels" in pass_cpp and
        "kProductionMaxRaysPerSurfel" in pass_cpp and
        "validationBudget" in pass_cpp and
        "Production uses motion-vector reprojection instead of validation-scale catch-up work" in pass_cpp,
        "production Surfel GI budgets must be capped separately from brute-force validation budgets",
    )
    require(
        "SurfelGridCellAverage" in common and
        "surfel_gi_grid_average_comp.glsl" in pass_cpp and
        "m_gridAverageSSBO" in pass_cpp and
        "surfelGIGridAverageBuffer" in context and
        "kSurfelBindingGridAverages" in indirect_pass,
        "GIBS final gather requires a per-cell average irradiance buffer exposed to the gather pass",
    )
    require(
        "RunNeighbourSharing(ctx, budget);" in pass_cpp and
        pass_cpp.find("RunNeighbourSharing(ctx, budget);") < pass_cpp.find("RunTemporalAccumulation(ctx, budget);") <
        pass_cpp.rfind("BuildGridCellAverages();"),
        "neighbour sharing must feed temporal history, and grid cell averages must be rebuilt from post-accumulation history",
    )
    require(
        ("SurfelHasInitializedLighting(s)" in grid_average or "SurfelHasReliableGatherLighting(s)" in grid_average) and
        "gridCellAverages[cell].irradianceWeight" in grid_average and
        "gridCellAverages[cell].normalCount" in grid_average,
        "grid average pass must compute cell irradiance from reliable persistent surfel history",
    )
    require(
        "irradianceLuma <= 0.000001" not in grid_average and "if (irradianceLuma" not in grid_average,
        "grid cell averages must preserve reliable dark irradiance instead of treating black as missing data",
    )
    require(
        "ResolveSmoothCellAverageFallback" in gather and
        "cellAverageConfidence" in gather and
        "directReliability" in gather and
        "1.0 - directReliability" in gather,
        "final gather must fill missing surfel contribution with smoothed cell-average irradiance based on reliability, not raw weight sum",
    )
    require(
        "SurfelHasReliableGatherLighting" in gather and
        "uDisableConfidenceReject != 0" not in gather.partition("float ComputeSurfelWeight")[2].partition("return tangentWeight")[0],
        "final gather must not let disabled confidence rejection promote uninitialized black surfels into full-weight contributors",
    )
    require(
        "SurfelHasCurrentRawSample(s, frameIndex)" in read("shaders/surfel_gi_temporal_accumulate_comp.glsl") and
        "SurfelHasCurrentRawSample(s, frameIndex)" in read("shaders/surfel_gi_guiding_update_comp.glsl") and
        "SurfelHasCurrentRawSample(s, frameIndex)" in read("shaders/surfel_gi_neighbour_share_comp.glsl") and
        'm_guidingUpdateShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex))' in pass_cpp and
        'm_neighbourShareShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex))' in pass_cpp,
        "raw, guiding, and neighbour sharing must consume only current-frame ray samples",
    )
    require(
        "uVelocity" in gather and
        "uHasVelocityHistory" in gather and
        "historyUV -= velocity" in gather and
        "ClampHistoryToCurrentEstimate" in gather and
        "ctx.velocityTex" in indirect_pass and
        "\"Velocity\", ResourceNames::SurfelField" in read("src/ModularRenderer.cpp"),
        "surfel final gather temporal history must use shared motion vectors and reject/clamp moving history",
    )
    require(
        "budget.gridRebuildInterval = kDefaultGridRebuildInterval" in pass_cpp,
        "GIBS spatial grid and cell averages must rebuild every frame for stable final gather",
    )
    require(
        "cameraMotion = std::max(cameraMotion, 0.35f)" not in pass_cpp and
        "firstCameraFrame" in pass_cpp,
        "surfel budget scaling must not force permanent camera-motion work every frame",
    )
    require(
        "kMinFinalGatherProjectionBudget" in pass_cpp and
        "budget.maxProjectedSurfels = std::max(budget.maxProjectedSurfels, finalGatherProjectionBudget)" in pass_cpp,
        "final gather winner coverage must not collapse when adaptive scale lowers the generic projection budget",
    )
    require(
        "surfelIndirectDiffuseUseTemporal = true" in context and
        "m_history" in indirect_header and
        "uHistoryIrradiance" in gather and
        "glCopyImageSubData(m_irradiance->ID()" in indirect_pass,
        "surfel final gather needs temporal history to prevent sparse candidate flicker",
    )
    require(
        "constantInjection" in pass_cpp and "!rtReady && !constantInjection" in pass_cpp,
        "constant irradiance injection must not be blocked by RT readiness",
    )
    require(
        "const float SURFEL_DEBUG_SUM_FIXED_SCALE = 256.0" in common and
        "kDebugSumFixedScale = 256.0f" in pass_cpp,
        "surfel luminance diagnostics need enough fixed-point precision for low indirect luma",
    )
    require(
        "SurfelPackWinner" in common and
        "SurfelUnpackWinnerID" in common and
        "imageAtomicMax(uWinnerSurfelID" in coverage_project and
        "SurfelUnpackWinnerID" in gather and
        "winnerClearValue = 0u" in pass_cpp,
        "projected coverage must select the strongest per-pixel surfel, not the smallest surfel ID",
    )
    require(
        "SURFEL_INDIRECT_RESPONSE_SCALE" in gather and
        "SURFEL_INDIRECT_RESPONSE_SCALE" in lighting and
        "* SURFEL_INDIRECT_RESPONSE_SCALE" in lighting,
        "surfel final gather diagnostics and deferred composite must share calibrated indirect response scale",
    )


if __name__ == "__main__":
    main()
