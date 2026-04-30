$ErrorActionPreference = 'Stop'

function Require-File {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing required file: $Path"
    }
}

function Require-Text {
    param([string]$Path, [string]$Pattern, [string]$Description)
    $text = Get-Content -LiteralPath $Path -Raw
    if ($text -notmatch $Pattern) {
        throw "Missing contract '$Description' in $Path"
    }
}

function Reject-Text {
    param([string]$Path, [string]$Pattern, [string]$Description)
    $text = Get-Content -LiteralPath $Path -Raw
    if ($text -match $Pattern) {
        throw "Forbidden contract '$Description' found in $Path"
    }
}

Require-File 'src/passes/SurfelIndirectDiffusePass.h'
Require-File 'src/passes/SurfelIndirectDiffusePass.cpp'
Require-File 'shaders/surfel_indirect_diffuse_gather_comp.glsl'

Require-Text 'src/ModularRenderer.cpp' 'SurfelIndirectDiffusePass' 'surfel indirect pass type is wired'
Require-Text 'src/ModularRenderer.cpp' 'SurfelIndirectDiffuse' 'surfel indirect frame-graph resource exists'
Require-Text 'src/passes/LightingPass.h' 'SetSurfelIndirectDiffuseTexture' 'lighting pass accepts surfel indirect texture'
Require-Text 'src/passes/LightingPass.cpp' 'surfelIndirectDiffuseMap' 'lighting pass binds surfel indirect map'
Require-Text 'shaders/deferred_lighting_frag.glsl' 'EvaluateSurfelIndirectDiffuseMap' 'deferred shader applies surfel texture'
Require-Text 'shaders/deferred_lighting_frag.glsl' 'INV_PI' 'surfel GI diffuse response uses lambert scaling'
Require-Text 'shaders/deferred_lighting_frag.glsl' 'uUseLegacySurfelFragmentGather' 'legacy per-fragment gather is debug gated'
Require-Text 'shaders/surfel_indirect_diffuse_gather_comp.glsl' 'ComputeFallbackSurfelWeight' 'fallback irradiance is receiver-relative weighted'
Require-Text 'shaders/surfel_indirect_diffuse_gather_comp.glsl' 'for \(int shell = 0; shell <= neighborRadius(?: && !gatherComplete)?; \+\+shell\)' 'receiver cell is visited before neighbor cells'
Require-Text 'shaders/surfel_indirect_diffuse_gather_comp.glsl' 'j \* count\) / max\(sampleCount, 1u\)' 'overfull cells are sampled across their populated range'
Require-Text 'shaders/surfel_indirect_diffuse_gather_comp.glsl' 'perCellBudget' 'candidate budget is distributed across queried cells'
Reject-Text 'shaders/surfel_indirect_diffuse_gather_comp.glsl' 'traceBVH' 'per-pixel ray tracing in Phase 3'
Reject-Text 'shaders/surfel_indirect_diffuse_gather_comp.glsl' 'fallbackW\s*=\s*clamp\(s\.irradianceHistory\.w' 'history-only cell-average surfel fallback causes tiled GI'
Reject-Text 'shaders/surfel_indirect_diffuse_gather_comp.glsl' 'cellI\s*=\s*clamp\(baseCell' 'clamped neighbor cells duplicate edge cells and cause tiled gather budgets'
Reject-Text 'shaders/surfel_indirect_diffuse_gather_comp.glsl' 'kTopGatherCapacity|selectedIrradiance|weakestWeight' 'full-resolution top-K gather is too expensive for Phase 3'
Reject-Text 'shaders/surfel_gi_integrate_comp.glsl' 'albedoAO\.rgb\s*\*' 'surfel irradiance cache must store incident irradiance, not albedo-multiplied surface color'

Write-Output 'Surfel Phase 3 contract verified.'
