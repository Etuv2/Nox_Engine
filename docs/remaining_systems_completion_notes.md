# Remaining Systems Completion Notes

## Material Finalisation
- Removed the runtime spec-gloss compatibility path and normalized the CPU-side material contract around metallic-roughness plus supported glTF extensions.
- Added canonical `emissiveStrength`, `clearcoatFactor`, and `clearcoatRoughnessFactor` to `MaterialDesc` and mirrored mesh state.
- Extended the G-buffer with a dedicated clearcoat attachment so deferred and RT paths can consume the same layered material data.

## TransformID Consumer
- Added a `TransformHistoryPass` compute stage after the G-buffer pass.
- The pass consumes `gTransformID` and the global transform SSBO, then writes:
  - a persistent per-`TransformID` history SSBO at binding `7`
  - a visible transform ID list SSBO at binding `8`
- This establishes a concrete GPU-side contract for future surfel, GI, and cache systems.

## Transmission
- Added volume-style transmission inputs to the canonical material contract: `thicknessFactor`, `attenuationDistance`, and `attenuationColor`.
- Forward transparent shading now applies Beer-Lambert style transmittance instead of a pure thin-surface tint heuristic.

## Validation
- `cmake --build build/vs2022-x64 --config Debug --target Nox_Engine` succeeded on April 13, 2026 after integrating the material/G-buffer/TransformID changes.
- Runtime visual validation is still pending for thin-vs-thick transmission scenes and the new TransformID history pass.
