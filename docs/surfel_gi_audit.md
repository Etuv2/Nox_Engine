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
| 1 | Spawning: lowest-coverage pixel per 16x16 tile, probability from coverage, per-frame budget | One 64-invocation workgroup per 16x16 tile. Each invocation evaluates four pixel slots (coverage only). A shared-memory atomic minimum over the coverage bit patterns, then the lowest lane holding it, picks the pixel. Up to four spawns per tile per frame: later rounds take the next-lowest pixel (masking a 5x5 neighbourhood) and only spawn where coverage is under half the threshold. Budget = tiles x (0.004 + 0.06 x deficit), clamped to [16, tiles/8], where deficit = last frame's under-covered / visible tiles. The old 64/frame cap is gone; `maxSpawnsPerFrame` is now only a ceiling. | Fixed (pixel choice, probability and budget); Choice (rounds, mask radius, budget constants) | `shaders/surfel_gi/spawn.comp` (`main`, `ComputeSpawnBudget`), `SurfelGIPipeline.cpp` (spawn dispatch) |
| 2 | Coverage from G-buffer vs. surfel projection | `coverage.comp` projects live surfels into tiles and tests depth, normal, radius. | Matches | `shaders/surfel_gi/coverage.comp` |
| 3 | Recycling by not-seen time, over-coverage, distance | Over-coverage, stale, pressure, oversize, duplicate, outside grid, too far. Probabilistic culls use a per-surfel random number. | Partial | `shaders/surfel_gi/recycle_surfels.comp` |
| 4 | Camera-centred acceleration; surfel inserted into every overlapping cell | Non-linear grid. Full cells now keep the surfels nearest the cell centre: each entry stores its squared distance in the otherwise unused `next` field, and an arriving surfel probes 8 entries and replaces the farthest probe if it is nearer. The overflow counter still counts every event. The probe is approximate: the nearest surfels usually survive, not guaranteed. | Fixed (approximate nearest-keeping) | `shaders/surfel_gi/build_grid.comp` (`InsertIntoCell`) |
| 5 | Ray budget by variance / age / contribution | Requests combine variance, age, visibility and confidence (as before), and now a coefficient-of-variation term: `round(2 x std/mean)` rays, clamped to [1, 8]. New surfels keep a floor of 4. The allocation cap (1024 rays/frame) still binds: requests reach 17k-38k per frame and 1024 are allocated. | Fixed (variance term added); the cap is the real limit | `shaders/surfel_gi/request_rays.comp` |
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
| 17 | Counter readback | No synchronous readback in the frame. Counters are copied on the GPU into a two-slot staging ring and mapped `kGpuTimingReadbackFrameDelay` (3) frames later. Only the placement-validation tool still reads synchronously. | Fixed | `SurfelGIPipeline.cpp` (`IssueCounterReadback`, `PumpCounterReadback`) |

## Changes in this pass

1. Spawning reworked to one workgroup per 16x16 tile, the lowest-coverage pixel per tile by shared atomic minimum,
   up to four spawns per tile per frame, and a resolution- and deficit-scaled budget (rows 1).
2. Asynchronous counter readback and dispatch from a dedicated args buffer (rows 16-17).
3. Overflow keeps the nearest surfels (row 4). Coefficient-of-variation ray term (row 5).
4. Gather stride seed includes the frame index (row 13); earlier fixes for rows 7, 8 and 10 kept.
5. Validation overrides: `NOX_SSGI=0|1`, `NOX_SURFEL_GI_SPAWN_STRIDE=1..4`, `NOX_SURFEL_GI_FINAL_GATHER_SCALE`.

## Tried and not kept

