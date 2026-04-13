# Engine Refactor Execution Ledger

## Goal
- Reduce the deferred renderer's bandwidth and fullscreen-pass cost.
- Stabilize physics behavior and determinism.
- Introduce stable render identity and GPU transform data for future GI work.
- Unify material evaluation and shadow behavior behind shared contracts.

## Wave Ownership
- Wave 1:
  - Renderer triage: pass-local SSAO, screen-space shadow, SSGI, TAA, and lighting changes.
  - Physics: deterministic pair/manifold handling, ownership fixes, narrowphase cleanup.
- Wave 2:
  - TransformID and GPU transform buffer.
  - Shadow backend stabilization without deferred-lighting integration changes.
- Wave 3:
  - Canonical material pipeline and shared BRDF integration.
- Wave 4:
  - Deferred shadow integration, debug views, cleanup.

## Shared Integration Contracts
- `RenderContext` owns the user-facing quality controls for SSAO, SSGI, and contact shadow resolution and temporal behavior.
- `TransformComponent` will gain stable identity and previous-frame state.
- The deferred G-buffer will expand to include a dedicated transform identity attachment.
- Material evaluation must converge on shared BRDF/material includes used by deferred, forward, and RT paths.

## Validation Expectations
- Renderer:
  - Record before/after timings for SSAO, screen-space shadows, SSGI, lighting, and total frame.
  - Verify no new full-resolution bilateral chains are introduced.
- Physics:
  - Verify contact counts and final transforms are stable across repeated runs.
  - Verify gizmo-grabbed bodies do not continue integrating in conflicting ownership states.
- Shadows:
  - Verify static-camera and slow-pan stability without cascade popping or shimmer.
- Materials:
  - Verify roughness, metallic, Fresnel, emissive, AO, and IBL parity on existing glTF reference scenes.

## Current Session Notes
- Baseline runtime captures were requested by plan, but this session has not yet automated GUI scene playback or screenshot capture.
- Shared pass-quality controls have been added to `RenderContext` and the rendering settings UI to support renderer triage integration.
- Wave 1 renderer integration now compiles with half/quarter-resolution SSGI and half-resolution screen-space shadow plumbing, plus shared temporal/resolution controls exposed in the UI.
- Wave 1 physics integration now compiles with deterministic pair/manifold ordering, narrower wake-up rules, gizmo transform-authority fixes, and expanded contact debug output.
- `TransformComponent` now carries `TransformID`, `transformGeneration`, and `prevWorldTransform`; `RenderSystem` uploads a global transform SSBO and feeds `prevModel` plus `uTransformID` into draw shaders.
- The G-buffer now includes a dedicated `R32UI` TransformID attachment and the debug-mode UI exposes both `Material ID` and `Transform ID` inspection paths.
- Shadow backend policy is partially centralized in `LightManager::ShadowConfig` and `ShadowPass`, including cascade count, split lambda, stable texel snapping, and default rotated-PCF intent.
- CPU-side PBR ownership is now normalized around `MaterialDesc` in mesh/import/runtime/RT code, with legacy per-mesh fields mirrored for compatibility instead of acting as the primary source of truth.
- Shader-side BRDF convergence is now live across deferred, forward-transparent, and RT paths via shared helpers in `shaders/includes/pbr_common.glsl` and `shaders/includes/material_common.glsl`.
- Deferred lighting now exposes dedicated shadow debug visualizations for cascade index, raw cascade depth, bias heatmap, texel density/coverage, and final shadow mask through the rendering settings UI.
- Physics narrowphase follow-through now includes EPA-backed fallback/witness generation for degenerate convex cases in box-box and sphere-box interactions.
- Verification completed so far:
  - `cmake --build build/vs2022-x64 --config Debug --target Nox_Engine` succeeded on April 13, 2026 after renderer and shadow integration fixes.
  - A full CMake regenerate also succeeded on April 13, 2026 after resolving merge markers in `CMakeLists.txt`; the generated VS project now rebuilds cleanly from configuration through link.
  - Physics worker reported direct compilation success for the owned physics slice before the full-engine build was repaired.
  - `cmake --build build/vs2022-x64 --config Debug --target Nox_Engine` succeeded again on April 13, 2026 after merging shader BRDF convergence and shadow-debug visualization plumbing.
- Remaining major workstreams from the directive:
  - TransformID propagation into more GPU consumers beyond the current geometry/velocity groundwork, including future surfel/indirect paths.
  - Removing the remaining spec-gloss compatibility shim once legacy import/runtime paths are fully retired.
  - Extending transparent transmission beyond the current thin-surface refraction model into thickness-aware absorption/transmittance.
  - Pushing physics from the current hybrid SAT + GJK/EPA fallback into a fully generic convex-manifold pipeline with richer debug views.
