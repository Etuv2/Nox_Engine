# HBAO+ ambient occlusion

`HBAOPass` (`src/passes/HBAOPass.*`) replaces the old random-hemisphere SSAO. It follows the
HBAO+ structure (Bavoil & Sainz, "Image-Space Horizon-Based Ambient Occlusion"; NVIDIA HBAO+ /
nvpro-samples `hbao`). It is enabled by default and runs entirely as compute passes through `rhi::Gpu()`.

## Pipeline

| Step | Shader | Work | Output |
|---|---|---|---|
| 1 | `shaders/hbao_deinterleave_comp.glsl` | one thread per quarter-res texel; linearizes the full-res gbuffer depth and writes its 16 pixels of a 4x4 block into 16 layers | `R32F` Tex2DArray, view depth (0 = sky) |
| 2 | `shaders/hbao_compute_comp.glsl` | dispatch (quarter tiles, 16 layers); `ssao_directions` x `ssao_steps` (default 8 x 4); horizon angle per direction; falloff `1 - d^2/R^2`; angle bias; gbuffer normals | `R16F` Tex2DArray, visibility |
| 3 | `shaders/hbao_reinterleave_comp.glsl` | back to the output resolution; packs view depth next to the AO | `RG16F` (ao, depth) |
| 4 | `shaders/hbao_blur_comp.glsl` | separable Gaussian x depth-edge weight `exp(-sharpness * relDepth)`, X then Y | `RG16F`, `.r` = AO |

The per-layer jitter is a fixed 4x4 table (rotation and step offset per layer), so there is no
noise texture and the 16 layers decorrelate the pattern before the blur.

`GetSSAOTexture()` returns the final texture. The lighting pass reads it exactly as before
(`ssaoMap`, `.r`, mixed by `ssao_intensity`). `ResourceNames::SSAO` and `ctx.enableSSAO` keep their names.

## Settings

| Key (scene `render_settings`) | RenderContext field | Default | Meaning |
|---|---|---|---|
| `ssao` | `enableSSAO` | `true` | master switch |
| `ssao_radius` | `ssaoRadius` | 0.75 | world-space horizon search radius (m); projected to pixels, clamped to 256 full-res px |
| `ssao_bias` | `ssaoBias` | 0.1 | tangent angle bias in radians; horizons below it are ignored (self-occlusion) |
| `ssao_power` | `ssaoPower` | 2.0 | exponent on the final visibility |
| `ssao_strength` | `ssaoStrength` | 1.0 | multiplier on accumulated occlusion before the power |
| `ssao_intensity` | `ssaoIntensity` | 0.5 | lighting blend (unchanged meaning) |
| `ssao_blur_radius` | `ssaoBlurRadius` | 4 | blur half-width in output px |
| `ssao_blur_sharpness` | `ssaoBlurSharpness` | 40 | depth-edge sharpness of the blur |
| `ssao_resolution_scale` | `ssaoResolutionScale` | 1.0 | output AO size (the horizon search always runs at quarter res) |
| `ssao_directions` | `ssaoDirections` | 8 | horizon directions per pixel, 4..16 (quality; cost is linear) |
| `ssao_steps` | `ssaoSteps` | 4 | samples per direction, 2..8 (quality) |

Old scene files that only set `ssao` and `ssao_intensity` load unchanged. The `ssao_bias` key now
means an angle, not the old view-space depth offset; the old field was never serialized.

Quality tiers in `RenderingSettingsWindow::ApplyQualityPreset`: every tier enables AO. Low uses the
half-resolution output (`ssao_resolution_scale` 0.5); the others use full resolution.

## Inspecting

`debug_view` value `ao` (Rendering settings > "Inspect image" > "Ambient occlusion") shows the AO
texture as grayscale with grain and dither disabled, so banding and halos are visible.

## Cost

Measure with `NOX_LOG_PASS_METRICS=1` (pass name `HBAOPass`). The target is under 1 ms at 1920x1080.