- **8 spawn candidates per tile** instead of 4 (earlier pass): no coverage gain while the cap bound. Superseded by row 1.
- **Spawn cap at 64 per frame**: the settings window and render context still set 64 after the budget rework, so the GPU budget was capped at 64 until this pass raised all four places to the 65,536 ceiling.
- **Spawn search stride 2 or 4** (evaluate a rotating subset of each tile per frame): cuts about 15% of the live
  population for about 2% of spawn time, with coverage unchanged. Not the default. Available as
  `NOX_SURFEL_GI_SPAWN_STRIDE`.
- **Thread-0 coverage-only evaluation** for the winning pixel: changed the spawn count, so the timing result was not
  clean; not kept.
- **256-invocation tile workgroup** (first version of row 1): 0.71 ms spawn against 0.55 ms with 64 invocations for
  the same coverage. Replaced.
- **Tree reduction per round**: replaced by the shared atomic minimum (fewer barriers).

## Results

Method: the main-checkout exe (baseline, `main`) and the worktree build, both run with `--preset game` (fixed 1920x1080),
`NOX_FIXED_DT=1/60`, static camera. Image statistics use frames 200-240; timing uses the subpass samples
(`NOX_SURFEL_GI_LOG_SUBPASS_METRICS_INTERVAL=10`, medians). The GI-off reference is the worktree build with SSGI and
surfels both disabled (`NOX_SSGI=0 NOX_DISABLE_SURFEL_GI=1`). Cornell is `scenes/surfel_gi_cornell_validation.json`
(no skybox). The second scene is `scenes/scene.json` (test room with a skybox seen through openings).

Sponza (`scenes/sponza.json`) was tried and not used: its default camera sits outside the palace geometry, and a
temporary repo-relative copy with the car removed still showed only sky and a plane. Not resolved here.

### Cornell validation scene

| | Baseline (main) | This pass |
|---|---|---|
| Spawn pass | 0.457 ms | 0.529 ms |
| Sum of listed surfel passes | 1.869 ms | 1.921 ms (+2.8%) |
| Direct gather pixels per frame | 41,381-41,383 | 41,384-41,397 |
| Live surfels at frame 240 | 5,978 | 4,420 |
| Indirect light, GI-on minus GI-off (lit px) | 22.2 | 18.6 |
| Temporal std of indirect, lit px, both paths | 0.631 | 0.588 |
| Temporal std of indirect, lit px, surfel only | 0.779 | 0.997 |

Frame means at frame 200: GI off 5.07, SSGI only 5.83, surfel only 12.42, both 8.15 (baseline both 8.89).

Energy (lit pixels, indirect): SSGI only 4.5, surfel only 42.9, surfel/SSGI = 9.5. **Not within the 20% target.**

### scene.json (sky and open geometry)

| | Baseline (main) | This pass |
|---|---|---|
| Spawn pass | 0.937 ms | 1.982 ms |
| Sum of listed surfel passes | 3.448 ms | 4.534 ms (+31%) |
| Direct gather pixels per frame | 203,331-203,362 | 203,336-203,343 |
| Live surfels at frame 240 | 5,949 | 8,806 |
| Indirect light, GI-on minus GI-off (lit px) | 8.9 | 18.9 |
| Temporal std of indirect, lit px, both paths | 0.312 | 0.439 |
| Temporal std of indirect, lit px, surfel only | not captured | 0.414 |

Frame means at frame 200: GI off 98.16, SSGI only 98.12, surfel only 107.48, both 107.43 (baseline both 100.62).

Energy (lit pixels, indirect): SSGI only 0.19, surfel only 18.8. SSGI adds essentially nothing in this scene; surfels
supply the off-screen light through the openings. Whether 18.8 is the right magnitude has no reference here.

### Spawn budget sweep (`NOX_SURFEL_GI_SPAWN_BUDGET_SCALE`)

