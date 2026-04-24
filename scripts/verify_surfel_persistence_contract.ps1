param(
    [string]$Root = (Resolve-Path "$PSScriptRoot/..").Path
)

$ErrorActionPreference = "Stop"

function Assert-Contains {
    param(
        [string]$Path,
        [string]$Pattern,
        [string]$Message
    )

    $fullPath = Join-Path $Root $Path
    if (-not (Test-Path -LiteralPath $fullPath)) {
        throw "Missing file: $Path"
    }

    $content = Get-Content -LiteralPath $fullPath -Raw
    if ($content -notmatch $Pattern) {
        throw $Message
    }
}

function Assert-NotContains {
    param(
        [string]$Path,
        [string]$Pattern,
        [string]$Message
    )

    $fullPath = Join-Path $Root $Path
    if (-not (Test-Path -LiteralPath $fullPath)) {
        throw "Missing file: $Path"
    }

    $content = Get-Content -LiteralPath $fullPath -Raw
    if ($content -match $Pattern) {
        throw $Message
    }
}

Assert-Contains "shaders/includes/surfel_gi_common.glsl" "vec4\s+irradianceHistory" "SurfelRecord must carry persistent irradiance history."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "vec4\s+shortTermStats" "SurfelRecord must carry short-term integration statistics."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "vec4\s+longTermStats" "SurfelRecord must carry long-term integration statistics."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "vec4\s+recycleData" "SurfelRecord must carry recycle priority/debug data."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "uvec4\s+coverageStats" "Pool header must expose coverage-gap diagnostics."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "uvec4\s+contributionStats" "Pool header must expose old-vs-new contribution diagnostics."

Assert-Contains "src/RenderSystem.cpp" "GetWorldPublicationGeneration\(\)" "RenderSystem must track transform publication generation for transform-attached surfels."
Assert-Contains "src/RenderSystem.cpp" "publicationChangedWithoutDirtyList" "RenderSystem must handle editor-updated transforms whose dirty list was already consumed."
Assert-Contains "src/RenderSystem.cpp" "forceFullTransformUpload" "RenderSystem must force a transform SSBO upload when publication changed but no dirty list remains."

Assert-Contains "src/passes/SurfelGIPass.cpp" "surfel_gi_recycle_comp\.glsl" "Surfel GI must load a separate recycle decision pass."
Assert-Contains "src/passes/SurfelGIPass.cpp" "surfel_gi_tile_clear_comp\.glsl" "Surfel GI must clear per-frame tile coverage before projecting persistent surfels."
Assert-Contains "src/passes/SurfelGIPass.cpp" "surfel_gi_tile_coverage_comp\.glsl" "Surfel GI must project persistent surfel coverage before spawning gaps."
Assert-Contains "Nox_Engine.vcxproj" "surfel_gi_tile_clear_comp\.glsl" "Persistent tile-clear shader must be part of the project assets."
Assert-Contains "Nox_Engine.vcxproj" "surfel_gi_tile_coverage_comp\.glsl" "Persistent tile-coverage shader must be part of the project assets."
Assert-Contains "src/passes/SurfelGIPass.cpp" "surfel_gi_integrate_comp\.glsl" "Surfel GI must load a separate irradiance integration pass."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunPersistentStateUpdate" "Surfel GI execute path must name the persistent state update phase."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunRecycleDecision" "Surfel GI execute path must name the recycle decision phase."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunPersistentTileCoverage" "Surfel GI execute path must apply persistent surfel coverage before gap filling."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunCoverageGapFill" "Surfel GI execute path must name the coverage gap-fill phase."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunIrradianceIntegration" "Surfel GI execute path must name the irradiance integration phase."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunPersistentStateUpdate\(ctx, \*renderSystem, camera\);" "Execute path must update persistent surfels before coverage and spawn."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RebuildSpatialGrid\(ctx\.view\);" "Execute path must rebuild the persistent spatial grid."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunRecycleDecision\(ctx\);" "Execute path must run a demand-aware recycle decision."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunPersistentTileCoverage\(ctx\);" "Persistent surfel coverage stage must run in the execute path."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunCoverageGapFill\(ctx, \*renderSystem, camera, invViewProj, invView\);" "Coverage gap fill stage must run in the execute path."
Assert-Contains "src/passes/SurfelGIPass.cpp" "constexpr uint32_t kSpawnIterations = 2;" "Coverage gap fill must allow an additional severe-deficit fill pass without rebuilding the grid."
Assert-NotContains "src/passes/SurfelGIPass.cpp" "for \(uint32_t iteration = 0; iteration < kSpawnIterations; \+\+iteration\) \{\s*RebuildSpatialGrid\(ctx\.view\);" "Coverage gap fill must reuse the current persistent grid instead of rebuilding it before each spawn pass."

