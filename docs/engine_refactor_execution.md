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
