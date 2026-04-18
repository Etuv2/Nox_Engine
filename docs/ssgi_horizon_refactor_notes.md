# Horizon-Based SSGI Refactor (Deprecated Ray-March Replacement)

## What Was Replaced

The previous SSGI path used a ray-march style gather over a lit history radiance source. That design produced:
- unstable temporal accumulation under camera/object motion,
- edge breakup and leaking at depth/normal discontinuities,
- poor behavior on thin geometry,
- noisy indirect estimates from binary hit logic,
- incorrect diffuse bounce energy handling.

The old implementation is now treated as deprecated and replaced with a new horizon-slice architecture with sector bitmask visibility.

## New Pass Chain

1. `ssgi_downsample2x_comp.glsl` (renamed role):
- builds quarter-resolution linear depth and quarter normal.
- depth texture has mip chain for mip-guided sampling.

2. `ssgi_radiance_comp.glsl`:
- creates a reusable diffuse-emissive radiance source from G-buffer channels.
- avoids direct recursive use of final lit framebuffer as the primary GI source.

3. `ssgi_raymarch_comp.glsl` (renamed role):
- horizon-based azimuthal slice gather around the receiver.
- per-slice visibility is represented with sector bitmasks rather than only dual horizon angles.
- thin occluder behavior modeled by angular sector expansion using thickness.
- stores raw indirect radiance + compact directional basis (dominant direction + anisotropy/confidence).

4. `ssgi_temporal_resolve_comp.glsl`:
- motion-vector reprojection.
- depth/normal/motion confidence tests for disocclusion rejection.
- confidence-reactive blending with explicit fallback to current frame.

5. `ssgi_bilateral_blur_comp.glsl`:
- joint bilateral denoise using quarter linear depth + normals.
- variance/confidence aware weighting for stability without heavy bleeding.

6. `ssgi_final_upsample_comp.glsl`:
- depth/normal-aware upscale from quarter to full resolution.
- applies directional basis response against receiver normal.
- supports debug visualization routing.

## Debug Views

`ssgiDebugMode` provides stage visibility:
- 0: final upscaled indirect
- 1: raw horizons
- 2: sector bitmask coverage
- 3: raw indirect radiance
- 4: directional basis
- 5: temporal history weight
- 6: disocclusion rejection
- 7: denoised indirect
- 8: upscaled output

## Tunables Added

- `ssgiTemporalResponse`
- `ssgiUpscaleSharpness`
- `ssgiSectorCount`
- `ssgiDebugMode`

Existing controls (`radius`, `sampleCount`, `thickness`, `temporalAlpha`, rejection thresholds) remain active.

## Remaining Trade-Offs

- As a screen-space technique, off-screen indirect contributors remain unavailable.
- Radiance source is bounded and conservative to avoid runaway brightness and feedback; very bright multi-bounce scenes may require tuning.
- Thin-geometry behavior is improved by sector bitmasks and thickness sectors, but still constrained by depth buffer fidelity.
