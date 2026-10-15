# Surfel GI audit against Frostbite GIBS (SIGGRAPH Advances 2021)

Scope: `src/passes/SurfelGIManager.*`, `src/passes/SurfelGIPipeline.*`, `src/surfel_gi/SurfelGIResources.*`,
`src/ComputeShader.*`, `shaders/surfel_gi/*`. Reference: the Frostbite paper in
`docs/SIGGRAPH Advances 2021 - Surfel GI.pdf` and `docs/surfel_gi_gibs_implementation_spec.md`.

The paper is a slide-deck transcript. It gives no equations for the MSME, the spawn threshold, the ray budget
formula, the overflow behaviour or the final-gather resolution. Where this document says "not in the paper", the
choice is ours and is marked as such.

Status column: **Fixed** = changed to match GIBS or the stated intent; **Partial** = still differs; **Matches** =
no change needed; **Choice** = not specified by the paper, our decision.

## Component table

| # | Frostbite component | Implementation now | Status | Location |
|---|---|---|---|---|
| 1 | Spawning: lowest-coverage pixel per 16x16 tile, probability from coverage, per-frame budget | One 64-invocation workgroup per 16x16 tile. Coverage comes from the gather pass: `apply_indirect` stores its gather weight sum (clamped to 1) to a coverage texture at the final-gather resolution, and the spawn pass reads it one frame later instead of running a grid query per pixel. Each invocation reads validity and coverage for four pixel slots; a shared atomic minimum picks the lowest. Up to four spawns per tile per frame (later rounds mask a 5x5 neighbourhood). Budget = tiles x (0.004 + 0.06 x deficit) x scale (default 0.25), clamped to [16, tiles/8]. | Fixed (coverage by-product, one-frame lag); Choice (round count, mask radius, budget constants and scale) | `shaders/surfel_gi/spawn.comp`, `shaders/surfel_gi/apply_indirect.comp` (coverage store), `SurfelGIPipeline.cpp` (coverage binding, budget uniform) |
| 2 | Coverage from G-buffer vs. surfel projection | `coverage.comp` projects live surfels into tiles and tests depth, normal, radius. | Matches | `shaders/surfel_gi/coverage.comp` |
| 3 | Recycling by not-seen time, over-coverage, distance | Over-coverage, stale, pressure, oversize, duplicate, outside grid, too far. Probabilistic culls use a per-surfel random number. | Partial | `shaders/surfel_gi/recycle_surfels.comp` |
| 4 | Camera-centred acceleration; surfel inserted into every overlapping cell | Non-linear grid. Full cells now keep the surfels nearest the cell centre: each entry stores its squared distance in the otherwise unused `next` field, and an arriving surfel probes 8 entries and replaces the farthest probe if it is nearer. The overflow counter still counts every event. The probe is approximate: the nearest surfels usually survive, not guaranteed. | Fixed (approximate nearest-keeping) | `shaders/surfel_gi/build_grid.comp` (`InsertIntoCell`) |
| 5 | Ray budget by variance / age / contribution | Requests combine variance, age, visibility and confidence, plus a coefficient-of-variation term (`round(2 x std/mean)`, clamped to 1..8 rays). Surfels younger than four frames request up to 12 rays (was 4). The allocation cap (1024 rays/frame) still binds. | Fixed (variance term, young surfels) | `shaders/surfel_gi/request_rays.comp` |
| 6 | Ray cosine / guided sampling | Cosine pdf mixed with guide-map pdf. | Matches | `shaders/surfel_gi/generate_rays.comp` |
| 7 | Ray length | The BVH path traces to 256 m (scene range), not the 4-12 m bounce length. The grid/screen fallback still uses the capped length. | Fixed (BVH rays) | `shaders/surfel_gi/trace_rays.comp` |
| 8 | Trace against scene | BVH is exact. A ray that passes the local cap no longer becomes a sky miss. | Fixed (sky leak) | `shaders/surfel_gi/trace_rays.comp` |
| 9 | Hit shading: direct + cached irradiance at hit (multi-bounce), sky on miss | `albedo/pi x (direct + cached E)`. Cosine-pdf samples give `L cos/pdf = L pi`, so E is the cosine-weighted hemisphere integral. Checked. | Matches | `shaders/surfel_gi/integrate.comp`, `trace_rays.comp` |
| 10 | Radial depth (Chebyshev / moments) | Misses are recorded and hits beyond `radius*8` are no longer dropped. Only the first 8 rays per surfel feed the update. | Fixed (over-occlusion); 8 rays | `shaders/surfel_gi/radial_depth_update.comp` |
| 11 | Temporal irradiance: MSME | `SurfelMSME` already follows the Ray Tracing Gems `MultiscaleMeanEstimator` structure (short mean and variance, inconsistency, variance-based blend reduction, catch-up blend). The paper gives no equations, so the reference is RTG. `integrate.comp` still adds heuristic branches (sparse-variance injection, lighting catch-up, bootstrap confidence). Response to a lighting change was not measured. | Partial (RTG estimator; heuristics kept; response not measured) | `shaders/surfel_gi/msme.comp`, `integrate.comp` |
| 12 | Energy normalisation | Cosine-pdf estimator, albedo applied once for the receiver in lighting (`giResponse` in `deferred_lighting_frag.glsl`). Checked. | Matches | `integrate.comp`, `deferred_lighting_frag.glsl` |
| 13 | Surfel-to-pixel gather | Weights from distance, normal, plane and radial depth (as before). The per-cell stride start now includes the frame index, so the strided subset rotates and the temporal filter averages the whole cell instead of a fixed subset. Budget unchanged (32 accepted samples per pixel). | Fixed (fixed-subset bias) | `shaders/surfel_gi/apply_indirect.comp` (`ReceiverGatherSeed`, `AccumulateCell`) |
| 14 | Final gather resolution and upsample | Medium tier gathers at 0.32 of output resolution, then spatial, temporal and upscale filters. The paper gives no gather resolution. Measured: going to 1.0 changes the indirect energy on lit pixels by +3.3% and adds ~1.17 ms, so 0.32 is a resolution trade-off, not an energy term. Kept as the named Medium-tier value. | Choice (named, energy-neutral within 3.3%) | `src/ModularRenderer.cpp` (tier caps), `SurfelGIPipeline.cpp` (`ApplyIndirect`) |
| 15 | Irradiance sharing | `irradiance_sharing.comp` (not GIBS). | Extra | `shaders/surfel_gi/irradiance_sharing.comp` |
| 16 | Indirect dispatch for surfel-wide passes | `dispatch_args.comp` (one thread) turns the high-water index from `count_live_surfels` (highest live index + 1) plus a spawn slack into group counts (`ceil(bound/64)`) in a dedicated 12-byte buffer (binding 35). A `BarrierCommand` follows it. `update`, `integrate` and `radial_depth` dispatch indirectly. | Fixed (earlier 20x slowdown came from keeping the args inside the counters buffer) | `shaders/surfel_gi/dispatch_args.comp`, `count_live_surfels.comp`, `SurfelGIPipeline.cpp` (`DispatchSurfelWide`) |
| 18 | Gather contribution by surfel age | Each surfel's contribution to the irradiance average ramps in over its first 8 frames and with its MSME sample count (`shortMean.a`), floor 0.25. The ramp only reweights the average; coverage (`sumWeight`) is not ramped, so spawning does not depend on it. | Fixed (young-surfel weighting); tested stronger ramp (floor 0.05, 12 frames): noise did not improve | `shaders/surfel_gi/apply_indirect.comp` (`SurfelContributionRamp`) |
| 17 | Counter readback | No synchronous readback in the frame. Counters are copied on the GPU into a two-slot staging ring and mapped `kGpuTimingReadbackFrameDelay` (3) frames later. Only the placement-validation tool still reads synchronously. | Fixed | `SurfelGIPipeline.cpp` (`IssueCounterReadback`, `PumpCounterReadback`) |

