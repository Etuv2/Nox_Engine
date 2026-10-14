# Surfel GI audit against Frostbite GIBS (SIGGRAPH Advances 2021)

Scope: `src/passes/SurfelGIManager.*`, `src/passes/SurfelGIPipeline.cpp`, `src/surfel_gi/SurfelGIResources.*`,
`shaders/surfel_gi/*`, `shaders/includes/surfel_gi_resources.glsl`. Reference: the Frostbite paper in
`docs/SIGGRAPH Advances 2021 - Surfel GI.pdf` and `docs/surfel_gi_gibs_implementation_spec.md`.
The audit baseline is commit `9d0ac15`. Status column: "Fixed" means changed in this pass, "Partial" means
still differs from GIBS, "Matches" means no change was needed.

## Summary

The energy estimator and the overall pass structure follow GIBS. The gaps that decide quality were hit and miss
semantics during tracing (ray length cap, sky leak), radial depth learning (over-occlusion in open directions),
spawn placement (candidates are a few jittered pixels, not the tile's lowest-coverage pixel), and gather sampling
(a fixed strided subset of each cell). This pass fixed the first two and the gather sampling bias. Spawn placement,
MSME heuristics and the sync readback remain (see "Remaining").

## Component table

| # | Frostbite component | Implementation | Status | Location |
|---|---|---|---|---|
| 1 | Screen-space spawning: one lowest-coverage pixel per tile, spawn probability from coverage | Tiles of `spawnTileSize` (Medium/High/Ultra force >= 8). Each tile tests 4 jittered candidates per pass (8 in fast fill), keeps the lowest `coverage - 0.12*importance - priority`. Not the lowest-coverage pixel of the tile. Spawn probability from `TilePlacementProbability` and a blue-noise test. Per-frame spawns are capped by `maxSpawnsPerFrame` (64). | Partial | `shaders/surfel_gi/spawn.comp` (`main`, candidate loop), `TilePlacementProbability` |
| 2 | Coverage from G-buffer vs. surfel projection | `coverage.comp` projects live surfels into tiles and tests depth, normal, radius. | Matches | `shaders/surfel_gi/coverage.comp` |
| 3 | Recycling: not-seen time, over-coverage, distance | Over-coverage (random 2-22%), stale contribution/sample age, pressure, oversize, duplicate, outside grid, too far from camera. Probabilistic culls use a per-surfel random number. | Partial | `shaders/surfel_gi/recycle_surfels.comp` |
| 4 | Camera-centred acceleration: uniform near grid + non-linear far grid; surfel inserted into every overlapping cell | Non-linear grid in `SurfelWorldToGridAddress`. `build_grid.comp` inserts into the cell and its neighbours. Cells capped at `maxSurfelsPerCell`; overflow dropped silently (counter `overflowGridEntries` is not used to adapt budget). | Matches, with overflow loss | `shaders/surfel_gi/common.glsl`, `build_grid.comp` |
| 5 | Ray budget by variance / age / contribution | `request_rays.comp`, `allocate_rays.comp`. Budget 1024 rays/frame at the default. Requests reach 17k-38k per frame in the test scene; 1024 are allocated. | Matches in structure | `request_rays.comp`, `allocate_rays.comp` |
| 6 | Ray cosine / guided sampling | Cosine pdf mixed with guide map pdf. | Matches | `shaders/surfel_gi/generate_rays.comp` |
| 7 | Ray length | The BVH path now traces to `SURFEL_TRACE_BVH_FAR_DISTANCE` (256 m, scene range) instead of the local bounce length (4-12 m). The grid/screen fallback and the miss position still use the capped length. | Fixed for BVH rays | `shaders/surfel_gi/trace_rays.comp` (`TraceSoftwareBVHRadiance`), `SurfelGIPipeline.cpp` (ray length clamp) |
| 8 | Trace against scene (HW RT in GIBS; BVH here) | `TraceSoftwareBVHRadiance` is exact, stride 1, so every ray uses the BVH. Before this pass a ray that passed the local cap without a hit was recorded as a sky miss (sky leak into interiors). | Fixed (sky leak) | `shaders/surfel_gi/trace_rays.comp` |
| 9 | Hit shading: direct light + cached irradiance at hit (multi-bounce) + sky on miss | `ShadeHitRadiance` adds direct (shadowed) and cached irradiance, falling back to sky visibility. The sky term costs an extra BVH ray per hit. | Matches; costly | `shaders/surfel_gi/trace_rays.comp` |
| 10 | Radial depth (Chebyshev / moments, prevents leaking) | `SurfelRadialDepthVisibility` is a correct one-sided Chebyshev test. The update used to ignore misses and drop hits beyond `radius*8`, so open directions stayed at the default depth and occluded receivers that could see. Still only the first 8 rays per surfel feed the update. | Fixed (misses recorded, no far cut); ray count still 8 | `shaders/surfel_gi/radial_depth_update.comp` |
| 11 | Temporal irradiance: MSME (short/long means, variance, vbbr) | `SurfelMSME` follows the Ray Tracing Gems MSME. `integrate.comp` adds heuristic branches (sparse-variance injection, lighting catch-up, bootstrap confidence) that are not in MSME. | Partial: heuristic | `shaders/surfel_gi/integrate.comp`, `msme.comp` |
| 12 | Energy normalisation (Monte Carlo estimator for E = integral L cos) | `sampleIrradiance = L * cos / pdf` with cosine pdf gives an unbiased E. | Matches | `integrate.comp`, `generate_rays.comp` |
| 13 | Surfel-to-pixel gather: grid cells, weights from normal, distance, radial depth | Weights match GIBS. Each cell is strided-sampled with a start index from a per-pixel hash. That hash ignored the frame, so each pixel saw the same subset of every cell forever. The frame index is now in the seed, so the subset rotates and the temporal filter averages over the whole cell. The budget (32 accepted samples per pixel) is unchanged. | Partial: bias fixed, budget unchanged | `shaders/surfel_gi/apply_indirect.comp` (`ReceiverGatherSeed`, `AccumulateCell`) |
| 14 | Screen-space integration / upscale | Gather at `finalGatherResolutionScale` 0.32 (Medium), then spatial, temporal and upscale filters. | Partial | `SurfelGIPipeline.cpp` (`ApplyIndirect`), `upscale_filter.comp` |
| 15 | Irradiance sharing (non-GIBS) | `irradiance_sharing.comp` shares irradiance between neighbouring surfels. It is an addition. | Extra | `shaders/surfel_gi/irradiance_sharing.comp` |

## Changes in this pass

1. BVH rays trace to the full scene range (row 7-8). Removes the sky leak from the local length cap.
2. Radial depth update records misses and no longer discards hits beyond `radius*8` (row 10).
3. Gather stride seed includes the frame index (row 13).
4. `count_live_surfels` runs only on frames that read the counters or show debug views (carried over from an earlier edit, no effect on output).

## Tried and reverted

- **Indirect dispatch sized by live surfels** (`glDispatchComputeIndirect`, high-water index from the count pass, args written by `begin_frame`). Measured on the test scene: `transform_update` median 0.034 ms -> 0.648 ms and `temporal_integration` 0.019 ms -> 0.665 ms, and the output changed (viewport mean luminance 13.8 -> 14.7, temporal std 0.348 -> 0.381), so the bound skipped or mis-sized work. The gain was at most ~0.05 ms at this live count (about 5k of 65k). Not shipped. The patch is not kept in the repo.
- **8 spawn candidates in steady state** (instead of 4). Spawn pass 0.245 ms -> 0.478 ms on spawn frames (one sample). Under-covered tiles at frame 61: 269 of 2776 visible (9.7%) -> 229 of 2271 (10.1%). Spawns were capped at 64/frame in both, so the extra candidates did not change coverage in this scene. Not shipped.

## Results (`scenes/surfel_gi_cornell_validation.json`)

Method: HEAD (`9d0ac15`) and the working tree built in the same worktree, same window size (1208x643 engine,
1920x1080 backbuffer dump), `NOX_FIXED_DT=1/60`, static camera. Captures: frames 200-240 for temporal metrics,
31 subpass samples for timing (`NOX_SURFEL_GI_LOG_SUBPASS_METRICS_INTERVAL=10`). Numbers are medians.

- Surfel GI GPU time (sum of per-pass medians): HEAD 1.46 ms, now 1.45 ms. Per pass: tracing 0.218 -> 0.219,
  final gather 0.168 -> 0.169, radial depth 0.026 -> 0.025, spawn 0.245 -> 0.244 (spawn frames only).
- Viewport mean luminance (frame 200): GI off 10.39, GI on HEAD 13.94, GI on now 14.35.
- GI contributes: GI on vs off differ by mean abs 3.96 (of 255), 15% of pixels differ by more than 8 levels. The
  back wall, ceiling and side walls are dark without GI and lit with it (see `NOX_SURFEL_GI_DEBUG_VIEW=12`).
- Temporal stability (viewport, frames 200-240): mean |frame_t - frame_t-1| 0.2641 -> 0.2662; mean temporal std
  0.354 -> 0.357. No measurable flicker change; the gather bias fix is not shown by this metric.
- The sky-leak fix (row 8) cannot show in this scene because the skybox is disabled, so there is no sky radiance to leak.

Note: the pre-built `build/vs2022-x64/Release/Nox_Engine.exe` in the main checkout is older than `9d0ac15`
(built 2026-10-13 23:45, HEAD committed 2026-10-14 06:40), so it was not used as a baseline.

## Remaining (not changed)

1. **Spawn placement (row 1).** Not the tile's lowest-coverage pixel. Spawns are capped at 64/frame, and when the cap
   binds the tile that wins is set by workgroup order, not by coverage.
2. **Gather budget (row 13).** 32 accepted samples per pixel; the early exit at weight 64 still applies.
3. **Low-resolution final gather (row 14).** 0.32 scale on Medium softens colour bleeding.
4. **Heuristic MSME (row 11).** Constants are tuned, not derived.
5. **Overflow in cells (row 4).** Overflowing surfels are dropped; counters exist but do not feed the budget.
6. **Sky visibility per hit (row 9).** One extra BVH trace per hit.
7. **Counter readback (CPU stall).** `ReadSurfelCounters` is a synchronous `ReadBuffer` every 30 frames.
8. **Ray count per surfel (row 10).** Radial depth uses 8 rays per surfel.
9. **Indirect dispatch.** Not shipped (see above). Needs a dedicated args buffer and a driver check before retrying.