| Scale | Cornell spawn / total / direct / live | scene.json spawn / total / direct / live |
|---|---|---|
| 1.0 (default) | 0.529 / 1.921 / 41,384-41,397 / 4,420 | 1.98 / 4.53 / 203,336-203,343 / 8,806 |
| 0.5 | 0.552 / 1.936 / 41,386-41,396 / 2,940 | 2.04 / 4.61 / 203,335-203,342 / 5,011 |
| 0.25 | 0.580 / 1.957 / 41,390-41,392 / 1,800 | 2.01 / 4.56 / 203,330-203,340 / 2,913 |

The budget is not what costs time: a quarter of the budget keeps direct coverage and changes spawn time by less than
0.05 ms on scene.json. The default stays at 1.0.

### Spawn search stride (`NOX_SURFEL_GI_SPAWN_STRIDE`)

Cornell: stride 2 gives spawn 0.523 ms, total 1.909 ms, direct 41,389-41,393, live 3,359 (stride 1: 0.545 / 1.942 / 41,394-41,397 / 3,932 in the same earlier build). scene.json: stride 4 gives spawn 1.87 ms, total 4.39 ms, live 5,584. The stride helps time a little and costs population. The default stays at 1.

### Final gather scale

Medium-tier 0.32 gives final gather 0.287 ms. Resolution 1.0 gives 1.452 ms, and indirect energy on lit pixels rises only
3.3%. The 0.32 value is kept as a named resolution trade-off.

### Images viewed

- `final_cornell_four.png` (GI off, SSGI only, surfel only, both): with GI off the walls are dark; SSGI adds a faint
  glow at the edges; surfels light the walls with colour bleeding; the combined view is darker than surfels alone
  because SSGI's visibility multiplies the far-field term (see Remaining, item 2).
- `final_scene_four.png` (same order): surfels lift the shadowed ceiling and walls seen through the openings; SSGI alone
  does not change the frame visibly. Both-path and surfel-only frames are almost the same.

### Validation scripts

- `tools/CheckNoRawGL.ps1`: passes.
- `scripts/validate_surfel_gi_static.py` and `scripts/validate_surfel_gi_correctness.py`: fail with the same assertions
  on the main checkout, so the failures predate this pass. Not changed here.
- `scripts/validate_shader_compile.py`: Linux-only (`libGL.so.1`). Shader compilation was checked at runtime: no
  `compile failed` lines in any run's log.

## Remaining (not changed)

1. **Spawn cost.** Spawning evaluates the coverage of every pixel of every tile (2M candidates per frame at 1920x1080),
   where the baseline evaluated 4-8 per tile. On Cornell this costs +0.07 ms (+2.8% total); on scene.json it costs
   +1.1 ms (+31%). Budget and stride changes do not close the gap. Next steps: a per-cell coverage summary instead
   of the full neighbourhood query per pixel, or a coarser lane grid with a rotating phase.
2. **Composition of SSGI and the surfel far field.** `diffuseAO = min(..., nearVisibility)` in
   `deferred_lighting_frag.glsl` multiplies the surfel term by SSGI's visibility. In the Cornell box this cuts the
   surfel contribution roughly in half when SSGI is on (surfel only 12.4 against 8.2 for both on the frame mean), and
   the energy comparison against SSGI fails. SSGI's near field (3.04 m radius) is also weak here. This needs an owner
   for the near field, which is a lighting design decision. Not changed in this pass.
3. **Temporal noise got worse.** Surfel-only indirect std rose from 0.78 to 1.00 on Cornell, and both-path std rose from
   0.31 to 0.44 on scene.json. The likely cause is the extra population and the per-tile multi-spawn, whose new surfels
   start with few samples. Not solved here.
4. **MSME response.** The estimator is the RTG structure; its response to a lighting change was not measured.
5. **Overflow and the ray term.** Implemented; neither test scene overflows, so neither was exercised.
6. **Gather budget (row 13).** 32 accepted samples per pixel; the early exit at weight 64 still applies.
7. **Sky visibility per hit (row 9).** One extra BVH trace per hit.
8. **Radial depth rays (row 10).** 8 rays per surfel.
