# Transmission Completion Audit

## Current State
- `src/passes/TransparentForwardPass.cpp` draws back to front with dual-source blending (`glBlendFunc(GL_ONE, GL_SRC1_COLOR)`), lit by the same light loop and shadows as the deferred pass (`shaders/includes/lighting_common.glsl`).
- `shaders/forward_transparent_frag.glsl` now composites with dual-source blending: reflections are added at full strength and the actual scene behind the surface is transmitted, tinted per channel by base color and Beer-Lambert absorption (`ComputeVolumeTransmittance`). It still assumes thin-surface behavior along the view ray: there is no refraction offset (no screen-space refraction) and thickness is a scalar.
- `shaders/includes/pbr_common.glsl` provides the transmission weight (`ComputeTransmissionWeight`) and a shared medium model (`ComputeVolumeTransmittance`: base-color tint plus Beer-Lambert absorption over `thicknessFactor`), used by the forward and RT paths.
- `shaders/includes/material_common.glsl` only unpacks the canonical opaque/transmissive runtime fields currently available to the renderer and does not yet carry a volume/thickness contract.

## Safe Prep Made
- Added this audit note only. No runtime, shader, or material-contract code has been changed yet.

## Expected File Scope For The Real Implementation
- `src/passes/TransparentForwardPass.cpp`
- `src/passes/TransparentForwardPass.h`
- `shaders/forward_transparent_frag.glsl`
- `shaders/includes/pbr_common.glsl`
- `shaders/includes/material_common.glsl` if the finalized material contract requires new unpacked fields or shared medium helpers
- `src/Scene.cpp` only if the finalized CPU material handoff needs new uniform uploads or texture bindings
- `src/MeshComponent.h` only if the finalized material contract adds transmission-volume fields to `MaterialDesc`
- `scenes/mat_test.json` or a new scene note/fixture for thin-vs-thick validation

## Material Finalisation Handoff Needed
- Finalized CPU-side `MaterialDesc` names and defaults for:
  - `transmissionFactor`
  - `ior`
  - `thicknessFactor` or equivalent thickness field
  - `attenuationColor` or equivalent absorption tint
  - `attenuationDistance` or equivalent absorption scale
- Confirmation that transparent rendering should continue to use the shared canonical BRDF for reflection/specular while applying a separate medium-transmission term for the transmitted path.
- Confirmation of any new texture slots or uniforms for thickness/volume data, or explicit instruction that thickness is scalar-only.
- Confirmation that the spec-gloss shim is gone from the final contract before this pass removes any remaining fallback references.

## Validation Scene Note
- Reuse `scenes/mat_test.json` as the base validation scene if the existing material test asset already contains transmissive candidates.
- If that asset is insufficient, add a dedicated thin-vs-thick glass fixture with:
  - one thin transmissive surface,
  - one thick volume-backed glass object,
  - one colored absorption sample,
  - one IOR comparison sample,
  - one opacity/mask edge case.