## Changes in this pass

1. Spawning reads a coverage texture written by the gather pass (one frame old), instead of a grid query per pixel (row 1).
   The lane path evaluates validity and reads the map; the winning pixel keeps its full evaluation for the new surfel's fields.
2. Gather contributions ramp in for young surfels (row 18). Young surfels request up to 12 rays for their first four frames (row 5).
3. Spawn budget scale default lowered to 0.25 (`SurfelGIManager.h`): the default budget over-spawned on the coverage map
   and roughly doubled the population.
4. Lane evaluation skips the spawn radius and importance terms when the map is used.
5. Earlier passes (rows 4, 7, 8, 10, 13, 16, 17) are unchanged.

## Tried and not kept

- **Gentler recycling** (over-coverage cull from 48 to 96 frames of age and 2-22% down to 1-8% per frame). Reverted: the
  population still grew to 22k on Cornell, so this was not the cause. Left for a later pass with a proper age and not-seen model.
- **Stronger young-surfel ramp** (floor 0.05, 12 frames): scene.json temporal std rose from 0.378 to 0.406. Not kept.
- **Static gather stride seed** (fixed per-pixel subset, the pre-earlier-pass behaviour): scene.json temporal std rose from 0.378 to 0.417.
  The per-frame subset is kept; it reduces noise.
- **Eight young-surfel rays** instead of twelve: scene.json temporal std 0.414 (no gain), tracing +0.03 ms. Kept twelve.
- **Spawn budget scale 0.5 and 1.0** with the map: population 15k to 23k on Cornell, surfel-only brightness and noise worse.
- **Coverage gain 3x** (map scaled before clamp): halved the population (23k to 13k) but still above baseline. Not kept; the budget
  scale does the same job more directly.
- **Earlier**: 8 spawn candidates per tile, spawn stride 2 or 4 (stride costs population), the thread-0 coverage-only shortcut,
  the 256-invocation tile workgroup, and the tree reduction per round were tried in the earlier pass and superseded.

