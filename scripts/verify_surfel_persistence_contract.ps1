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

Assert-Contains "src/passes/SurfelGIPass.cpp" "ResetIrradianceFrameState\(budget\);" "Execute path must reset irradiance stats before request/allocation."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunAdaptiveRayRequest\(ctx, budget\);" "Execute path must run adaptive ray requests."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunGlobalRayAllocation\(ctx, budget\);" "Execute path must run global capped allocation."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunBoundedRayTrace\(ctx, dirLight, budget\);" "Execute path must run bounded surfel ray solving."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunTemporalAccumulation\(ctx, budget\);" "Execute path must run temporal accumulation."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunGuidingUpdate\(ctx, budget\);" "Execute path must run guiding updates."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunNeighbourSharing\(ctx, budget\);" "Execute path must run neighbour sharing."
Assert-Contains "src/passes/SurfelGIPass.cpp" "RunRadialDepthValidityUpdate\(ctx, budget\);" "Execute path must run radial-depth validity updates."

Assert-Contains "shaders/surfel_gi_ray_allocate_comp.glsl" "floor\(float\(requested\) \* float\(globalBudget\) / float\(totalRequested\)\)" "Ray allocation must proportionally enforce the global ray budget."
Assert-NotContains "shaders/deferred_lighting_frag.glsl" "RunCoverageGapFill|SpawnSurfel|RecycleSurfel" "Deferred shading must consume surfel irradiance without generating or recycling surfels."
Assert-Contains "shaders/deferred_lighting_frag.glsl" "sharedIrradiance" "Deferred shading must consume shared persistent irradiance when available."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 24" "Debug views must expose requested ray counts."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 25" "Debug views must expose allocated ray counts."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 26" "Debug views must expose raw irradiance."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 27" "Debug views must expose shared irradiance."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 28" "Debug views must expose history confidence."
Assert-Contains "shaders/surfel_gi_debug_vert.glsl" "uDebugMode == 29" "Debug views must expose guiding confidence."

Write-Host "Surfel persistence and irradiance contract passed."
