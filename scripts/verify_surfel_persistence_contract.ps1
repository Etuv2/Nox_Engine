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
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "vec4\s+rawIrradiance" "SurfelRecord must carry raw irradiance before neighbourhood sharing."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "vec4\s+sharedIrradiance" "SurfelRecord must carry shared irradiance after neighbourhood blending."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "vec4\s+solveState" "SurfelRecord must carry requested/allocated rays and solve state."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "struct\s+SurfelIrradianceHeader" "Surfel GI must expose a bounded irradiance dispatch header."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "SurfelCosineHemisphereSample" "Surfel GI must provide cosine hemisphere sampling for irradiance rays."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "SurfelGuideBin" "Surfel GI must provide compact guiding-bin indexing."
Assert-Contains "shaders/includes/surfel_gi_common.glsl" "SurfelRadialDepthBin" "Surfel GI must provide radial-depth bin indexing."

$phaseShaders = @(
    "surfel_gi_ray_request_comp.glsl",
    "surfel_gi_ray_allocate_comp.glsl",
    "surfel_gi_ray_trace_comp.glsl",
    "surfel_gi_temporal_accumulate_comp.glsl",
    "surfel_gi_guiding_update_comp.glsl",
    "surfel_gi_neighbour_share_comp.glsl",
    "surfel_gi_radial_depth_update_comp.glsl"
)

foreach ($shader in $phaseShaders) {
    Assert-Contains "src/passes/SurfelGIPass.cpp" $shader "Surfel GI must load $shader."
    Assert-Contains "Nox_Engine.vcxproj" ([regex]::Escape($shader)) "Project must include $shader."
}

Assert-Contains "src/passes/SurfelGIPass.h" "GpuSurfelRecord\).*== 272" "CPU surfel layout must match the expanded shader payload."
Assert-Contains "src/passes/SurfelGIPass.h" "GpuIrradianceHeader" "CPU pass must mirror the irradiance dispatch header."
Assert-Contains "src/passes/SurfelGIPass.h" "m_guidingBinsSSBO" "Surfel GI must allocate a stable-ID guiding distribution buffer."
Assert-Contains "src/passes/SurfelGIPass.h" "m_radialDepthBinsSSBO" "Surfel GI must allocate a stable-ID radial depth buffer."
Assert-Contains "src/RenderContext.h" "surfelGIMaxIrradianceRays" "RenderContext must expose a strict surfel irradiance ray budget."
Assert-Contains "src/RenderContext.h" "surfelGIMaxRaysPerSurfel" "RenderContext must cap per-surfel requested rays."
Assert-Contains "src/RenderContext.h" "surfelGIRTBuildBudgetMs" "RenderContext must expose a per-frame Surfel GI RT BLAS build budget."
Assert-Contains "src/RenderContext.h" "surfelGIRTMaxResidentMB" "RenderContext must expose a Surfel GI RT resident-memory budget."
Assert-Contains "src/RenderContext.h" "rtSceneResources" "RenderContext must expose shared RT scene resources for path tracing and Surfel GI."
Assert-Contains "src/RTSceneResources.h" "class\s+RTSceneResources" "Shared RT scene resource owner must exist."
Assert-Contains "src/RTSceneResources.h" "EnsureBuilt" "Shared RT scene resources must own BVH build/update access."
Assert-Contains "src/RTSceneResources.h" "EnsureIncrementalBLAS" "Shared RT scene resources must support incremental BLAS building for real-time Surfel GI."
Assert-Contains "src/RTSceneResources.h" "BindIncrementalForTracing" "Shared RT scene resources must bind incremental BLAS/TLAS buffers for Surfel GI tracing."
Assert-Contains "src/RTSceneResources.h" "BindForTracing" "Shared RT scene resources must bind BVH buffers for tracing shaders."
Assert-Contains "Nox_Engine.vcxproj" "src\\RTSceneResources\.cpp" "Visual Studio project must compile RTSceneResources.cpp."
Assert-Contains "Nox_Engine.vcxproj" "src\\RTSceneResources\.h" "Visual Studio project must include RTSceneResources.h."