## Results (current build, `main` at 1296919 plus SSGI merge)

Method: the main-checkout exe (baseline) and the worktree build, `--preset game`, `NOX_FIXED_DT=1/60`, static camera,
image statistics on frames 200-240, timings as subpass medians. The GI-off reference is the worktree build with SSGI and surfels
off. Noise is the temporal standard deviation of the indirect image (both-path minus GI-off) over frames 200-240, on lit pixels.
The lit mask is the same for baseline and current in each scene, so the absolute numbers compare directly; relative noise
(std / mean indirect) is given because the signal differs.

### Cornell validation scene

| | Baseline (main) | Now |
|---|---|---|
| Spawn pass | 0.815 ms | 0.481 ms |
| Sum of listed surfel passes | 2.460 ms | 2.100 ms |
| Direct gather pixels per frame | 73,424-73,431 | 73,440 |
| Live surfels at frame 240 | 5,992 | 5,138 |
| Indirect light, GI on minus GI off (lit px) | 31.8 | 15.7 |
| Temporal std of indirect, lit px (absolute) | 0.801 | 0.649 |
| Temporal std / mean indirect (relative) | 2.5% | 4.1% |
| Surfel-only indirect, lit px | not captured | 36.1 (std 0.947) |

Energy: SSGI only 4.0, surfel only 36.1 on lit pixels (ratio 9.2). The combined path is below surfel only because SSGI's
visibility multiplies the far-field term (see Remaining, item 2).

### scene.json (sky and open geometry)

| | Baseline (main) | Now |
|---|---|---|
| Spawn pass | 1.877 ms | 1.625 ms |
| Sum of listed surfel passes | 6.049 ms | 6.013 ms |
| Direct gather pixels per frame | 484,922-485,014 | 485,044-485,053 |
| Live surfels at frame 240 | 5,929 | 3,222 |
| Indirect light, GI on minus GI off (lit px) | 5.8 | 22.9 |
| Temporal std of indirect, lit px (absolute) | 0.273 | 0.418 |
| Temporal std / mean indirect (relative) | 4.7% | 1.8% |
| Surfel-only indirect, lit px | not captured | 23.9 (std 0.425) |

Whole-frame means and the four-way images were checked by eye; on this scene SSGI alone does not change the frame and the
surfels light the ceiling and right-hand walls through the openings.

### Targets

- Spawn at or below baseline: met on both scenes (0.48 vs 0.82 ms; 1.63 vs 1.88 ms).
- Total surfel time at or below baseline: met on both scenes (2.10 vs 2.46 ms; 6.01 vs 6.05 ms).
- Coverage kept: met on both scenes (direct gather pixels at or above baseline).
- Off-screen contribution kept: met on scene.json (3.9x baseline indirect light); on Cornell the indirect light is about
  half the baseline, because the lower budget keeps fewer surfels there.
- Temporal std at or below baseline: met on Cornell in absolute terms (0.65 vs 0.80), not met on scene.json (0.42 vs 0.27)
  in absolute terms. Relative noise is lower on scene.json (1.8% vs 4.7%) and higher on Cornell (4.1% vs 2.5%).

### Images viewed

- `r4_cornell_four.png` (GI off, SSGI only, surfel only, both): surfels light the walls with colour bleeding; the combined view
  is darker on the walls than surfels alone (SSGI visibility composition, unchanged by design).
- `r4_scene_four.png` (same order): surfels add light to the ceiling and right-hand walls seen through the openings; SSGI alone
  does not change the frame visibly.

### Validation scripts

- `tools/CheckNoRawGL.ps1`: passes.
- `scripts/validate_surfel_gi_static.py` and `scripts/validate_surfel_gi_correctness.py`: fail with the same assertions on
  `main`, so they predate this work.
- Shader compilation: checked at runtime through the logs (no `compile failed` lines).

## Remaining (not changed)

1. **Scene.json absolute noise.** Still 0.42 against 0.27 for baseline. The signal is 3.9x larger, so relative noise is lower.
   Open: whether the baseline's lower brightness is itself the under-converged state.
2. **Cornell indirect light is about half the baseline.** The lower spawn budget leaves fewer surfels; direct coverage held.
   Needs a decision between population density and the spawn budget.
3. **SSGI and the surfel far field.** `diffuseAO = min(..., nearVisibility)` multiplies the surfel term by SSGI's visibility.
   The composition is intended to avoid double counting and is left as is.
4. **MSME response.** The estimator is the RTG structure; its response to a lighting change was not measured.
5. **Overflow and the ray term.** Implemented; neither test scene overflows.
6. **Gather budget (row 13).** 32 accepted samples per pixel; the early exit at weight 64 still applies.
7. **Sky visibility per hit (row 9).** One extra BVH trace per hit.
8. **Radial depth rays (row 10).** 8 rays per surfel.