Assert-NotContains "shaders/surfel_gi_lifecycle_comp.glsl" "RecycleSurfel\s*\(" "Lifecycle update must not recycle surfels; recycling belongs to the budgeted recycle pass."
Assert-NotContains "shaders/surfel_gi_lifecycle_comp.glsl" "uCameraDemand" "Camera motion must not drive lifecycle recycling."
Assert-NotContains "shaders/surfel_gi_spawn_comp.glsl" "uCameraMotion" "Camera motion must not drive surfel spawning."

Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "coverageStats\.x" "Spawn pass must count tiles where persistent coverage prevents spawning."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "tilePersistentCoverage" "Spawn pass must consume persistent surfel-projected tile coverage."
Assert-NotContains "shaders/surfel_gi_spawn_comp.glsl" "if \(tilePersistentCoverage >= threshold\) \{\s*if \(localIndex == 0u\)" "Projected tile coverage alone must not skip spawning; rotating objects need G-buffer-verified local coverage."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "kTileSkipVerificationSamples" "Spawn pass must verify projected tile coverage against sampled G-buffer pixels before skipping."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "sTileVerifiedSkip" "Spawn pass must share the verified tile-skip decision across the workgroup."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "validSampleCount > 0u && verifiedMinCoverage >= threshold" "Tile skip must require valid same-transform sampled coverage, not only projected discs."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "if \(severity > 0\.82\)" "Severe uncovered gaps, including newly exposed rotated surfaces, must bypass probabilistic spawn throttling."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "severeGapBudgetScale" "Severe uncovered gaps must use remaining free IDs instead of being blocked by reserve throttling."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "severeGapBudgetScale = 0\.0" "Severe uncovered gaps must fully bypass the reserve floor while free IDs remain."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "frames\.y" "Coverage evaluation must update last-visible state for reused surfels."
Assert-NotContains "shaders/surfel_gi_spawn_comp.glsl" "frames\.z,\s*uint\(max\(uFrameIndex" "Coverage evaluation must not masquerade as lighting contribution."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "EvaluatePersistentTransformCoverage" "Spawn coverage must query persistent same-transform surfels, not only visible screen pixels."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "candidate\.ids\.x != transformID" "Coverage matching must reject surfels attached to a different transform."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "candidate\.localPositionAge\.xyz" "Coverage matching must compare stored transform-local surfel positions."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "candidate\.localNormalDebug\.xyz" "Coverage matching must compare stored transform-local surfel normals."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "kMaxCoverageEntries" "Spawn coverage must cap surfel candidates per texel for performance."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "kMaxConfirmedCoverageEntries" "Spawn admission must run a stronger persistent coverage confirmation for the selected tile texel."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "GridCellIndexForViewPosition\(viewPos, header\)" "Spawn coverage must use the persistent spatial grid for bounded lookup."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "if \(!hasCandidates\)" "Spawn coverage must avoid transform-local reconstruction when no persistent surfel candidates exist."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "markedPersistentHit" "Spawn coverage must limit visibility/stat atomics to one representative persistent hit per texel."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "ConfirmPersistentCoverageAtPixel" "Spawn pass must confirm missing coverage on the selected tile texel before allocation."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "minCoverage = confirmedCoverage" "Spawn threshold must use confirmed persistent coverage, not only cheap per-texel scan coverage."
Assert-Contains "shaders/surfel_gi_coverage_project_comp.glsl" "transformID != surfelTransformID" "Projected surfel coverage must reject visible pixels attached to a different transform."
Assert-Contains "shaders/surfel_gi_coverage_project_comp.glsl" "SurfelProjectPatchCorners" "Projected surfel coverage must use the projected tangent-plane patch corners for spec-faithful support."
Assert-Contains "shaders/surfel_gi_coverage_project_comp.glsl" "SurfelSupportBoundsFromCornerPixels" "Projected surfel coverage must derive raster bounds from the projected tangent-plane patch corners."
Assert-Contains "shaders/surfel_gi_coverage_project_comp.glsl" "kMaxExactCoverageSupportPixels" "Exact projected coverage must cap pathological screen-space support from grazing surfels."
Assert-Contains "shaders/surfel_gi_coverage_project_comp.glsl" "SurfelExactCoverageSupportWeight" "Exact projected coverage must use shape-weighted support instead of flat full-tile coverage."
Assert-Contains "shaders/surfel_gi_coverage_project_comp.glsl" "uProjectionFrameModulo" "Exact projected coverage must support temporal slicing to reduce full-pool projection cost."
Assert-Contains "src/passes/SurfelGIPass.cpp" "kExactCoverageFrameModulo" "Surfel pass must configure exact coverage temporal slicing."
Assert-NotContains "shaders/surfel_gi_coverage_project_comp.glsl" "float\s+supportWeight\s*=\s*1\.0;" "Exact projected coverage must not mark every texel in a projected patch with full weight."
Assert-NotContains "shaders/surfel_gi_coverage_project_comp.glsl" "s\.metrics\.z" "Projected surfel coverage must not size current-frame support from cached projected-radius heuristics."
Assert-NotContains "shaders/surfel_gi_lifecycle_comp.glsl" "orientationScale" "Surfel radius update must not bias world radius by view-angle heuristics."
Assert-NotContains "shaders/surfel_gi_lifecycle_comp.glsl" "complexityScale" "Surfel radius update must not bias world radius by depth-variance heuristics."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "return 12\.0;" "Automatic surfel target radius must use the accepted 12px operating point."
Assert-Contains "src/RenderContext.h" "surfelGITargetRadiusPixels = 12\.0f" "Optimized surfel coverage must use 12px as the accepted default operating radius."
Assert-Contains "src/ImGui/RenderingSettingsWindow.h" "m_surfelGITargetRadiusPixels = 12\.0f" "UI defaults must expose the 12px operating radius."
Assert-Contains "src/ImGui/RenderingSettingsWindow.cpp" "m_surfelGITargetRadiusPixels = 12\.0f" "Reset/default UI state must preserve the 12px operating radius."
Assert-Contains "src/passes/SurfelGIPass.cpp" "m_tileCoverageShader->Dispatch" "Optimized surfel coverage must dispatch the coarse tile confidence stage."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunPersistentTileCoverage\(ctx\);" "Coverage gap fill optimization must retain the persistent coverage stage."
Assert-NotContains "shaders/surfel_gi_coverage_project_comp.glsl" "layout\(binding = 24, std430\) buffer TileCoverageBuffer" "Exact projected coverage must not suppress the source-of-truth coverage field using same-frame coarse tile confidence."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunIrradianceIntegration\(ctx, dirLight\);" "Optimized execute path must continue to integrate irradiance after coverage and spawn."
Assert-Contains "src/passes/SurfelGIPass.cpp" "kIntegrationFrameInterval" "Surfel irradiance integration must support a lower update cadence than coverage/spawn."
Assert-NotContains "src/passes/SurfelGIPass.cpp" "RunRecycleDecision\(ctx\);\s*RunPersistentTileCoverage\(ctx\);\s*RunCoverageGapFill\(ctx, \*renderSystem, camera, invViewProj, invView\);\s*RunRecycleDecision\(ctx\);" "Optimized execute path must not run recycle twice per frame."
Assert-NotContains "src/passes/SurfelGIPass.cpp" "RunCoverageGapFill\(ctx, \*renderSystem, camera, invViewProj, invView\);\s*RunRecycleDecision\(ctx\);\s*RebuildSpatialGrid\(ctx\.view\);" "Optimized execute path must not rebuild the persistent grid again after spawn."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "kCoveredTileRecheckFrames" "Covered tiles must remember persistent coverage and skip repeated spawn checks."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "kUncertainTileRecheckFrames" "Uncertain tiles must be rechecked on a budget instead of every frame."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "previousTile\.w" "Tile coverage state must store the last coverage evaluation frame."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "skipTileEvaluation" "Spawn pass must skip tiles that recently had adequate persistent coverage."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "frameIndex \+ 1u\);" "Tile coverage evaluation must refresh persistent tile state."
Assert-NotContains "shaders/surfel_gi_spawn_comp.glsl" "for \(int z = -1; z <= 1; \+\+z\)" "Spawn coverage must not scan a 27-cell neighbourhood for every tile texel."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "kCoveredTileRecheckFrames = 4u" "Covered tile recheck windows must stay short so camera motion gets fresh G-buffer coverage quickly."
Assert-Contains "shaders/surfel_gi_spawn_comp.glsl" "kUncertainTileRecheckFrames = 1u" "Uncertain tile recheck windows must be effectively immediate while moving or revealing surfaces."
Assert-Contains "shaders/surfel_gi_grid_build_comp.glsl" "kMaxGridOverlapCellRadius = 1" "Spatial reinsertion must keep overlap fanout bounded to preserve sub-10ms surfel cost."
Assert-NotContains "shaders/surfel_gi_grid_build_comp.glsl" "ivec3\(3\)" "Spatial reinsertion must not expand to a 7x7x7 cell fanout."
Assert-NotContains "shaders/deferred_lighting_frag.glsl" "for \(int pass = 0; pass < 7; \+\+pass\)" "Deferred surfel application must not scan seven grid cells per full-resolution pixel."
Assert-Contains "shaders/deferred_lighting_frag.glsl" "uint cell = GridCellIndexForViewPosition\(viewSpacePos, surfelHeader\)" "Deferred surfel application must use the primary cell populated by overlap insertion."
Assert-Contains "src/passes/SurfelGIPass.cpp" "kStatsReadbackInterval" "Surfel stats readback must be throttled to avoid a per-frame CPU/GPU sync."
Assert-Contains "src/passes/SurfelGIPass.cpp" "ShouldReadBackStats" "Surfel pass must explicitly gate blocking stats readback."
Assert-Contains "src/passes/SurfelGIPass.cpp" "kMaxDebugSurfelInstances" "Surfel debug disc rendering must cap blended instance count."
Assert-Contains "src/passes/SurfelGIPass.cpp" "uDebugInstanceStride" "Surfel debug disc rendering must stride sampled surfels when the pool is large."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugInstanceStride" "Surfel debug vertex shader must use the CPU-provided sampling stride."
Assert-NotContains "src/passes/SurfelGIPass.cpp" "debugInstanceCount = std::clamp\(m_lastStats.liveCount \+ 4096u" "Surfel debug views must not draw nearly the full fixed pool in normal debug modes."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "EstimateCurrentGridCoverage" "Recycle pass must evaluate redundancy from the current rebuilt persistent surfel grid."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "layout\(binding = 25, std430\).*GridHeaderBuffer" "Recycle pass must read current spatial grid headers."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "layout\(binding = 26, std430\).*GridEntryBuffer" "Recycle pass must read current spatial grid entries."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "protectedByRecentVisibility" "Recycle pass must protect visible/recently visible surfels from stale-contribution recycling."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "!protectedByRecentVisibility && !protectedByRecentContribution" "Stale contribution recycling must not remove surfels that are still visible cache coverage."
Assert-Contains "src/passes/SurfelGIPass.cpp" "uCoverageDemandPressure" "Recycle pass must receive previous-frame coverage demand so new under-covered areas can make room."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "uniform float uCoverageDemandPressure" "Recycle shader must expose coverage-demand pressure separate from camera motion."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "demandPressure" "Recycle shader must fold under-coverage demand into budget pressure."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "frameCoverageDemandPressure" "Recycle shader must read current GPU gap-fill demand for same-frame reclamation."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "poolStall" "Recycle shader must detect pool-stall conditions once no-free-ID events begin."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "header\.coverageStats\.z > 0u" "Recycle shader must react directly to no-free-ID pressure."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "forceRecycle" "Recycle shader must provide a deterministic reclaim path under pool-stall pressure."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "emergencyReserveRefill" "Recycle shader must refill a small free-ID reserve when the pool is hard-stalled."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "stallRecycleCandidate" "Emergency recycling must still prefer far, stale, offscreen, or redundant surfels."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "targetFreeReserve" "Pool-stall recycling must use a bounded reserve target instead of unbounded cache destruction."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "visibleProtectionFrames = poolStall" "Recycle shader must shorten visibility protection windows under pool-stall pressure."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "contributionProtectionFrames = poolStall" "Recycle shader must shorten contribution protection windows under pool-stall pressure."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "farFromCamera" "Recycle shader must prefer old distant/off-screen surfels when making room for new coverage."
Assert-Contains "shaders/surfel_gi_recycle_comp.glsl" "redundantEnough = .*oversubScore" "Recycle shader must recycle genuinely redundant dense areas under pressure."

Assert-Contains "src/RenderContext.h" "surfelGISurfelBuffer" "RenderContext must expose the persistent surfel buffer to shading."
Assert-Contains "shaders/deferred_lighting_frag.glsl" "EvaluatePersistentSurfelGI" "Deferred lighting must consume persistent surfel irradiance."

Write-Host "Surfel persistence contract passed."