Assert-Contains "src/passes/SurfelGIPass.cpp" "ResetIrradianceFrameState\(budget\);" "Execute path must reset irradiance stats before request/allocation."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunAdaptiveRayRequest\(ctx, budget\);" "Execute path must run adaptive ray requests."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunGlobalRayAllocation\(ctx, budget\);" "Execute path must run global capped allocation."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunBoundedRayTrace\(ctx, dirLight, budget\);" "Execute path must run bounded surfel ray solving."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunTemporalAccumulation\(ctx, budget\);" "Execute path must run temporal accumulation."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunGuidingUpdate\(ctx, budget\);" "Execute path must run guiding updates."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunNeighbourSharing\(ctx, budget\);" "Execute path must run neighbour sharing."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunRadialDepthValidityUpdate\(ctx, budget\);" "Execute path must run radial-depth validity updates."

Assert-Contains "shaders/surfel_gi_ray_allocate_comp.glsl" "floor\(float\(requested\) \* float\(globalBudget\) / float\(totalRequested\)\)" "Ray allocation must proportionally enforce the global ray budget."
Assert-Contains "shaders/surfel_gi_ray_allocate_comp.glsl" "remainderScore" "Ray allocation must include deterministic remainder scoring so one-ray surfels are not starved by flooring."
Assert-Contains "shaders/includes/rt_scene_common.glsl" "HitInfo\s+traceBVH" "Shared RT scene include must expose BVH traversal."
Assert-Contains "shaders/surfel_gi_ray_trace_comp.glsl" "includes/rt_scene_common.glsl" "Surfel ray tracing must use the shared RT traversal include."
Assert-Contains "shaders/surfel_gi_ray_trace_comp.glsl" "traceBVH" "Surfel ray tracing must call the shared BVH traversal backend."
Assert-Contains "shaders/surfel_gi_ray_trace_comp.glsl" "u_triangleCount" "Surfel ray tracing must receive shared RT triangle count."
Assert-Contains "shaders/surfel_gi_ray_trace_comp.glsl" "u_bvhNodeCount" "Surfel ray tracing must receive shared RT BVH node count."
Assert-Contains "shaders/surfel_gi_ray_trace_comp.glsl" "u_rtInstanceCount" "Surfel ray tracing must receive the incremental RT instance count."
Assert-Contains "shaders/includes/rt_scene_common.glsl" "traceInstanceScene" "Shared RT scene include must expose incremental instance traversal."
Assert-NotContains "shaders/surfel_gi_ray_trace_comp.glsl" "hitPoint\s*=\s*s\.worldPositionRadius\.xyz\s*\+\s*normal\s*\*\s*max\(s\.worldPositionRadius\.w\s*\*\s*0\.05,\s*0\.002\)\s*\+\s*rayDir\s*\*\s*maxDistance" "Surfel ray tracing must not fabricate hit points at max ray distance."
Assert-Contains "src/passes/SurfelGIPass.cpp" "rtSceneResources" "Surfel GI pass must consume shared RT scene resources."
Assert-Contains "src/passes/SurfelGIPass.cpp" "EnsureIncrementalBLAS" "Surfel GI must advance incremental BLAS builds instead of forcing monolithic scene BVH builds."
Assert-Contains "src/passes/SurfelGIPass.cpp" "BindIncrementalForTracing\(0u,\s*1u,\s*31u,\s*32u\)" "Surfel GI pass must bind incremental RT triangle/BVH/instance/TLAS buffers."
Assert-Contains "shaders/includes/rt_scene_common.glsl" "RTInstanceNodeBuffer" "Shared RT scene include must use a TLAS node buffer instead of a linear instance scan."
Assert-NotContains "src/passes/SurfelGIPass.cpp" "EnsureBuilt\(sceneGraph" "Surfel GI must not trigger the old monolithic full-scene BVH builder."
Assert-NotContains "shaders/deferred_lighting_frag.glsl" "RunCoverageGapFill|SpawnSurfel|RecycleSurfel" "Deferred shading must consume surfel irradiance without generating or recycling surfels."
Assert-Contains "shaders/deferred_lighting_frag.glsl" "sharedIrradiance" "Deferred shading must consume shared persistent irradiance when available."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 24" "Debug views must expose requested ray counts."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 25" "Debug views must expose allocated ray counts."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 26" "Debug views must expose raw irradiance."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 27" "Debug views must expose shared irradiance."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 28" "Debug views must expose history confidence."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 29" "Debug views must expose guiding confidence."

Write-Host "Surfel persistence and irradiance contract passed."
