# Surfel GI rework plan

Status: research and measurement only. No engine code was changed. Branch `worktree-agent-a18dcfd50e5d75539`.
Reference paper: `docs/SIGGRAPH Advances 2021 - Surfel GI.pdf` (EA/Frostbite GIBS talk transcript, 210 pages).
Team spec: `docs/surfel_gi_gibs_implementation_spec.md`. Previous audit: `docs/surfel_gi_audit.md`.

## 0. Summary

The default Medium tier is too dark, and it is too slow for what it delivers.

- Energy: with the same scene, the default (1024 rays/frame) gives 46% of a converged reference on the Cornell validation scene, 93% on `scenes/scene.json`, and 64% on the NFS night street (manual exposure). The cause is ray starvation: about 17k rays are requested per frame and 1024 are allocated (about 6%). Going to 4096 rays gives 96%, 99% and 104%.
- Cost: `scenes/scene.json` costs 2.99 ms of surfel GPU time at 1080p on the RTX 3080. About 1.4 ms of that is final gather plus three screen-space filters (spatial, temporal, upscale). About 0.5 ms is recycling and spawn. The grid is built two or three times a frame (about 0.2 ms) and occupies 92 MB at the Medium cap. About 1.1 ms of Execute GPU time is outside every labelled sub-timer.
- SSGI: with SSGI also on, Cornell drops to 66% of surfel-only, because the far field is multiplied by SSGI visibility (`deferred_lighting_frag.glsl:438-439, 509-510`). The composition is a multiplication, not a split of the hemisphere.
- Correctness: `integrate.comp` consumes all allocated ray slots of a surfel, but only 8 are generated for it, so young surfels (up to 12 slots) read stale hits from other surfels. Several knobs and buffers are dead (section 4, RC11).
- Scaling: the final gather is linear in pixels. The High and Ultra tiers measure 24 ms in the gather alone at 1080p, and tier ray caps never raise the default 1024 rays.
- Not established: whether the surfel estimate is biased low at 1024 rays, or only slow to converge, cannot be separated with the data here. It is at least a 2x shortfall in rays, which is the first thing to fix.

The paper is a presenter transcript. It gives no equations for the spawn budget, the temporal estimator constants, the radial depth tuning, the grid sizes or the gather kernel. Where this plan gives numbers for those, they come from the team spec or from our measurements, and each is labelled.

## 1. Method and limits

Measurement setup (all runs): `build/vs2022-x64/Release/Nox_Engine.exe` built from this worktree (release, `--preset vs2022-x64`). Launch `--game --scene <scene> --window windowed --width 1920 --height 1080`, `NOX_FIXED_DT=0.0166666667`, static camera (no input), `NOX_EXIT_AFTER_FRAMES`, `NOX_LOG_PASS_METRICS=1` (interval 1), `NOX_SURFEL_GI_LOG_SUBPASS_METRICS=1` (interval 1), `NOX_FINAL_DUMP_DIR` / `NOX_FINAL_DUMP_FRAMES`. Surfel GI is enabled with `NOX_ENABLE_SURFEL_GI=1` (no scene key exists; `src/render/RenderSettingsTable.cpp` has SSGI keys but no surfel key). SSGI is set with `NOX_SSGI=0/1`. The surfel-off config uses `NOX_DISABLE_SURFEL_GI=1`. LPV is off by default (`src/ModularRenderer.h:159`).

Configurations per scene: `off` (no GI), `ssgi` (SSGI only), `surf` (surfel only, default Medium), `both` (surfel + SSGI), `surf_r4k` (`NOX_SURFEL_GI_RAY_BUDGET=4096`), `surf_spawn1` (`NOX_SURFEL_GI_SPAWN_BUDGET_SCALE=1.0`), and `ref`:

- `ref` = `NOX_ENABLE_SURFEL_GI=1 NOX_SSGI=0 NOX_SURFEL_GI_CAPTURE=1 NOX_SURFEL_GI_RAY_BUDGET=65536 NOX_SURFEL_GI_MAX_SURFELS=262144 NOX_SURFEL_GI_MAX_PROJECTED_SURFELS=524288 NOX_SURFEL_GI_MAX_SPAWNS=65536 NOX_SURFEL_GI_MAX_RECYCLES=65536 NOX_SURFEL_GI_SPAWN_BUDGET_SCALE=1.0 NOX_SURFEL_GI_QUALITY_TIER=3`, 900 frames.

Analysis: frames 200-260 for live configs (61 frames each), frames 780-840 for `ref`. GI contribution = frame(config) minus frame(off), decoded from sRGB to linear. Lit pixels = off-frame display luminance in (0.02, 0.97). Reference error = mean lit GI ratio, RMSE, and Pearson correlation against `ref`.

Limits, which apply to every number below:

1. The reference is not an independent ground truth. It is the same algorithm with a 65k-ray budget, so it shares any bias in the estimator (MSME heuristics, normalisation, radial depth). Its own convergence is about 5% on `scene.json`, 10% on Cornell (300 vs 600 frames) and about 8% on NFS (low signal). An independent reference needs a path tracer or analytic radiosity, which this phase did not build.
2. Output is 8-bit display-referred (tone-mapped). The composite debug modes do not add linearly through the tone curve, so per-term attribution from them is not reliable.
3. The GI-off frames themselves vary in time: 0.0028 (Cornell), 0.0053 (scene) and 0.0064 (NFS) display-luminance std over frames 200-260. Frame-difference noise is therefore an upper bound for scene and NFS.
4. NFS night street: `exposure_mode: auto` in the scene made GI on/off differences change sign (auto exposure reacts to GI brightness). All NFS numbers below use a temp copy with `exposure_mode: manual` (`scenes/_tmp_surfel_plan_nfs_manual.json`, deleted before finishing).
5. One GPU (RTX 3080), 1080p unless noted. The previous build was not rebuilt, so "halved vs previous build" from the audit is not reproduced. We show only that the current default is too dark relative to the reference.
6. Not measured: light-change lag, popping, light leaking through thin walls (see 3.7), and transparency.

Paper access: the Read tool could not render any page of the PDF (`pdftoppm` is not installed), so all 210 pages were read as the `pdftotext` text layer, through six extraction workers. The paper is a narrated transcript. Figures and graphs were not interpretable. A few embedded slide images were pulled out with pypdf, but not reviewed.

## 2. Paper summary (GIBS talk, page refs are PDF pages)

### 2.1 Surfel representation

- A surfel is position, radius and normal, and approximates a small disk of the surface (p.13). Surfels persist once spawned (p.14, p.18).
- Each surfel stores the transform ID of the geometry it is attached to and its local-space position. World position is recomputed each frame from a transform buffer (p.18-19). Skinned meshes use the single bone with the largest weight (p.20).
- Surfels live in a free list (stack of indices, p.26-28). All slots are allocated up front, so the count has a hard limit (p.25).
- Per-cell storage is lists of surfel references (p.32, p.40). No byte layout is given.

### 2.2 Radius, spawn, recycling

- Radius is scaled so that the screen-space footprint is roughly constant, both at spawn and while moving (p.23-24). Distant surfels have large radii (p.35). Radius must not exceed one grid cell side (p.33). No formula is given.
- Spawning is in screen space from the G-buffer (p.14-15, p.17). The screen is split into 16x16 tiles. Each tile takes the texel with the least current coverage. If that coverage is below a randomised threshold, one surfel is spawned from that texel (p.17). No threshold, budget or coverage metric is given. The team spec (section 5.5, not the paper) gives threshold 0.85 + 0.15 (blue-noise minus 0.5).
- Recycling is heuristic, to avoid discarding accumulated work (p.29). Three factors are combined: live surfel count, frames since last contribution to visible lighting, and distance. The combined score is compared against a uniform random number, so less relevant surfels are recycled with higher probability (p.29). No weights or timers are given.
- Converged surfels go dormant and send just enough rays to detect change (p.76, p.79).

### 2.3 Acceleration structure

- PICA PICA used a uniform grid (p.30). Each surfel is inserted into its own cell (p.32), into every cell its radius overlaps (p.33) and into immediate neighbours for continuity (p.34).
- Large distant radii force large cells and defeat the grid (p.35). The fix is a uniform centre grid plus trapezoidal (non-uniform) grids along each principal axis, with slice thickness growing with distance from the centre (p.36-37). The stated aim is constant cell size in screen space (p.38-39).
- Lookup: fetch the first N surfels in the pixel's cell, N not given (p.40). Cell capacity and overflow are not described.
- Ray binning sorts rays by a key that is dominated by the surfel's cell as a 1D hash, plus the direction (p.123-126). It is two counting passes and a reorder pass (p.126).

### 2.4 Ray budget, guiding, sharing

- Per surfel, rays requested per frame depend on variance, frames since last visible contribution, and a bonus for new spawns (p.76-78). A global ray budget is set by the content creator (p.78). It is enforced by a count pass (each surfel adds its request to a global counter) and a binning pass that gives each surfel a share proportional to its request (p.78).
- Ray guiding: default is cosine-lobe importance sampling (p.80). The paper shows the failure case where the cosine lobe misses a bright patch (p.83-84). The guiding map is a per-surfel 6x6 hemisphere radiance function, stored as 8-bit components with a 16-bit scale, normalised after each update (p.89-90). Sampling is inverse-CDF over the texels (p.91-107). Guiding is enabled once the map is populated (p.90). The paper's gain is conditional on the map approximating incoming irradiance (p.110).
- Irradiance sharing: a high-variance surfel blends with the average of neighbours from the acceleration structure (p.116). The paper reports that it significantly reduces perceived noise at 64 samples (p.118-122).

### 2.5 Tracing, hit lighting, multi-bounce

- Rays hit all geometry in the ray-tracing structure, including skinned and dynamic meshes (p.62). At a hit, direct light is evaluated by tracing the lights (p.62) and the existing surfel irradiance at the hit is added, giving "effectively infinite bounce" where coverage exists (p.62). Emissive surfaces are supported (p.61). Sky and miss handling are not described.
- Many lights (p.127-154): the stress scene has about 1400 lights in the frustum (p.127). Uniform random sampling converges too slowly (p.129-131). Stochastic lightcuts use 2-8 nodes (p.137). Reservoir sampling draws 4-8 candidates and keeps one (p.143), one shadow ray per sample, "cheaper on consoles" (p.141). Comparison references: brute force with 2 shadow rays (p.149), 8-sample reservoir after 15 frames (p.150), 4-sample lightcut (p.151-154), converged reference about 5 s (p.148).

### 2.6 Temporal integration

- Lighting is accumulated over many frames (p.74). The estimator is a modified moving average: a long-term mean plus a short-term mean and short-term variance, where the short-term estimators set the blend factor (p.75). The paper gives no formulas, constants or clamps and defers to a previous talk (p.75).
- Probes use the same MSME integrator and de-ringing windowing for SH (p.158).

### 2.7 Radial depth and visibility

- Problem: surfels do not know their neighbourhood, so light bleeds through walls (p.41, p.45).
- Each surfel stores a low-resolution radial depth function over its hemisphere, initialised to the surfel diameter (p.47). It is updated only from rays that hit geometry within the diameter (p.49). It stores a moving average of depth and of depth squared (p.53-54). A Chebyshev test gives a smooth visibility (p.55). Differences from DDGI: only depth within the diameter and hemisphere counts (p.56). Default resolution is 4x4 texels per hemisphere (p.57). The radial Gaussian removes wall and ceiling artifacts (p.58-59).

### 2.8 Application to screen

- Per pixel: reconstruct world position, find its cell, fetch the first N surfels, and accumulate their irradiance weighted by orientation and distance to the pixel (p.40). If the total weight is below 1, add the cell's weighted-average irradiance (p.40). The team spec (section 5.14) writes this as sum plus `cellAverage * max(0, 1 - totalWeight)`, which is an unnormalised sum, not an average.
- No gather resolution, kernel, upsampling or screen denoiser is described.

### 2.9 Interaction, transparency, performance

- Fully dynamic world, no precomputation (p.10-11, p.61). Surfels cannot cache transparent surfaces, because they spawn from the G-buffer (p.156). The answer given is ray-traced SH probe volumes with clipmaps (p.155-176, p.201-203). The clipmap has 4 levels (p.162) and blends two levels, which costs 2 volume samples per shaded pixel (p.174). A blue-noise dithered level choice is the cheaper alternative (p.174-175).
- SSGI is mentioned only as a way to reduce surfel spawning where SSGI already solves the scene (p.204). No composition rule is given.
- Targets and timings: PS5 class target, 4K output with screen-space passes at 1080p (p.183). Filtering about 0.2 ms (p.181). A PvZ open scene is about 7 ms total (p.186). The 1400-light stress scene is about 11 ms and still converging (p.191). A probe sample pass at 900p is about 0.2 ms (p.203). A 1800p checkerboard test is "quite a bit faster" (p.202).
- Artifacts named: coverage gaps (p.15-17), discontinuities at cell boundaries and for large radii (p.33-36), low-contribution pixels (p.40), noise from uneven sampling (p.84, p.118), clipmap border discontinuities and popping (p.169-170), dark or bright banding in SH (p.158). Fixes are given for each.
- Not done or open: procedural geometry is not explicitly supported (p.204). Light selection still selects dead branches (p.205). Lightcut traversal is not optimised (p.205). Shadow rays are to move to a later pass (p.205).
- Not in the paper: budgets, MSME constants, radial depth tuning, grid sizes, gather kernel, screen denoiser, memory budgets, light-change lag, leak tests.

## 3. Current implementation: pass map, gap table, measurements

### 3.1 Pass order (per frame, `src/passes/SurfelGIPipeline.cpp`)

Execute (`Execute`, lines 904-1512):

1. `begin_frame` (1058), `dispatch_args` (1076): counters and indirect group counts. Infra.
2. `transform_update` (`update_surfels.comp`, 1106): reattach surfels to transforms, radius from screen target. Extra (GIBS-like).
3. `recycling` (`recycle_surfels.comp`, 1126): cull by score. GIBS-core heuristic, plus extras (duplicates, sleeping).
4. `grid_clear`, `grid_build`, `cell_averages` (1131): full grid rebuild. GIBS-core, with deviation (27-neighbour insert, 8-probe overflow).
5. `coverage_reset`, `coverage_project` (1155, 1160; `coverage.comp`): project live surfels to tiles. GIBS-core screen coverage.
6. `coverage_spawn` (`spawn.comp`, 1242): one 64-invocation workgroup per 16x16 tile; min-coverage texel, budget `tiles*(0.004+0.06*deficit)*scale`, scale 0.25 (`SurfelGIManager.h:76`). Runs every frame, or every third frame when stationary (1165-1170).
7. `grid_clear`, `grid_build`, `cell_averages` again (1251), only when spawn ran.
8. `ray_request` (`request_rays.comp`, 1270): 1-8 rays, young surfels up to 12 (`:30`, `:151`), CV term. GIBS-core with extras.
9. `ray_allocation` (`allocate_rays.comp`, 1284 and 1287): priority pass, then proportional pass. GIBS-deviation (no binning).
10. `ray_generation` (`generate_rays.comp`, 1304): cosine or guide sampling, at most 8 rays per surfel (`:197`). GIBS-core, guide map 6x6.
11. `tracing` (`trace_rays.comp`, 1429): software BVH to 256 m (`trace_rays.comp:210`), shade with direct light and cached E. GIBS-core with extras (screen/grid fallback, sky visibility).
12. `temporal_integration` (`integrate.comp`, 1443): MSME via `msme.comp` plus heuristics (sparse-variance injection, lighting catch-up, bootstrap confidence). GIBS-core plus extras.
13. `radial_depth` (`radial_depth_update.comp`, 1455): 4x4 moments, EMA 0.2, 8 rays per surfel. GIBS-core, with deviations (section 3.3).
14. `irradiance_snapshot`, `irradiance_sharing` (1467, 1487): neighbour sharing when variance is high (up to 48 neighbours). GIBS-core (paper p.116); the audit calls it an extra, which is wrong.
15. `cell_averages` again (1493), then `count_live_surfels` every frame (1498, label `debug_overhead`).

ApplyIndirect (`ApplyIndirect`, 1514-1700), called after lighting:

16. `final_gather` (`apply_indirect.comp`, 1582): gather at `finalGatherResolutionScale = 0.32` (`SurfelGIManager.h:106`), writes raw irradiance and coverage (coverage = min(sumWeight, 1)). GIBS-core, with normalised average and heuristic fallback.
17. `composite_filter` (`spatial_filter.comp`, 1601): 7x7 bilateral. Extra.
18. `temporal_resolve` (`temporal_filter.comp`, 1638): history at 0.975 max weight. Extra.
19. `upscale_filter` (`upscale_filter.comp`, 1665): joint bilateral to output. Extra.

Debug: `debug_surfels.vert/.frag`, debug only.

Not dispatched: `msme.comp` is an include. `SurfelRayQueue` methods and `BinAndSortRays` are no-op stubs (`SurfelGIResources.cpp:364-402`).

### 3.2 Gap table (paper versus NOX)

| Component | GIBS (paper / spec) | NOX now (file:line) | Gap | Class |
|---|---|---|---|---|
| Surfel fields | Position, normal, radius, transform ID, local position (p.13-19) | 208 B struct (`SurfelGIResources.h:86-101`) | Extra debug, msme.zw, shortM2.a, unused fields | Deviation |
| Spawn | 16x16 tile, min-coverage texel, randomised threshold (p.17) | Same, budget formula and 0.25 scale (`spawn.comp:969-977`, `SurfelGIManager.h:76`) | Population set by a scale hack, not a target | Deviation |
| Recycling | Heuristic score vs random (p.29) | Score over many factors (`recycle_surfels.comp:330-351`) | No population target; 1024-group dispatch | Deviation |
| Grid | Uniform plus trapezoidal axis grids (p.36-37) | Central 32^3 plus 6 axis regions, 24 slices, growth 1.22 (`SurfelGIResources.h:9-19`, `common.glsl:537`) | Grid is 180,224 cells, 92 MB entries at 64 per cell | GIBS-core, oversized |
| Grid insert | Own cell plus overlapped and neighbour cells (p.32-34) | 27-neighbour insert (`build_grid.comp:150-156`) | Overlap test is wider than the radius | Deviation |
| Overflow | Not described | 8-probe nearest-keeping, key in `next` (`build_grid.comp:54-92`) | Approximate, not GIBS | Extra |
| Grid rebuild | Once per frame (implied) | 2 or 3 times per frame (1131, 1251, 1493) | Duplicate cell averages | Waste |
| Ray request | Variance, frames since contribution, new-spawn bonus (p.76-78) | Variance, young bonus, CV term, caps (`request_rays.comp:113-151`) | Requests are 17k vs 1k allocated | Deviation |
| Ray budget | Global budget by count and binning passes (p.78) | Proportional floor with priority pass (`allocate_rays.comp:71-137`) | No binning; priority pass has no effect (inventory finding) | Deviation |
| Ray binning | Two counting passes and a reorder (p.123-126) | Declared, not implemented (`SurfelGIResources.cpp:390-392`); `rayBins` written, never read (`generate_rays.comp:225`) | Missing | Missing |
| Guiding | 6x6 map, 8+16 bit, normalise each iteration (p.89-90) | 6x6 guide map (`generate_rays.comp:133-171`, `integrate.comp:99-110`) | Normalisation and enable threshold not verified | Partial |
| Hit lighting | Trace lights, reservoir for many lights (p.61-62, p.141-146) | Loop of up to 32 lights (`trace_rays.comp`, `SURFEL_TRACE_MAX_LIGHTS`) | No reservoir | Deviation |
| Multi-bounce | Existing surfel lighting at hits (p.62) | `albedo/pi * (direct + cached E)` (`trace_rays.comp:1040, 1462`) | None found | Matches |
| Temporal | Long and short mean, variance (p.74-75); spec 5.11 simplified MSME | RTG MSME (`msme.comp`) plus lighting/sparse/bootstrap heuristics (`integrate.comp:186-266`) | Heuristics not in paper or spec | Deviation |
| Radial depth | Diameter clamp, EMA per spec 0.05, default diameter (p.47-56; spec 5.12) | Hit distance not clamped, EMA 0.2, default `max(8r, 1.25)` (`radial_depth_update.comp:30, 45, 80-82, 105-106`) | Stores hits beyond diameter; over-occludes | Deviation |
| Radial visibility | Chebyshev (p.55) | Chebyshev, smoothstep (`common.glsl:408-427`) | Matches | GIBS-core |
| Sharing | High-variance neighbour blend (p.116) | Up to 48 neighbours, blend up to 0.32 (`irradiance_sharing.comp:52-54, 257`) | Audit label wrong | GIBS-core |
| Final gather | Sum with cell fallback when total < 1 (p.40, spec 5.14) | Normalised average (`apply_indirect.comp:632-633`), heuristic bounded fallback (`:328-357`) | Sparse pixels get full brightness from a single weak surfel | Deviation |
| Screen filters | None described | Spatial, temporal (max 0.975), upscale (`spatial_filter`, `temporal_filter`, `upscale_filter`) | Extra latency and cost | Extra |
| SSGI coupling | Only "combine to reduce spawning" (p.204) | `min(screenAO, nearVisibility)` multiplies far field (`deferred_lighting_frag.glsl:438-439, 509-510`) | Darkens (measured 66% on Cornell) | Deviation |
| Probes, transparency | Probe clipmap (p.155-176, p.201-203; spec 7) | Not present in the pass list | Out of scope for this plan | Missing |
| Tiers | Scalability claimed, no tiers (p.10) | Tier caps (`ModularRenderer.cpp:143-180`) | Caps never raise rays; High and Ultra gather at full resolution | Deviation |
| Stats readback | n/a | Async, 3-frame delay (`SurfelGIPipeline.cpp:737-778`) | No sync stall in frame path | OK |

### 3.3 Measured cost and state

Output is 1920x1080, indirect at 0.32 (615x346). Subpass GPU times are means over frames 40-300 (`SurfelGISubpassMetrics`). Execute times are per-frame medians (`SurfelGIUpdatePass`).

| Pass (ms) | Cornell surf | scene surf | NFS surf (auto exposure timing) |
|---|---|---|---|
| final_gather | 0.314 | 0.820 | 0.955 |
| tracing | 0.209 | 0.374 | 0.651 |
| ray_generation | 0.170 | 0.137 | 0.142 |
| recycling | 0.160 | 0.260 | 0.305 |
| irradiance_sharing | 0.134 | 0.166 | 0.165 |
| coverage_spawn (mean over all frames) | 0.113 | 0.241 | 0.315 |
| grid_build | 0.093 | 0.110 | 0.114 |
| cell_averages (2 to 3 calls) | 0.085 | 0.087 | 0.088 |
| composite_filter (spatial) | 0.060 | 0.337 | 0.293 |
| upscale_filter | 0.066 | 0.208 | 0.213 |
| coverage_project | 0.049 | 0.056 | 0.054 |
| ray_allocation | 0.044 | 0.041 | 0.044 |
| ray_request | 0.027 | 0.027 | 0.029 |
| temporal_integration | 0.020 | 0.021 | 0.020 |
| radial_depth | 0.015 | 0.016 | 0.017 |
| temporal_resolve | 0.013 | 0.031 | 0.026 |
| irradiance_snapshot | 0.013 | 0.013 | 0.013 |
| transform_update | 0.012 | 0.013 | 0.014 |
| count_live (`debug_overhead`) | 0.011 | 0.011 | 0.010 |
| grid_clear | 0.010 | 0.010 | 0.010 |
| begin_frame + cov_reset + dispatch_args | 0.018 | 0.019 | 0.024 |
| **Sum of labelled subpasses** | **1.63** | **2.99** | **3.50** |
| SurfelGIUpdatePass (Execute, median) | 1.48 | 2.73 | 3.36 |
| Unlabelled inside Execute (derived) | about 0.3 | about 1.1 | about 1.3 |

The unlabelled part is inside Execute but outside every `timedDispatch`. The likeliest place is `EnsureIncrementalBLAS` and `UploadIncrementalScene` (`RTSceneResources.cpp:599, 692`), called before the tracing timer (`SurfelGIPipeline.cpp:1320-1326`). This is not isolated. It grows with scene complexity, which fits the numbers.

**Correction (section 10.1):** the 1.1 ms in the table is an accounting error. The pass total includes ApplyIndirect and the labels cover it: per frame the unlabelled GPU time is 0.00-0.04 ms on scene.json, Cornell and NFS.

Counters (Medium, frame 300): live surfels 1.7k (Cornell), 1.5k (scene), 2.2k (NFS). Rays requested 17.4k, 16.2k and 22.3k. Rays allocated 1024 in all three, so 4.6 to 6.3% of requests are served. Under-covered tiles 44 of 1638 visible (Cornell), 124 of 7910 (scene), 459 of 7890 (NFS).

Grid: 180,224 cells (32^3 central plus 6 x 32x32x24 axis cells). Entries at 64 per cell: 11.5M x 8 B = 92 MB. `clear_grid` rewrites 2.75 MiB of headers each frame. Overflow counters are not logged, so occupancy and overflow were not measured.

Memory at 65,536 surfels (from the inventory): surfels 13.6 MB, radial depth 8.4 MB, guide map 9.4 MB, irradiance snapshot 1 MB, ray requests 1 MB, grid headers 2.9 MB, grid entries 92 MB, coverage tiles 1.2 MB at 1080p, dead `GridAverages` 5.8 MB, dead `SortedRays` 4 MB.

### 3.4 Energy against the reference (linear, lit pixels, frames 200-260)

| Scene | Reference (`ref`) | surf | surf / ref | rmse / ref mean | Pearson | both / surf | ssgi / ref |
|---|---|---|---|---|---|---|---|
| Cornell | 0.289 | 0.134 | 0.46 | 0.60 | 0.80 | 0.66 | 0.09 |
| scene.json | 0.122 | 0.113 | 0.93 | 0.17 | 0.99 | 0.95 | 0.01 |
| NFS (manual) | 0.0112 | 0.0072 | 0.64 | 1.04 | 0.93 | 1.10 | 0.01 |

Other configs, GI-on mean divided by reference: Cornell `surf_r4k` 0.96, `surf_spawn1` 0.27, `both` 0.31 (with SSGI). scene.json `surf_r4k` 0.99, `surf_spawn1` 0.83, `both` 0.88. NFS `surf_r4k` 1.04, `surf_spawn1` 0.61, `both` 0.71.

Convergence of the reference: Cornell 0.315 (frame 300), 0.286 (600), 0.289 (780-840). scene.json 0.122 for all three. NFS 0.0118, 0.0100, 0.0112.

Interpretation: the default is under-converged, not over-bright. A 4x ray budget brings Cornell to 96% and scene to 99%. A 4x population at the same ray budget makes the estimate darker (Cornell 0.27 of reference), because each surfel gets fewer rays. Population is not the lever; rays are.

### 3.5 Temporal noise (lit pixels, frames 200-260)

| Scene | Config | GI std over time, linear | Relative to GI mean | Note |
|---|---|---|---|---|
| Cornell | surf | 0.0040 | 3.0% | GI-off std 0.0028 (display) |
| Cornell | both | 0.0025 | 2.8% | |
| Cornell | surf_r4k | 0.0097 | 3.5% | |
| scene.json | surf | 0.0020 | 1.7% | GI-off std 0.0053 (display), so not separable |
| NFS (manual) | surf | 0.0008 | 10.5% | low signal (GI mean 0.007) |

The relative figure for NFS reflects a small mean, not a noisy estimate. The noise numbers for scene and NFS should be read as upper bounds. The next step (WP0) is a surfel-term-only readout (`NOX_SURFEL_GI_COMPOSITE_DEBUG=4`), with GI-off temporal noise subtracted.

### 3.6 Resolution and tier scaling (scene.json, surf)

| Output | Indirect px | final_gather | upscale | composite | tracing | UpdatePass median | Sum of subpasses |
|---|---|---|---|---|---|---|---|
| 1280x720 | 410x231 | 0.445 | 0.100 | 0.167 | 0.388 | 2.31 | 2.25 |
| 1920x1080 | 615x346 | 0.829 | 0.204 | 0.364 | 0.371 | 2.73 | 3.03 |
| 2560x1440 | 820x461 | 1.379 | 0.362 | 0.582 | 0.383 | 4.02 | 4.21 |

Gather and filters scale linearly with output pixels. Tracing does not depend on resolution. The Execute part grows too (tile-based coverage and spawn).

| Tier (1080p, scene.json, surf) | Indirect px | maxSurfels | rays used | final_gather | tracing | Sum of subpasses |
|---|---|---|---|---|---|---|
| Low (tier 0) | 960x540 | 24576 | 1024 | 1.867 | 0.174 | 3.84 |
| Medium (default, tier 1) | 615x346 | 65536 | 1024 | 0.829 | 0.371 | 3.03 |
| High (tier 2) | 1920x1080 | 65536 | 1024 | 24.18 | 0.362 | 27.2 |
| Ultra (tier 3) | 1920x1080 | 65536 | 1024 | 24.37 | 0.368 | 27.5 |

All tiers run 1024 rays, because the context default (`RenderContext.h:168`, 1024) is only capped by the tier (`ModularRenderer.cpp:565` raises to 1024 minimum; the Medium cap comment at `:179` says 4096). Low disables both the software BVH trace and the screen trace (`ModularRenderer.cpp:169`, positions 11-12), so its energy is not comparable and was not measured. Low's Execute time (3.81 ms) is higher than Medium's (2.73 ms) for a reason not established.

### 3.7 Other measurements

- SSGI interaction: `both / surf` is 0.66 on Cornell, 0.95 on scene.json and 1.10 on NFS. The darkening follows how enclosed the scene is, consistent with `min(screenAO, nearVisibility)` removing far-field light where SSGI sees occluders (average visibility about 0.66 on lit Cornell pixels is inferred, not measured).
- Wall-leak scene (`scenes/surfel_gi_wall_leak_validation.json`): inconclusive. The GI-off frame has a dark region that the surfel and reference frames light (a red bounce). Surfel and reference agree there, so the default matches the converged reference, but whether that light is physically correct depends on the geometry, which was not verified. No leak metric was produced.
- Auto exposure artefact: with `exposure_mode: auto` the NFS on/off difference was -0.069 (linear, lit threshold 0.08, earlier run), and with manual exposure +0.0072 (linear, lit threshold 0.02, final run). Enabling surfel GI changes the exposure of auto-exposed scenes. This is a user-visible effect, not only a measurement issue.
- Composite debug modes on NFS (manual): IBL-only frame identical with surfel on or off (0.1866 display), direct-only identical (0.2188). Surfel term alone 0.2108 display. These are not additive through the tone curve, so the diffuse-IBL occlusion effect (`diffuseIBLOcclusion = diffuseAO*(1-coverage)`, `deferred_lighting_frag.glsl:500`) is not resolved either way.
- Validators: `scripts/validate_surfel_gi_static.py` fails at `require("m_pendingTimingQueries"... "GL_QUERY_RESULT_AVAILABLE")` (line 143). `scripts/validate_surfel_gi_correctness.py` fails at `emitterFacing/receiverFacing/transferFacing` (line 61). Both stop at the first assertion, and both check names from an older design.
- Light-change lag, popping, transparency: not measured.

## 4. Root causes (with evidence)

RC1. Too dark: ray starvation.
- Evidence: requested 16-22k rays per frame, allocated 1024 (section 3.3). Energy ratio 0.46 at 1024 rays and 0.96 at 4096 (section 3.4). Population x4 at 1024 rays gives 0.27, so the ray budget is the bottleneck, not the population.
- Cause: the default `surfelGIMaxRayBudget = 1024` (`RenderContext.h:168`), with tier caps that never raise it (`ModularRenderer.cpp:565`, cap comment `:179`). Allocation is a proportional floor (`allocate_rays.comp:71-73`), so the largest requests are cut hardest.
- Note: 1024 rays over about 2000 surfels is 0.5 rays per surfel per frame. Convergence takes hundreds of frames, which the static-camera runs show as frame 300 still moving 10% on Cornell.

RC2. Darkening with SSGI.
- Evidence: `both / surf` 0.66 (Cornell), 0.95 (scene), 1.10 (NFS). SSGI alone is 0.09 of reference on Cornell, so SSGI adds little while it removes far-field light.
- Cause: `diffuseAO = min(screenAO, gi.nearVisibility)` (`deferred_lighting_frag.glsl:438-439`), then the far field is multiplied by `diffuseAO` (`:510`). `nearVisibility` is the open fraction of SSGI's own horizon (`:316-317`), which removes light from occluded directions that SSGI does not replace (SSGI misses are zero).

RC3. Cost: screen-space chain and rebuilds.
- Evidence: final gather 0.82 ms, spatial 0.34, upscale 0.21, temporal 0.03 on scene.json (1.40 ms, section 3.3). Grid rebuilt twice or three times (`SurfelGIPipeline.cpp:1131, 1251, 1493`). 1.1 ms of Execute is unlabelled.
- Cause: the gather runs at 0.32 (`SurfelGIManager.h:106`) over up to 27 cells x 64 candidates (`apply_indirect.comp:77-78`, early exit at weight 64 on `:82`). Three screen filters are extras with their own history (`temporal_filter.comp:28-30`). Grid clear, build and averages cover 180k cells each time.

RC4. Cost: dispatch and population.
- Evidence: seven passes dispatch 1024 groups at 65k surfels for about 2k live (inventory). Recycling 0.26 ms, spawn 0.24 ms on scene.json.
- Cause: `groupsSurfels` dispatch for surfel-wide passes (`SurfelGIPipeline.cpp:917, 1270, 1304`, others). Population is set by the spawn budget scale 0.25, not a target (`spawn.comp:969-977`).

RC5. Noise and stability.
- Evidence: Cornell temporal rel 3.0%, scene 1.7%. Stacked heuristics (`integrate.comp:186-266`) and screen temporal history 0.975 (`temporal_filter.comp:30`) add latency.
- Cause: the gather uses a normalised average, so a sparse pixel takes the full average of its few surfels (`apply_indirect.comp:632-633`). Young surfels (up to 12 slots, 4 valid) add stale samples (RC6).

RC6. Correctness: stale ray slots.
- Evidence: `request_rays.comp:119-121` gives young surfels 12 slots, `:151` caps them at 12. `generate_rays.comp:197` writes `min(allocatedCount, 8)` rays. `integrate.comp:136-147` reads all `allocatedCount` slots with no ownership check, so slots 8 to 11 are stale. `radial_depth_update.comp:72` does check ownership, so the bug is specific to integrate. Impact not measured. Young surfels are a small share of steady state, larger in spawn-heavy phases.

RC7. Leak risk: normalised gather and unclamped radial depth.
- Evidence: the gather normalises (`apply_indirect.comp:632-633`) instead of summing with cell fallback (paper p.40, spec 5.14). Radial depth stores hits beyond the diameter, default depth `max(8r, 1.25)` (`radial_depth_update.comp:45, 80-82`, paper p.47-49).
- Cause: a single weak surfel with a large weight gets full brightness, and a receiver behind a wall gets light from a surfel whose radial depth was never bounded. Not measured. The wall-leak scene did not give a usable answer.

RC8. Poor scaling with resolution and tiers.
- Evidence: gather scales with pixels (section 3.6). High and Ultra measure 24 ms in the gather at 1080p.
- Cause: High and Ultra set `finalGatherResolutionScale = 1.0` and `gatherNeighborRadius = 2` (`ModularRenderer.cpp:150-200`), so each pixel scans 125 cells. Tier ray caps never raise rays (RC1).

RC9. Scene size.
- Evidence: fixed grid of 180k cells and 92 MB at 64 per cell, independent of scene size. Surfels per tile about 0.2 to 0.3. Far extent 140 m (`SurfelGIPipeline.cpp:814`).
- Cause: fixed grid layout sized for the central 24 m region and far trapezoids. The grid cost does not depend on live population, and BVH rays go to 256 m regardless of geometry (`trace_rays.comp:210`).

RC10. Light lag.
- Evidence: a light-change hold of `6 + 18 * change` frames and decay (`SurfelGIPipeline.cpp:932-941`), plus a catch-up blend (`integrate.comp:231-235`).
- Not measured.

RC11. Dead and misleading code.
- `rayBins` written and never read (`generate_rays.comp:225`). `GridAverages` (binding 26) and `SortedRays` (17) allocated and unused. `useRayBinning` is UI-only. `radialDepthUpdateInterval`, `spawnPasses`, `fastFillSpawnPasses`, `spawnCandidateCount` are unused. `msme.zw` written and unread. `debug.z` is overwritten by `update_surfels.comp:138` before `request_rays.comp:72` reads it as spawn coverage. `sourceDirect*` counters never incremented. `allocate_rays.comp:126-131` priority branch assigns the same value on both paths (inventory finding, not re-read). `request_rays.comp:160-162` mixes `atomicMax` with `atomicAdd` on `requestedRays`. `common.glsl:537` hard-codes slice growth 1.22 instead of the setting. `spawn.comp:844-848` `missingFinalGI` is always false.

## 5. Work packages

Principles: GIBS core only. Remove screen-space filters that exist to hide noise. Make the ray budget the explicit quality knob. Target one grid build per frame. Keep everything generic (no game-specific data in passes or shaders, per the engine rule).

Acceptance metrics refer to section 3 measurements. Regression runs use the same scenes, the same reference (`ref`, section 1), and frames 200-260 unless stated.

Dependency order and file ownership are in section 6.

### WP0. Measurement and reference (no behaviour change)

Goal: make the acceptance numbers reproducible and separable.

Work:
1. Lock exposure in every on/off comparison (`exposure_mode: manual`). Add a note in scene files used for GI tests, or a `NOX_EXPOSURE_MODE` override.
2. Add a surfel-term-only temporal metric (`NOX_SURFEL_GI_COMPOSITE_DEBUG=4`) and subtract GI-off temporal noise, so noise is separable.
3. Add to `SurfelGIFrameCounters`: `overflowGridEntries`, allocation shortfall (`requested - allocated`), live count, and young-surfel count.
4. Time `EnsureIncrementalBLAS` and `UploadIncrementalScene` (wrap in the surfel timer or a new label). Closes the 1.1 ms gap.
5. Add an independent reference mode for Cornell (brute-force path or hemisphere sampling with the software BVH, many samples per pixel). Needed for WP6 and WP8 leak tests.
6. Replace the two failing validators with metric tests that use the harness above. Keep them in `scripts/`.

Files: `src/passes/SurfelGIPipeline.cpp` (logging only), `src/RTSceneResources.cpp` (timing only), `scripts/validate_surfel_gi_*.py`, new harness under `tools/` (optional).

Acceptance: the harness reproduces section 3.4 and 3.5 numbers within 5% on two repeat runs.

### WP1. Correctness fixes and dead-code removal (no algorithm change)

Goal: remove wrong and unused work before tuning.

Work:
1. Stale slots: in `integrate.comp`, loop over `min(allocatedCount, generatedCount)` and check `hits[rayID].ids.x == surfelID` (the check radial depth already has). Preferred: make generation cover every allocated slot (see WP2) and keep the check as an assertion.
2. Separate `debug.z` (spawn coverage) from the radius-error write in `update_surfels.comp:138`.
3. Delete `rayBins` (`generate_rays.comp:225`, binding 27), `GridAverages` (26), `SortedRays` (17) and their allocation in `SurfelGIResources.cpp`. Delete `BinAndSortRays` and the no-op `SurfelRayQueue` methods.
4. Delete `useRayBinning` (UI at `RenderingSettingsWindow.cpp:1113`, `ModularRenderer.cpp:586`), `radialDepthUpdateInterval`, `spawnPasses`, `fastFillSpawnPasses`, `spawnCandidateCount` and the dead `sourceDirect*` path (`integrate.comp:133, 187, 190, 232, 233, 247, 258, 261-265`).
5. Remove `msme.zw` writes (`msme.comp:29-32`) unless debug uses them.
6. Fix `request_rays.comp:160-162` (use one atomic type for `requestedRays`).
7. Make `common.glsl:537` read the setting, or delete the setting.
8. Reduce the cell_average and grid rebuild calls to the minimum that keeps the trace valid (WP5 does the full fix).

Files: `generate_rays.comp`, `integrate.comp`, `msme.comp`, `request_rays.comp`, `update_surfels.comp`, `common.glsl`, `SurfelGIResources.{h,cpp}`, `SurfelGIManager.h`, `SurfelGIPipeline.cpp`, `ModularRenderer.cpp`, `RenderingSettingsWindow.cpp`.

Acceptance: energy within 2% of section 3.4 except the stale-slot fix (report the delta); GPU time down at least 0.05 ms; no shader or host warnings.

### WP2. Ray requests, budget and allocation (GIBS p.76-78, dormancy p.76, p.79)

Goal: spend the ray budget where it reduces error, and serve the budget fully.

Algorithm, per live surfel s each frame it is requested:
- Relative spread `cv_s = sqrt(max(msme.shortM2_luminance, 0)) / max(lum(shortMean), eps)`.
- Request `r_s = clamp(ceil(2 * cv_s), 1, 8)`.
- Young (age < 4 frames): `r_s = max(r_s, 4)` (down from 12).
- Dormant: if `cv_s < 0.05` and `age > 32`, request 1 ray on frames where `(id + frame) % 8 == 0`.
- Lighting change: `r_s = max(r_s, 2)` for `holdFrames` (keep the existing factor).

Allocation with budget `B` (new Medium default 2048; High and Ultra 4096 and 8192; Low 1024):
- If `sum(r_s) <= B`, grant `r_s`.
- Else grant `floor(r_s * B / sum(r_s))`, then give the remaining rays to the largest fractional parts (largest remainder). This needs one prefix-sum over the request buffer, or a 2-pass method with fixed point.
- Grant contiguous ray ranges; generate exactly the granted count (never more, never fewer).

Files: `request_rays.comp`, `allocate_rays.comp`, `generate_rays.comp`, `RenderContext.h:168` (default follows tier), `ModularRenderer.cpp:150-200, 565, 179` (tier table as values, not caps).

Acceptance (Cornell and scene, frames 200-260): energy ratio to `ref` in [0.9, 1.1]; `sum(allocated) / sum(requested)` at least 0.3 on Cornell; tracing at most 0.25 ms on scene.json at B = 2048. Reference points: B = 1024 gives 0.46 (Cornell) and 0.93 (scene); B = 4096 gives 0.96 and 0.99.

The rule above may still fall short at 2048. If it does, WP3 (coherence, cheaper hits) is the next lever, not a larger budget.

### WP3. Trace efficiency and hit lighting (GIBS p.123-126, p.141-146; spec 5.8, 6.1)

Goal: lower the cost per ray so a 2048 to 4096 budget fits the cost target.

Work:
1. Ray binning: bin key = `(cellHash(surfel cell) * 8 + octant(direction))` with a 2-pass count and scatter, then trace in bin order. Reuse the `rayBins` binding (WP1 removes the dead write).
2. Hit lighting: replace the loop over up to 32 lights (`SURFEL_TRACE_MAX_LIGHTS`) with reservoir sampling of 4 to 8 candidates by `weight = luminance(intensity) / max(dist^2, eps)` (p.143-145). One shadow ray for the winner, scaled by `total_weight / (N * weight_winner)`. Keep the directional light as one candidate.
3. Tighten the software BVH to 256 m only when the scene needs it. Use `tMax = min(256, 2 * farScale)` as a first step, then measure.
4. Remove the screen-space and grid fallback traces when the BVH is present (`trace_rays.comp:1417-1432`), as the audit already notes.

Files: `trace_rays.comp` (1504 lines; split a helper include if needed), `generate_rays.comp`, `SurfelGIPipeline.cpp` (bin passes), `SurfelGIResources.{h,cpp}`, `common.glsl`.

Acceptance: at 2048 rays, tracing at most 0.3 ms on scene.json (today 0.37 ms at 1024; the per-1024 cost is 0.13 ms on Cornell and 0.22 ms on scene.json). Energy and noise unchanged within 5% of WP2.

### WP4. Spawn, population and recycling (GIBS p.14-17, p.23-29; spec 5.3, 5.5)

Goal: population set by a target, not a scale hack; cheap spawn and recycle.

Population target: `N_target = kappa * visibleTiles`, kappa = 0.25 at Medium (tuned per tier), `visibleTiles` from `coverageVisibleTileCount`.

Spawn per frame: `spawnBudget = clamp(N_target - N_live + recycleCount, 0, maxSpawnsPerFrame)`, with `maxSpawnsPerFrame` at 64 for Medium. Candidate per tile: argmin coverage (the existing coverage map is accepted with one-frame lag). Threshold `T = 0.85 + 0.15 * (bn(tile, frame) - 0.5)` (spec 5.5). Spawn if `coverage_min < T` and the budget allows.

Recycle score (per paper p.29):
- `s = 0.4 * sat((N_live - N_target) / N_target) + 0.3 * sat(framesSinceContributed / 120) + 0.3 * sat(dist / farScale)`.
- Cull if `hash01(id, frame) < 0.5 * s`, at most 256 per frame.

Dispatch: size recycle and count passes by the high-water index from `dispatch_args`, not `groupsSurfels`.

Files: `spawn.comp`, `recycle_surfels.comp`, `update_surfels.comp`, `coverage.comp` (only if the tile counts change), `SurfelGIManager.h` (`spawnBudgetScale` becomes `kappa`), `SurfelGIPipeline.cpp` (dispatch args, spawn uniforms).

Acceptance (scene.json): recycle plus spawn at most 0.20 ms (today 0.50 ms). Live count within 20% of `N_target` at steady state. Under-covered visible tiles at most 3% (today 1.6% scene, 2.7% Cornell, 5.8% NFS, so NFS needs the change). Spawn rate lower in stationary views.

### WP5. Acceleration grid and cell averages (GIBS p.30-40)

Goal: one grid build per frame, memory at most 25 MB, cell averages free.

Layout: keep the central plus axis-region layout (p.36-37), with cell capacity 32 and 4-byte entries (`surfelID` only, sorted by no key). Radius must be at most one cell side at the cell size of its region (p.33).

Build in 3 dispatches, each sized by live count:
1. Count: `atomicAdd(cellCount[c], 1)` for the surfel's own cell and overlapped cells (use a radius test, not the fixed 27 neighbours).
2. Prefix sum over cells (two-level scan, about 180k cells) into `cellOffset`.
3. Scatter: `entries[cellOffset[c] + atomicAdd(cursor[c], 1)] = surfelID`, and accumulate the cell average in the same pass with fixed-point atomics (`sumE`, `sumConf`).

Remove `clear_grid` (the count pass clears), `cell_average.comp`, and the two extra rebuilds (`SurfelGIPipeline.cpp:1131, 1251, 1493`). Cell averages are read as `sumE / sumConf` at lookup.

Overflow: a surfel that does not fit is dropped and counted (no nearest-keeping hack, `build_grid.comp:54-92`). Its loss is measured and logged (WP0).

Files: `build_grid.comp`, `clear_grid.comp` (delete), `cell_average.comp` (delete), `SurfelGIResources.{h,cpp}` (capacity, entry struct, averages buffer), `SurfelGIPipeline.cpp` (grid calls), `common.glsl` (cell helpers), readers in `trace_rays.comp`, `apply_indirect.comp`, `recycle_surfels.comp`, `spawn.comp`, `coverage.comp`, `irradiance_sharing.comp`.

Acceptance: grid total (count + scan + scatter) at most 0.10 ms on scene.json (today 0.21 ms across clear, build and averages). Memory at most 25 MB at Medium. Overflow counter 0 on Cornell and scene.json.

### WP6. Radial depth (GIBS p.41-59; spec 5.12)

Goal: stop light leaking through walls, and stop over-occluding open surfaces.

Algorithm (per surfel, per granted ray that resolves):
- `d = min(hitDistance, 2 * r)` for hits. Misses count as `d = 2 * r` (open within the diameter).
- Update only when the ray is in the diameter (`hitDistance <= 2 * r` or a miss), in the hemisphere (`hemiDir.z > 0`).
- Per texel: `mean = mix(mean, d, 0.05)`, `meanSq = mix(meanSq, d*d, 0.05)` (spec EMA 0.05, not 0.2).
- Default: `mean = 2 * r`, `meanSq = mean^2`.
- Read: unchanged (Chebyshev with smoothstep and minimum visibility as in `common.glsl:408-427`).

Rays: use the GI rays already granted, up to 4 per surfel (paper p.52: the estimate improves with rays).

Files: `radial_depth_update.comp` (lines 30, 45, 80-82, 95-106), `common.glsl` (texel bin and defaults), `apply_indirect.comp` (visibility call only).

Acceptance: wall-leak scene: dark-side GI reduces to within 10% of the independent reference (WP0 item 5) after the change, and the before-change leak is measured first with the same reference. Scene.json energy within 5% of WP2 result.

### WP7. Temporal integration (GIBS p.74-75; spec 5.11)

Goal: one estimator with two rates, not a stack of heuristics.

Algorithm, per surfel per frame, with sample `x` (clamped only by the RTG firefly gate):
- `shortMean = mix(shortMean, x, 0.2)`.
- `shortM2 = mix(shortM2, (x - oldShort)^2, 0.2)`.
- Long mean blend: `alpha = mix(0.02, 0.35, reactive)` with `reactive = sat(lum(shortM2) * 4 + lum(|shortMean - mean|) * 2)` (spec 5.11).
- New surfel: `alpha = max(alpha, 0.6)`.
- Lighting change `L` in [0, 1] from the existing factor: `alpha = max(alpha, L * 0.5)` and `shortMean` snaps to `x` when `L` jumps (replaces catch-up and bootstrap blends).

Remove: sparse-variance injection (`integrate.comp:186-192`), lighting reactive variance (`:216-221`), catch-up blend (`:231-235`), bootstrap confidence (`:257-266`), guide alpha coupling (`:104`, which WP10 owns).

Files: `integrate.comp`, `msme.comp` (keep the firefly gate `:62`, drop unused vbbr if unneeded), `SurfelGIPipeline.cpp:930-946` (lighting change factor, simplified).

Acceptance (Cornell, scene.json; frames 200-260): energy within [0.9, 1.1] of reference; temporal relative noise at most 2% (Cornell) and 1.5% (scene.json) on the surfel-term readout. Light-change response (new test, needed): 90% of a step change in 12 frames or fewer at 60 fps.

### WP8. Final gather and screen chain (GIBS p.40; spec 5.14)

Goal: gather cheaply, with no screen-space temporal or spatial denoiser.

Algorithm, per output pixel (at gather resolution 0.5 of output):
- Candidates: first N = 32 accepted surfels from the 3x3x3 cells around the receiver's cell (cell list order, as GIBS p.40).
- Weight: `w = exp(-(d/r)^2) * normalWeight * planeWeight * radialVisibility` (spec 5.14 forms; `normalWeight` uses `finalGatherNormalCos`).
- Sum: `E_sum = sum(w * E_s)`, `W = sum(w)`.
- Fallback: `E = E_sum + cellAvg * max(0, 1 - W)`. No normalisation by W.
- Output coverage `c = min(W, 1)` (keeps spawn coverage and the composite blend, as today).

Upsample: joint bilateral from gather resolution to output (depth and normal, 4 taps) inside `upscale_filter.comp`, which becomes the only screen-space pass. Delete `spatial_filter.comp` and `temporal_filter.comp`.

Files: `apply_indirect.comp` (fallback `:328-357`, gather `:582-665`), `upscale_filter.comp`, `SurfelGIPipeline.cpp` (`ApplyIndirect`, `:1514-1700`), `ModularRenderer.cpp:179` (tier scale).

Acceptance (scene.json): final gather plus upscale at most 0.35 ms (today 1.03 ms with spatial and temporal). Energy within 5% of the WP7 result. Temporal relative noise of the final image at most 2% (Cornell). No ghosting in a camera-motion check (to be defined).

### WP9. SSGI and surfel composition (lighting shader; cross-team)

Goal: no double counting and no darkening. SSGI adds near-field light, surfels add far-field light, neither multiplies the other.

Recommended composition (Option A, default):
- Far field: `farFar = giResponse * E_surfel` (from WP8), multiplied only by material AO (`aoTex`) and the screen-space AO of the existing SSAO (not by SSGI visibility).
- Near field: SSGI `nearIrradiance` as today, times `aoTex`, which already contains its own occlusion (`deferred_lighting_frag.glsl:507`).
- Double counting is bounded by the SSGI-only contribution (Cornell 0.026, 9% of reference, so 0.026 at most).

Option B (if double counting is measured to exceed 5% of reference): gate SSGI by `1 - coverage` for the diffuse IBL and keep SSGI for contact darkening only.

Also verify `diffuseIBLOcclusion = diffuseAO * (1 - coverage)` (`deferred_lighting_frag.glsl:500`): with the surfel sky term under-converged, this can darken. The NFS diagnostic did not resolve it.

Files: `shaders/deferred_lighting_frag.glsl` (shared, coordinate with the lighting owner), `shaders/indirect_diffuse_*.comp` if the SSGI output changes.

Acceptance: `both` at least `surf` on all three scenes; `both - surf` at most 1.05 times `ssgi` alone; `both` within 10% of reference on Cornell (today 0.31 of reference).

### WP10. Ray guiding and irradiance sharing (GIBS p.80-122; spec 5.7, 5.13)

Goal: keep guiding only if it reduces variance at equal rays; share only at high variance.

Work:
1. Guide map: 6x6 per surfel, 8-bit components with a 16-bit scale, normalised after each update (paper p.90). Enable sampling once the map's total is above a threshold (paper p.90).
2. Evaluate guiding on and off at equal rays (WP0 harness). Keep it only if it lowers variance on Cornell or NFS by at least 10%.
3. Sharing: blend only when `lum(shortM2) > threshold`, with neighbours from the same cell and adjacent cells (spec 5.13, paper p.116). Cap the neighbour count at 16 (today 48).

Files: `generate_rays.comp` (guide sampling), `integrate.comp` (guide update, `:93-110`), `recycle_surfels.comp:434`, `spawn.comp:753` (guide reset), `irradiance_sharing.comp`, `irradiance_snapshot.comp`.

Acceptance: sharing at most 0.05 ms on scene.json (today 0.17 ms), or removed. Energy change within 5%.

### WP11. Tiers and scalability (paper p.183; spec tiers)

Goal: tiers scale cost predictably and do not break at High.

Tier table (1080p, scene.json, surfel only), proposed targets:

| Tier | Rays per frame | Surfels max | Gather scale | Gather candidates | Target surfel ms |
|---|---|---|---|---|---|
| Low | 1024 | 24,576 | 0.25 | 16 | 0.8 |
| Medium (default) | 2048 | 65,536 | 0.5 | 32 | 1.5 |
| High | 4096 | 131,072 | 0.5 | 48 | 2.5 |
| Ultra | 8192 | 262,144 | 0.5 | 64 | 3.5 |

Changes: tier values are the settings, not caps over a default (`RenderContext.h:168`, `ModularRenderer.cpp:143-200`). High and Ultra do not use gather scale 1.0 or radius 2. Low keeps the software BVH (it currently disables it, `ModularRenderer.cpp:169`) if the energy of the Low tier is to be meaningful.

Cost model (fit after WP2-WP8; the form is what matters): `T = a * outputPixels * N_candidates * gatherScale^2 + b * B + c * N_live + d * tiles + e * cells`, where `B` is rays per frame, `tiles` is the spawn tile count and `cells` is the grid cell count (fixed at 180k or reduced by WP5).

Scaling requirements: 1440p at most 1.8 times 1080p (today 1.39 total, 1.66 for the gather). Scene size: the cost should depend on live count and budget, not on scene extent.

Files: `ModularRenderer.cpp`, `RenderContext.h`, `SurfelGIManager.h`, `SurfelGIPipeline.cpp` (sizing).

Acceptance: Low at most 0.8 ms, Medium at most 1.5 ms, High at most 2.5 ms, Ultra at most 3.5 ms (scene.json, 1080p). Tier energy checked against `ref` at each tier (Low has no reference yet; state it).

### WP12. Transparency and probes (out of scope here)

Surfels cannot cache transparents (p.156). The paper's answer is SH probe clipmaps (p.155-176, 201-203; team spec 7). The probe pass is not in the engine, so this rework does not change transparency. A separate plan should cover it.

## 6. Order, parallelism and file ownership

Dependencies:
- WP0 first. No behaviour change, so it can run alongside WP1.
- WP1 next (correctness and dead code), which unblocks everything in `integrate.comp` and `generate_rays.comp`.
- After WP1, these can run in parallel because their files do not overlap: WP2 (`request_rays`, `allocate_rays`, `generate_rays`), WP4 (`spawn`, `recycle_surfels`, `update_surfels`), WP5 (`build_grid`, `clear_grid`, `cell_average`, `SurfelGIResources`), WP6 (`radial_depth_update`).
- WP7 (`integrate.comp`) after WP1. WP10 touches `integrate.comp` and `generate_rays.comp`, so it waits for WP7 and WP2.
- WP3 (`trace_rays.comp`, binning) after WP2, because it changes the ray order that WP2 creates.
- WP8 (`apply_indirect`, `upscale_filter`) after WP5 (grid layout and averages) and WP7 (stable E).
- WP9 (lighting shader) after WP8, and coordinated with whoever owns `deferred_lighting_frag.glsl`.
- WP11 last, after the cost model is fitted.

File ownership (one writer at a time): `SurfelGIPipeline.cpp` is touched by WP0, WP1, WP2 (dispatch), WP3, WP4, WP5, WP8 and WP11. Serialise those edits, or split the pipeline file into per-pass units first. `integrate.comp`: WP1, WP7, WP10. `generate_rays.comp`: WP1, WP2, WP3, WP10. `apply_indirect.comp`: WP5 (readers), WP6 (visibility), WP8. `ModularRenderer.cpp`: WP1, WP2 (defaults), WP11.

## 7. Deletions (summary)

Delete outright: `cell_average.comp`, `clear_grid.comp` (after WP5), `spatial_filter.comp`, `temporal_filter.comp` (after WP8), `BinAndSortRays` and the no-op queue methods, `GridAverages`, `SortedRays`, `rayBins` write, `useRayBinning`, `radialDepthUpdateInterval`, `spawnPasses`, `fastFillSpawnPasses`, `spawnCandidateCount`, `msme.zw`, sparse-variance injection, lighting catch-up, bootstrap confidence, the 8-probe nearest-keeping grid overflow, the screen/grid fallback traces when the BVH is on, the `min(screenAO, nearVisibility)` coupling in the far field.

Keep: the 16x16 coverage-driven spawn, the MSME firefly gate, the Chebyshev visibility, the 6x6 guide map (if it passes WP10), the non-linear grid layout, the async stats readback.

## 8. Doubts and open items

- The reference is self-consistent, not independent. Its residual bias is unknown. WP0 item 5 is the fix.
- The "halved vs previous build" claim from the audit was not reproduced. We only show the current default is about 46% of its reference on Cornell.
- The unlabelled 1.1 ms in Execute is not isolated (WP0 item 4).
- Noise numbers for scene and NFS are upper bounds because GI-off frames vary too (section 3.5).
- The leak test is inconclusive (section 3.7). No leak number is claimed.
- Light lag, popping and transparency were not measured.
- Tier 0 Execute time (3.8 ms, above Medium's 2.7 ms) is unexplained.
- Stale-slot impact on energy was not measured. It affects young surfels, so its size depends on the spawn rate.
- Inventory findings (dead settings, the priority-pass no-op, `atomicMax` on `requestedRays`, `debug.z` clash, `missingFinalGI`, `common.glsl` slice growth) come from a read-only inventory worker. The key ones were checked by hand (RC6, RC7, RC11); the rest are marked as inventory findings.
- The previous audit's table has several labels that do not match the code: irradiance sharing is GIBS (p.116), not an extra; radial depth is marked "Fixed" in its own table but "Matches" in the summary; "8 vs 12 rays" compares stale slots.
- The team spec's numbers (threshold 0.85 +/- 0.075, MSME blends 0.02/0.35/0.6, radial EMA 0.05, final-gather form) are not in the paper. The plan uses them as the team's choice and checks them against the reference.
- Two validators fail on main, at their first assertion. Their checks no longer match the design. WP0 item 6 replaces them.

## 9. WP0/WP1 results

Branch `worktree-agent-a056a4952764d652b`, built from main at 4d24639. Before = that commit (built and run from its own source root). After = this branch. All runs: `NOX_FIXED_DT=0.0166666667`, manual exposure, static camera, 1920x1080, one GPU. Energy is over lit pixels, linear. Indirect reference = `NOX_GI_REFERENCE=1` (independent path tracer, section 9.2).

### 9.1 What changed

WP0: `NOX_EXPOSURE_MODE=manual|auto` (applied after scene load, `SceneRenderSettings.cpp`). `NOX_CAMERA_PATH=dolly:dx,dy,dz:frames|orbit:deg` (`src/CameraPath.{h,cpp}`, applied in `Core.cpp`). Counters: young surfels (age < 4), ray shortfall, grid overflow, stale-slot count, logged per frame. RT scene update timed under the `rt_scene_update` label, with CPU split and upload flags. Independent reference `reference_gi.comp`, sharing the BVH and direct-light code via `shaders/surfel_gi/surfel_scene_trace.glsl`. Harness `tools/surfel_gi_metrics.py`. The two failing validators are replaced.

WP1: integrate reads only the slots generate_rays wrote (cap `SURFEL_GENERATED_RAYS_PER_SURFEL`), with a surfel, slot and frame ownership check (trace stamps the frame into `hits.ids.w`). `requestedRays` has one atomic type: it starts at the reservation `max(maxSurfels/64,1)` from `begin_frame`, which replaces a racy `atomicMax`. `debug.z` has one writer (radius error). Slice growth reads the setting. Removed: `rayBins`, `GridAverages`, `SortedRays` and their bindings, `BinAndSortRays`, the no-op and unused `SurfelRayQueue` methods, `RayCounters` binding, `useRayBinning` (settings, UI, env), `radialDepthUpdateInterval`, `spawnPasses`, `fastFillSpawnPasses`, the `sourceDirect*` path (folded to its constant value, exact), `msme.zw` writes, the `sourceDirectSamples`/`rayHitSourceDirect` counters, the unused `SurfelGrid::Build`/`BuildCellAverages`. Mid-frame cell averages are gone: averages are built once, at the end of Execute, which is where the final gather reads them.

Not removed (deliberate): `spawnCandidateCount` is not a setting. It is a per-frame local that drives the spawn candidate count, so it stays.

### 9.2 Independent reference vs the self-consistent `ref`

Cornell, lit pixels (after build; `indep` is deterministic and identical across repeats):

| Run | Energy | / indep |
|---|---|---|
| indep (path tracer, 900 frames) | 0.2773 | 1.000 |
| ref, first after-run | 0.2717 | 0.980 |
| ref, repeat 1 | 0.2828 | 1.020 |
| ref, repeat 2 | 0.2805 | 1.012 |
| ref, before build (this section's baseline) | 0.2834 | 1.022 |

The 65k-ray `ref` and the independent path tracer agree to about 2% on the mean. The per-pixel correlation is weak: Pearson 0.68–0.71, RMSE 0.42 of the mean. So the mean agrees, but the spatial structure does not, and the independent reference itself has not been checked against an analytic solution. Treat the agreement as evidence that the surfel estimator is not biased low at convergence on Cornell, not as proof.

Reference limits: three diffuse bounces after the first hit, with direct light at every hit; hit albedo is the BVH material (no texture lookup), as in the surfel tracer; accumulation restarts on view change only, not on light change; quarter-resolution, bilinear-upsampled. Cost (Execute GPU time, one path per quarter-resolution texel per frame): 0.58 ms on Cornell, 18.0 ms on scene.json and 8.9 ms on NFS. `ref` is 17.0 ms on Cornell.

### 9.3 Before/after (single runs unless marked)

| Scene | Config | Before, energy (/indep) | After, energy (/indep) |
|---|---|---|---|
| Cornell | surf (1024 rays) | 0.1333 (0.481) | 0.1254 (0.452); repeats 0.1159, 0.1257 (mean 0.436) |
| Cornell | ref (65k rays) | 0.2834 (1.022) | 0.2717, 0.2828, 0.2805 (mean 0.2817, 1.016) |
| scene.json | surf | 0.1318 (0.460) | 0.1389 (0.485) |
| scene.json | both | 0.1252 (0.437) | 0.1344 (0.469) |
| NFS night street (manual) | surf | 0.00906 (0.351) | 0.00894 (0.346) |
| NFS night street (manual) | both | 0.00839 (0.325) | 0.00874 (0.339) |

Repeat spread (Cornell, 2 runs): surf energy 8.1% (fails 5%), both 4.8%, ref 0.8%, indep and ssgi 0.0% (deterministic). The surf spread comes from non-deterministic atomic ordering in allocation and spawn at 1024 rays. A single surf energy number is therefore only good to about 8%.

Stale-slot energy delta: not resolved at 1024 rays. Before/after differences (-9% Cornell surf, +5% scene surf, -1% NFS surf) are inside the run-to-run spread. A variant with the old integrate loop and everything else unchanged gave 0.418 (single run), also inside the spread. At 65k rays, where the estimate is well converged, the change is -0.6% (ref 0.2834 -> 0.2817), inside the 2% acceptance. The stale-slot counter reads 0 on every frame of every after-run: no unowned hit is consumed.

Noise (surf_term corrected std / mean, lit pixels): Cornell 3.65% -> 4.60% (repeats 4.01%, 3.05%); scene.json 0.70% -> 0.52%; NFS 0.47% -> 0.52%. The Cornell repeat spread is 27%, so Cornell noise is not resolved to better than about 1 point.

GPU time (`SurfelGIUpdatePass` median, surf): Cornell 1.452 -> 1.428 ms; scene.json 7.33 -> 6.42 ms; NFS 3.06 -> 3.10 ms. The 0.05 ms acceptance is not demonstrated on Cornell or NFS (repeat total GPU spread is 2.0%, so the Cornell change is within noise). The scene.json figures are about 2.5x the plan's 2.73 ms; the cause is not established.

RT scene upload: the plan's hypothesis (re-upload every frame on a static scene) is rejected. Uploads ran on 3 of 260 frames on scene.json (scene load) and 8 of 260 on NFS (BLAS build), and on 0 of 260 on Cornell. The `rt_scene_update` GPU label costs 0.001 ms per frame; the CPU instance-list rebuild costs 0.0002 ms. The unlabelled ~1 ms of Execute GPU time is therefore not the RT upload. It is still unexplained. Untested guess: GPU idle between dispatches while the CPU waits in `WaitForCompletion`.

Grid overflow (counter, last build of each frame, surf): Cornell 18,088 events per frame, scene.json 7,797, NFS 10,791. Events are inserts beyond the 64-entry cell capacity, so about a third of the 27-neighbour inserts saturate their cells on Cornell. This is the WP5 target (overflow 0 on Cornell and scene.json). Ray shortfall: 14,325 of 15,349 requested rays on Cornell (93%).

### 9.4 Verification

Pass: `tools/CheckNoRawGL.ps1`, `tools/CheckGenericExe.ps1`, `python tools/CheckVcxprojSources.py`, `scripts/validate_surfel_gi_static.py`, `scripts/validate_surfel_gi_correctness.py` (both replaced; they check the current design, not the old names). `NOX_EDITOR_SMOKE=1 NOX_EXIT_AFTER_FRAMES=150`: 0 FAIL. Racing regression: `[Race] Finished in 47.4994 s`. Camera path: `dolly:0.5,0,0:30` moves the view (mean difference 10.3 of 255 between frames 5 and 35).

Fail: `nox_tests.exe` reports 277 tests, 3 failed. All three are CHECKs in `test_asset_io.cpp` (loose tree vs packed fixture, one byte per line). Cause: the fixtures are LF in git (`i/lf`) and CRLF in this Windows working copy (`attr/text=auto`). The files are unchanged, so this is an environment issue that predates this work. Not fixed here.

### 9.5 Harness commands (verbatim; the next workers use these)

Cornell, with repeats, all configs:

    python tools/surfel_gi_metrics.py --exe build/vs2022-x64/Release/Nox_Engine.exe --scene scenes/surfel_gi_cornell_validation.json --out-dir %TEMP%/nox_surfel_wp0/cornell --configs off,ssgi,surf,both,surf_term,indep,ref --repeat 2 --table %TEMP%/nox_surfel_wp0/cornell.md --json %TEMP%/nox_surfel_wp0/cornell.json

scene.json and NFS (the temp manual-exposure copy is made and removed by the harness):

    python tools/surfel_gi_metrics.py --exe build/vs2022-x64/Release/Nox_Engine.exe --scene scenes/scene.json --out-dir %TEMP%/nox_surfel_wp0/scene --configs off,ssgi,surf,both,surf_term,indep --table %TEMP%/nox_surfel_wp0/scene.md --json %TEMP%/nox_surfel_wp0/scene.json
    python tools/surfel_gi_metrics.py --exe build/vs2022-x64/Release/Nox_Engine.exe --scene scenes/nfs_night_street.json --out-dir %TEMP%/nox_surfel_wp0/nfs --configs off,ssgi,surf,both,surf_term,indep --table %TEMP%/nox_surfel_wp0/nfs.md --json %TEMP%/nox_surfel_wp0/nfs.json

Baseline from another commit: add `--cwd <its source root>` (the exe needs `shaders/`, `config/` and `CMakeLists.txt` beside it), and reuse an `indep` run with `--reuse` where its directory is copied in. Dropping `indep` and `ref` saves most of the time; `ref` is 900 frames.

### 9.6 Not done

WP2 and later: not started, as instructed. The stale-slot energy effect is not measured above noise, so the 2% energy check is inconclusive for surf. Grid overflow is measured but not fixed (WP5). The ~1 ms unlabelled Execute GPU time is unexplained. The `ref` vs `indep` per-pixel correlation (about 0.7) is unexplained. `shaders/includes/surfel_gi_resources.glsl` is dead (not included anywhere) but kept: vcxproj lists it and two validators reference it. `cell_average.comp` and `clear_grid.comp` stay until WP5. Other legacy validators (for example `validate_surfel_gi_irradiance_signal.py`, `validate_surfel_coverage_priority.py`) and `tests/test_surfel_gi_quality.py` reference removed names; they were not run or updated. Repeat-spread acceptance fails for surf energy and for noise. `nox_tests.exe` has 3 environment-related failures.

## 10. WP3 / Execute gap results

Worker C. Branch `worktree-agent-a6d1045c1c551ea9e`, merged with main at d447ec2. Result in one line: the Execute gap is an accounting error, the scene.json anomaly is GPU contention, and none of the WP3 changes tried gave a measurable speedup, so the engine code is unchanged.

Conditions: §9.5 flags (frames 200-260, manual exposure, static camera, 1920x1080, `NOX_FIXED_DT`). The GPU was shared with other engine processes during every run (the user's editor and other workers' harnesses). GBufferPass, which the surfel code does not touch, ranged from 0.35 ms to about 1.0 ms per frame and had spikes to 60 ms, so absolute GPU times carry 2-3x contention noise. Medians are per run; variants were compared interleaved and by minimum where stated.

### 10.1 Execute gap: accounting error, nothing to remove

§3.3 and §9.3 put about 1.1 ms of GPU time inside Execute outside every label. The comparison was wrong. `SurfelGIUpdatePass` is one render-graph pass that runs BeginFrame, Execute and ApplyIndirect (`src/ModularRenderer.cpp:872-886`), so its total contains the ApplyIndirect labels. The derivation removed those labels from the label sum but not from the total, and compared a median with a sum of means. Same-run, per-frame comparison (mean over frames 200-260, `SurfelGISubpassMetrics` totals against `SurfelGIUpdatePass`):

| run | pass total (ms) | sum of labels per frame (ms) | gap (ms) |
|---|---|---|---|
| scene r1 | 14.481 | 14.452 | +0.029 |
| scene r2 | 3.160 | 3.130 | +0.030 |
| scene r3 | 12.317 | 12.277 | +0.041 |
| Cornell r1 | 1.585 | 1.558 | +0.027 |
| Cornell r2 | 3.320 | 3.283 | +0.036 |
| Cornell r3 | 2.002 | 1.963 | +0.039 |
| NFS r1 | 13.938 | 13.938 | 0.000 |
| NFS r2 | 10.009 | 9.982 | +0.028 |
| NFS r3 | 7.550 | 7.524 | +0.027 |

The gap is 0.00-0.04 ms in every run, under 1% (Cornell at most 2%). Before and after are the same, because no Execute block changed. Per-block GPU time for scene r2 (the quietest scene run), mean over the 61 frames, absent labels counted as zero:

| block | ms | block | ms |
|---|---|---|---|
| final_gather (ApplyIndirect) | 0.903 | ray_allocation | 0.040 |
| tracing | 0.443 | cell_averages | 0.034 |
| composite_filter (ApplyIndirect) | 0.356 | temporal_resolve (ApplyIndirect) | 0.030 |
| coverage_spawn | 0.290 | ray_request | 0.026 |
| recycling | 0.245 | temporal_integration | 0.021 |
| upscale_filter (ApplyIndirect) | 0.199 | radial_depth | 0.015 |
| irradiance_sharing | 0.159 | irradiance_snapshot | 0.013 |
| ray_generation | 0.131 | transform_update | 0.013 |
| grid_build | 0.092 | debug_overhead, begin_frame, grid_clear, coverage_reset, dispatch_args, rt_scene_update | 0.036 |
| coverage_project | 0.054 | total of labels | 3.100 |

CPU side: `SurfelGIUpdatePass` cpuMs mean 0.32-0.44 ms and cpuWaitMs 0. The counter and timing readbacks are delayed three frames and polled with `IsQueryResultAvailable`, so nothing waits on the CPU in the frame. The §9.3 hypothesis (GPU idle while the CPU waits) is not supported.

Harness note, not changed: `tools/surfel_gi_metrics.py` looks for labels containing "Execute" (none exist, so its "Execute median" row is n/a), and its "sum of labelled passes" adds medians taken over different frames. Use per-frame totals as above.

### 10.2 scene.json 6.4-7.3 ms: contention, not confirmed as a regression

Same binary, same scene, same session (baseline runs, three repeats):

| repeat | SurfelGIUpdatePass gpu median (ms) | tracing median (ms) | GBufferPass median (ms, not touched by surfel code) |
|---|---|---|---|
| 1 | 6.50 | 1.08 | 1.03 |
| 2 | 3.09 | 0.37 | 0.35 |
| 3 | 6.89 | 1.09 | 1.04 |

The quiet repeat reproduces the plan's figures (2.73 ms pass, 0.37 ms tracing). The two contended repeats are about 2.2x slower overall and about 2.9x slower in GBufferPass, which contains no surfel work, and show spikes of 60-120 ms in unrelated passes. That pattern is GPU time-slicing with other clients. A code regression cannot be separated from contention without a quiet GPU, so quote scene.json figures with the GPU load, and do not treat 6.4-7.3 ms as a surfel cost.

### 10.3 Where the tracing time goes (per 1024 rays, quiet runs)

Each row turns off one part of the hit path (diagnostic only, not kept):

| variant | scene r1 (ms) | NFS r1 / r2 (ms) |
|---|---|---|
| full | 0.409 | 0.639 / 0.664 |
| no direct light at hits | 0.401 | 0.622 / 0.611 |
| no cached indirect (grid walk) | 0.263 | 0.437 / 1.109 (contended) |
| no albedo lookups (grid walk) | 0.300 | 0.504 / 0.515 |
| no hit shading | 0.204 | 0.359 / 0.356 |
| no BVH walk (all rays miss) | 0.010 | 0.009 / 0.009 |
| BVH walk only (no shading, no albedo) | - | 0.212 / 0.202 / 0.221 (three runs) |

On NFS the BVH walk is about 0.21 ms (35%), albedo lookups about 0.09 ms (15%), the cached-indirect grid walk about 0.16-0.20 ms (30%), direct light about 0.02 ms (3%), and the rest about 0.1 ms. The 0.22 ms per 1024 rays for scene.json in the brief was not reproduced: quiet runs give 0.37-0.41 ms.

### 10.4 WP3 items tried

- **Ray binning** (WP3.1): count, scan and scatter passes, 256 hash buckets x 8 direction octants, keyed by the ray origin's grid cell and the ray direction; trace in bin order. Built and run. Scene 0.385 ms against 0.378 base; NFS 0.59-0.61 against 0.58-0.60. No measurable gain, so removed (the brief says keep it only if it helps).
- **Hit lighting** (WP3.2): directional light evaluated exactly; local lights above eight are resampled, one shadow test for the winner; exact below that. Two variants:
  - 8 uniform candidates (RIS): NFS energy ratio to indep 0.255, against 0.36-0.37 with the exact loop (about -30%). Rejected.
  - Weighted reservoir over all local lights, weight = unshadowed luminance: 8 runs, mean energy 0.00902 against 0.00951 for the exact loop (-5%, outside the 3% limit). Speed: no change (NFS 0.63 against 0.64 ms). Rejected. Probable cause: a visible light's sample is scaled by the total weight, so more samples reach the integrator's 32-unit clamp (`integrate.comp:51`), which removes energy. The paper's 4-8 candidates target scenes with over 1000 lights; here the 32-light cap makes the full pass cheap, but the variance still reaches the integrator.
  - Not shipped. The shader and host code were removed, not left as an opt-in with a known bias.
- **Software BVH tMax** (WP3.3): NFS at 64, 128 and 256 m gave 0.59, 0.60 and 0.61 ms. No gain. The plan's `min(256, 2 * farScale)` evaluates to 256 and changes nothing. A shorter tMax also turns geometry beyond it into sky (see the comment at `trace_rays.comp` `TraceSoftwareBVHRadiance`), so it is not adopted.
- **BVH micro-optimisations** (not in the plan; found while reading the walk): vertex-only triangle test, inverse direction once per ray, TLAS instance walks capped at the closest hit so far. Built, neutral (scene 0.408 against 0.378; NFS 0.60-0.62 against 0.58-0.60). Reverted.
- **Fallbacks** (WP3.4): the screen-space and grid traces are already not run while the software BVH is on. `uBVHTraceStride` is 1, so `useBVHThisRay` is true for every ray (`trace_rays.comp`, `TraceSurfelGrid`). Removing them saves nothing. They stay for frames before the BVH is built and for tiers without it (Low disables the BVH).

### 10.5 Energy and noise

No engine code is kept, so the energy and noise are those of the plan's §9.3 after-WP1 build: NFS surf energy 0.353 (ratio 0.345-0.369 over three repeats), indep 0.0258, NFS surf_term noise 0.52%. The reservoir variants above were measured against the exact loop on NFS only; scene.json and Cornell have at most three and one light, so they take the exact branch in any case.

### 10.6 Not met

- Tracing per 1024 rays halved on scene.json and NFS: not met, and no kept change reduces it by 10% or more. On NFS the costs are the BVH walk (about 0.21 ms) and the per-hit grid walks (about 0.3 ms: cached indirect and cached albedo, 27 cells, up to four surfels per cell). Both are latency-bound: 1024 rays fill 16 of 68 SMs, and each ray is a chain of dependent loads. Halving needs fewer dependent grid reads per hit (fewer neighbour cells, a per-cell budget, or the WP5 cell averages). That changes the multi-bounce energy and needs the grid owner, which is why it was not done here. The walk also needs a wider or ordered BVH, or several rays per thread.
- Execute gap: explained (section 10.1). There is no unlabelled GPU time to remove.
- scene.json anomaly: explained as contention (section 10.2). A regression is not excluded without a quiet GPU.

### 10.7 Verification

- `tools/CheckNoRawGL.ps1`, `tools/CheckGenericExe.ps1`, `python tools/CheckVcxprojSources.py`: pass.
- `scripts/validate_surfel_gi_static.py`, `scripts/validate_surfel_gi_correctness.py`, `scripts/validate_surfel_gi_texture_dump.py`: pass.
- `scripts/validate_surfel_gi_fallback_contract.py`, `_irradiance_signal.py`, `_ray_radiance_contract.py`, `_stability_contract.py`: fail on HEAD before this work. They check `apply_indirect.comp`, `cell_average.comp` and `spatial_filter.comp` text, a Medium quality-cap table, a ray-miss rule, and screen-space-before-BVH trace order, which HEAD does not have. Not changed.
- `NOX_EDITOR_SMOKE=1 NOX_EXIT_AFTER_FRAMES=150`: 0 FAIL.
- Racing: `[Race] Finished in 47.4994 s (new record)`.

## 11. WP2/WP7/WP10 results

Branch `worktree-agent-acf4c659e250f2d39`, merged with main at d447ec2. Method as section 9: 1920x1080, manual exposure, static camera, frames 200-260, lit pixels, energy against the independent reference (`indep`, section 9.2). "Before" is the merged tree built unmodified and run from a frozen copy. Main tables are means of 3 repeats for `surf` and its twins. `off`, `ssgi` and `indep` are the baseline runs, reused (their code path is unchanged). Single-repeat numbers are marked. Run-to-run spread is about 8-15% on energy and 16-34% on noise, so differences smaller than that are not claims.

### 11.1 What changed

- WP2 requests (`request_rays.comp`): `r = clamp(ceil(2 cv), 1, 8)`, `cv = sqrt(lum shortM2) / max(lum shortMean, 1e-3)`. Young (age < 4): `max(r, 4)`, a floor per the plan, not a cap (the two differ only when r > 4). Dormancy: `cv < 0.05` and age > 32 requests 1 ray on frames where `(id + frame) % 8 == 0`, else 0. A light change (factor > 0.03) raises every surfel to at least 2 rays, after dormancy, so it wakes dormant surfels.
- WP2 allocation (`allocate_rays.comp`, `SurfelRayQueue` scratch buffer, binding 36): when demand exceeds the budget, largest remainder in three dispatches. Phase 0 sums floor quotas and fills a 1024-bin histogram of remainders. Phase 1 (one group) finds the threshold bin. Phase 2 grants each surfel its floor plus one ray when its remainder is above the threshold, or in it within the tie quota. Ranges are contiguous (atomic reservation on `allocatedRays`). The grants sum to the budget exactly. Demand at or below the budget is granted in full. The priority pass is removed.
- `requestedRays` no longer starts at `max(maxSurfels/64, 1)` (1024 at Medium). Before, that phantom reservation was inside `requestedRays`, so the requested counts in section 9 and the "before" rows here are 1024 too high. The before ratios below use the true demand (requested minus 1024).
- Budgets (`ModularRenderer.cpp`, `RenderContext.h`, `SurfelGIManager.h`): Low 1024, Medium 2048, High 4096, Ultra 8192 as defaults. `surfelGIMaxRayBudget = 0` means the tier default. An explicit value is used as given, with no tier cap. The resolved budget is no longer written back to the context, so a tier change takes effect.
- WP7 estimator (`msme.comp`, `integrate.comp`): two-rate estimator. Short mean (rate 0.2) and short variance from batch means, long mean (the gather input). New surfel: long-mean rate at least 0.6. Lighting change L: rate at least 0.5 L, and the short mean snaps to the batch when L >= 0.5. The firefly gate is kept on the batch mean and skipped during a lighting change. Removed: sparse-variance injection, lighting reactive variance, catch-up blend, bootstrap confidence. **Deviation:** the long rate is fixed at 0.02 in steady state, not `mix(0.02, 0.35, reactive)` (section 10.4).
- WP10 (`irradiance_sharing.comp`): neighbour cap 16 (was 48). Gate: blend only where `(std/mean)^2 >= 0.09`, or where confidence is at most 0.5. The gate is relative, because an absolute lum(shortM2) threshold is not scale-free. Guiding is kept on (section 10.5).
- Light-step hook (generic): `NOX_LIGHT_STEP=<frame>:<scale>` in `LightManager::UpdateLights`. It multiplies every registered light's intensity once, at that call. Log line `[LightStep]`.
- Bug fixed in `generate_rays.comp`: `hasGuide` was read uninitialised when guiding is off. The guiding-off collapse (11.5) happens in the baseline too, so the fix does not explain it.
- Harness (`tools/surfel_gi_metrics.py`): twins `surf_term_*` (noise) for the ablations and budgets; `surf_noguide`, `surf_nosharing`, `surf_r8k`; noise is computed for every `surf_term*` config.
- Validator update: `scripts/validate_surfel_gi_correctness.py` checks the new contract (reservation 0, three-phase allocation, scratch binding). The static validator is unchanged and passes.

### 11.2 Before and after (surf, 3 repeats; surf_term noise)

| Scene | Config | Energy | Ratio vs indep | Noise (surf_term, rel) | Requested | Allocated | Alloc / requested |
|---|---|---|---|---|---|---|---|
| Cornell | before | 0.1226 | 0.442 | 3.78% | 14326 | 1024 | 0.071 |
| Cornell | after | 0.2764 | 0.997 | 2.58% | 5913 | 2048 | 0.346 |
| scene.json | before | 0.1152 | 0.464 | 0.49% | 13151 | 1024 | 0.078 |
| scene.json | after | 0.1553 | 0.625 | 1.06% | 2263 | 2048 | 0.905 |
| NFS night street | before | 0.00881 | 0.341 | 0.49% | 18961 | 1024 | 0.054 |
| NFS night street | after | 0.01034 | 0.401 | 0.53% | 8381 | 2048 | 0.244 |

Other rows (after, 3 repeats), ratio vs indep: `both` 0.577 (Cornell), 0.597 (scene), 0.371 (NFS). `surf_noguide` 0.099, 0.446, 0.381. `surf_nosharing` 1.327, 0.635, 0.415.

Cornell budget replicate (second independent set of 3 repeats, fresh runs; a junction mistake meant these were not reused): `surf` 2048: energy 0.2739 (0.988), noise 3.42%. `surf` 4096: 0.2984 (1.076), noise 3.38%. `surf` 8192: 0.2574 (0.928), noise 3.82%. Demand: 5833, 5050, 4660; allocated 2048, 4096, 4660.

### 11.3 Targets

| Target | Result |
|---|---|
| Energy in [0.9, 1.1], Cornell, scene, NFS | Cornell met (0.997; replicate 0.988). Scene not met (0.625). NFS not met (0.401). |
| Allocated / requested >= 0.3, Cornell | Met (0.346). |
| Noise <= 2%, Cornell | Not met: 2.58% (set A), 3.42% (set B). |
| Noise <= 1.5%, scene | Met (1.06%). |
| Light step: 90% in <= 12 frames | Not established (11.6). |
| Tracing cost at 2048 | Not reliably measured (11.7). |
| Racing `[Race] Finished in 47.4994 s` | Met. |
| Editor smoke, 0 FAIL | Met. |
| `CheckNoRawGL`, `CheckGenericExe`, `CheckVcxprojSources`, static validator, correctness validator | All pass. |
| `nox_tests.exe` | 277 tests, 3 failed: the known `test_asset_io` CRLF failures from section 9.4. No new failures. |

### 11.4 Why the energy misses on scene and NFS (single repeats unless marked)

The lead's question was whether allocation, dormancy or per-ray cost limits energy. The answers are below. Allocation efficiency is high on scene (0.91) and low on NFS (0.24), but raising the budget does not help scene at all.

- Budget. Scene: 4096 gives 0.625, 8192 gives 0.622. Demand is 2.1k-2.3k, so every requested ray is already granted at 2048, and more budget cannot add rays. NFS: 4096 gives 0.545, 8192 gives 0.515. NFS is budget-limited at 2048 (demand 8.4k), but the plateau comes well below the band.
- Dormancy. Off on scene: 0.636 (demand 2570). Off on NFS: 0.416. Not the limit.
- Long rate. 0.05 on scene: 0.759. On NFS: 0.428. Cornell at 0.05: 1.196, with noise 4.32%. Convergence lag is part of the scene gap, but no single long rate meets all three scenes with this estimator.
- Reactive term (the plan's formula, taken literally). Cornell: 1.158 with noise 11.36%. The absolute short variance drives the rate toward 0.35 on irradiance of order 0.3. Fixed at 0.02: 0.983, noise 3.27%.
- Per-ray clamp (`SURFEL_MAX_SAMPLE_IRRADIANCE`, 32). Raised to 1000: NFS 0.874 (in band), scene unchanged at 0.624, Cornell overshoots to 1.87. So NFS's shortfall is clamp bias from emitters. The clamp is a firefly trade-off that also clips rare emitter hits on Cornell, where clamping currently happens to land on the reference. The clamp needs a scene-relative definition, which I did not design here.

The energy target therefore holds on Cornell only. For scene and NFS the next levers are the request floor for converging surfels (the rule asks for too few rays once cv is low, even while the long mean is still moving) and a scale-aware clamp. Both change the plan's rules, so I did not change them without the lead's decision.

### 11.5 WP10: guiding and sharing

- Guiding off at equal rays (3 repeats, 2048 rays): energy 0.099 of indep on Cornell, 0.446 on scene, 0.381 on NFS, with negative correlation on Cornell. The baseline shows the same collapse (Cornell 0.17 of indep, single repeat). Cosine-sampled rays rarely hit the emitter, and the per-ray clamp removes the rare hits that do land. Guiding aims rays at the light, which avoids both. The variance criterion ("keep only if variance drops by 10%") cannot be applied: the unguided estimate is biased low by a factor of 3 to 10, and relative noise on a mean that small is not comparable. **Guiding is kept on**, justified by the energy, not by a measured variance reduction.
- Sharing on (cap 16, relative gate) against off (3 repeats): Cornell noise 2.58% against 4.25%, and the second set gives 3.42%. Scene 1.06% against 1.09%. NFS 0.53% against 0.61%. Energy: Cornell 0.997 against 1.327, scene 0.625 against 0.635, NFS 0.401 against 0.415. **Sharing is kept**, with the cap and the gate. The benefit is on Cornell only, and it is not larger than the replicate spread once the second set is included. The sharing cost was not measured reliably (11.7).

### 11.6 Light step

The hook works: `[LightStep] frame=230 scale=2` fires once. The full composite brightens from 0.0546 (frame 230) to 0.0796 (frame 234), then falls back to about 0.066 by frames 246-249. The surfel term (`COMPOSITE_DEBUG=4`) goes from 0.205 before the step to a peak of 0.253 at +7 frames, then back to 0.200 by +55. So the step is not held in the output, and I could not find why in this pass. The 90%-response metric is therefore **not established**. Measured only: the surfel term reaches 90% of its transient excursion at +7 frames. That is not the step-change metric the plan asks for. The ratio 0.97 against the expected 2.0 confirms the step is not held.

### 11.7 Timings

GPU timings in these runs are contaminated. Other workers were using the GPU during both before and after runs. The same Cornell `grid_build` ranges from 0.07 to 0.99 ms across configs, and the baseline scene `tracing` is 1.44 ms against the plan's quiet-machine 0.37 ms. So the "tracing at 2048" target (<= 0.25 ms on scene) is not reported as a result. `irradiance_sharing` on scene reads 0.14-0.77 ms in the after runs, against 0.16 ms before, and the spread is the same kind of contamination. Re-measure on a quiet GPU before using any cost number from this section.

### 11.8 Not done

- The light-change factor in `SurfelGIPipeline.cpp` (around line 966) is unchanged; the plan's simplification was not made.
- `update_surfels.comp` still writes `msme.x`, `msme.y` (WP7 removed their readers). Dead writes, owned by another worker.
- The sharing phase count (`budget <= 4096 ? 2 : 1`) is unchanged, so at 2048 sharing covers half the surfels per frame.
- The request rule has no sample-count floor, so converging surfels can be starved (11.4).
- Not done: light-step response on a quiet GPU, the step with the raw gather (debug view 35), an independent per-ray variance measurement for the guiding decision.


### 11.9 A follow-up (round 2)

Commits on `worktree-agent-acf4c659e250f2d39`: `98118f3` (light-step hook: persistent gain and GPU re-upload), `58bd127` (guide sampling inside the unit disk), `53fc516` (cumulative average for the early long mean), and the commit that adds this subsection. Method as 11.2: 3 repeats for `surf` and `surf_term`, with `off`, `ssgi` and `indep` reused from the section 9 baseline runs. "Before" is the round-1 after (11.2). "After" is `53fc516`.

| Scene | Metric | Before (11.2) | After (53fc516) | Target |
|---|---|---|---|---|
| Cornell | energy / indep | 0.997 | 0.935 | [0.9, 1.1]: met |
| Cornell | surf_term noise | 2.58% | 3.52% | <= 2%: not met, worse (replicate sets gave 2.6-3.4%) |
| Cornell | allocated / requested | 2048 / 5913 | 2048 / 5902 | |
| scene.json | energy / indep | 0.625 | 0.668 | [0.9, 1.1]: not met |
| scene.json | surf_term noise | 1.06% | 0.71% | <= 1.5%: met |
| scene.json | allocated / requested | 2048 / 2263 | 2024 / 2024 | demand now fits the budget |
| NFS | energy / indep | 0.401 | 0.363 | >= 0.85: not met, worse |
| NFS | surf_term noise | 0.53% | 0.53% | |
| NFS | allocated / requested | 2048 / 8381 | 2048 / 8662 | |

**1. Light-step hook.** The step is now a read-path gain (`g_noxLightStepGain` in `BaseLight::GetIntensity` and `GetEffectiveColor`) from the step frame on, so a stored intensity re-applied each frame cannot undo it. The step frame is also marked dirty. Before, the step was applied before the dirty snapshot in `LightManager::UpdateLights`, so it never reached the packed GPU light buffer. I did not confirm that authored intensities are re-applied each frame; the gain makes that question moot. **The 90% measurement is not valid on these scenes.** The registered lights hardly drive the lit output: with all lights at zero, the Cornell composite moves from 0.109 to 0.114 (mean linear), scene.json from 0.392 to 0.389, and NFS (44 lights) from 0.0124 to 0.0103 (-17%). So there is no steady state to measure against. The only surf_term response measured on Cornell, with the light doubled at frame 230: 0.541 before, peak 0.647 at +5 frames, 0.578 at +29 (+7% over before). On scene.json the change is +1.4%. The 12-frame target is not established, and the peak should not be read as a pass. Lead for the lighting owner: the lit output does not follow the registered light intensities here, so something else feeds the lighting pass.

**2. Scale-aware firefly clamp.** I implemented a per-sample bound of `k * max(lum long, lum short) + m * sigma` (sigma from the short variance), with a bound of 64 for surfels with fewer than 8 rays. Results at 2048 rays, one repeat each, frames 200-260:

| Sample bound | Cornell energy / indep | NFS energy / indep |
|---|---|---|
| absolute 32 (shipped) | 0.96 (cumulative only) | 0.41 |
| absolute 1000 | 1.87 | 0.87 |
| k=8, m=8, cumulative | 1.89 | 0.77 |
| k=4, m=4, cumulative | 1.47 | 0.76 |
| k=8, m=8, batch gate off | 1.93 | 0.86 |
| k=8, m=8, with the guide fix | 1.70 | 0.68 |

No setting meets both NFS >= 0.85 and Cornell in [0.9, 1.1]. A long capture (frames 620-660, k=8, m=8) gives Cornell 1.89, the same as frames 200-260, so the overshoot is a steady-state bias, not convergence. The guide fix is a real bias fix (commit `58bd127`): the old sampler projected the square's corner samples onto the disk rim while using the full-square density, which overweighted near-horizon directions. It lowers the k=8 overshoot from 1.89 to 1.70, but the remaining overshoot is not located. I shipped the absolute 32 bound unchanged. The scale-aware bound is not in the code. The NFS target is not met (0.36). Fireflies: I read the follow-up frames at 260 for Cornell and NFS; there are no speckle or firefly artefacts.

**3. scene.json convergence.** Age histogram at frame 240 (1836 live surfels): under 4 frames 32, 4-8 16, 8-16 48, 16-32 80, 32-50 96, 50-100 270, 100-200 524, 200 and over 770. Share of irradiance from surfels younger than 50 frames (a population proxy: the sum of E per age bucket, not the gather weights): 15% at frame 240, 8-9% at 250-260. On Cornell it is 9% at 240. Surfels of 200 frames and older carry 41%. Convergence lag can therefore account for at most about 15% of the scene gap (0.38 of indep); it is not the main cause. The cumulative schedule was neutral on energy at one repeat (Cornell 0.96, NFS 0.41). With the schedule and the guide fix, scene.json reaches 0.668 at 3 repeats, up from 0.625. Churn, for worker B: in frames 200-260 on scene.json there are about 1.6 spawns per frame (after the first readback, which counts every live surfel as new) and 0.2 recycles per frame (13 in 61 frames: 5 from ages 50-100, 1 from 100-200, 7 from 200 and over). Churn is small and falls on old surfels, so it is not the energy problem. Next suspect, not tested (trace is worker C's file): the surfel path adds sky irradiance through a sky-visibility term, while the reference gets sky from path-traced misses with the environment map. scene.json has a sky and Cornell does not.

**4. Cornell noise.** Not met: 3.52% in this round, and 2.58% and 3.42% in earlier sets. A long rate of 0.01 (one repeat) gives noise 2.37% and energy 0.556, so the long-mean average is not the main noise source. Budgets of 4096 and 8192 did not lower it (11.4). Churn is negligible in the window (above). Not tested: the gather's candidate subsampling and the screen temporal and upscale filters, which are outside my files.

**Timing.** No timing A/B pairs were run in this round.

**Not met or not established:** the NFS energy target (0.36, need >= 0.85); the scene-adaptive clamp; the Cornell noise target (3.52%); the light-step 90% response (not measurable on these scenes); scene.json energy (0.67, below band); the remaining Cornell overshoot under the scale-aware bound.
## 13. Estimator correctness

Worker D (Haiku 5.5). Built on round 2 (`ed55deb`). Result in one line: the measurement chain was not linear, so every earlier energy number here is suspect; the independent reference now uses Russian roulette, and four estimator bugs (B1 to B4) are fixed; the furnace and white-sky tests, the guiding and clamp invariances, and the scene.json and NFS energy targets are not met; an offset in the reference-mode output path is not explained.

### 13.1 Measurement (found first; it changes every earlier energy)

- The dumps are display-referred. The post-process applies the GT tone curve by default, plus bloom, grain and dither, and TAA and HBAO are on in the composite. The harness converted the PNG with the sRGB LUT and called the result linear, which was wrong. Evidence: with the same code, Cornell indep (composite minus off) is 0.2773 under the default tone curve and 0.0743 with the linear profile below. The emission-only composite is exact under the linear profile (furnace `off`: 0.0999 for LE 0.1, 0.25016 for LE 0.25).
- `tools/surfel_gi_metrics.py`: `MEASUREMENT_RENDER_SETTINGS` (tonemap none, bloom, vignette, grain, chromatic aberration, dither, TAA and SSAO off) is written into a temp copy of every scene. All numbers below use it.
- `indep` and `surf` energies are composite minus `off`. On sky-lit scenes that subtracts the deferred IBL, which `off` contains, so it is not the GI term. New configs `indep_term` (reference alone, composite debug 4) and the existing `surf_term` give `term_energy` and `term_ratio_vs_indep`. Ratios in this section use those.
- The "Execute" row never matched a label. `sum_labels_ms` is now the median of per-frame sums (the old value was a sum of medians over different frames, plan 10.1). `execute_ms` stays n/a.

### 13.2 Bugs (file:line at `ed55deb`)

B1. Sky miss read mip 5 of a cube with five prefiltered levels (`surfel_scene_trace.glsl`, `SURFEL_ENVIRONMENT_MISS_MIP = 5.0`; log: "Prefiltered map generation completed (128 base, 5 mips)"). Now mip 0 (exact HDR). The same function clamped sky radiance to 64 ("firefly guard"), which biased bright skies low; removed. The energy effect of B1 is not isolated.

B2. The reference stopped after 3 bounces (`reference_gi.comp`, `for (bounce = 0; bounce <= uBounces; ...)`). Now Russian roulette after the third hit with q = min(0.95, max albedo), division by q, cap 32 (the cap drops a residual of albedo^32; 0.19 at albedo 0.95).

B3. Emitted radiance in the irradiance slot. `integrate.comp` floored each emissive surfel at its previous mean (`max(batchMean, emissiveBaseline)`), a one-way ratchet; removed, and the slot is E only. `spawn.comp:715` still seeds emission into the slot (worker B's file, not changed; it is overwritten by the first batch on reset). On grid-path hits (tiers without the BVH) an emitter returns no emission now; the BVH path reads it from the material.

B4. Emission strength. `trace_rays.comp` used `max(emissiveStrength, 1.0)`; the reference and the deferred pass use the product. Fixed to the product (emitters with strength below 1 were brightened).

B5. Default light fallback. `SurfelGIPipeline.cpp` (surfel and reference update, about lines 965 and 1604) used a white unit sun when no directional light was found. The engine always registers `MainDirectionalLight`, so this rarely applies; the fallback is now zero. Low impact, not a measured fix.

B6. Per-sample firefly clamp 32 (`integrate.comp`, `SURFEL_MAX_SAMPLE_IRRADIANCE`). The dominant Cornell bias. The light is emissive (cornell_box.glb, KHR strength 384.8), so an emitter sample is about 1200 and is clipped to 32. Guided Cornell, 262 frames: clamp 32 gives 0.2469, clamp 1e6 gives 0.5557 (+125%). **Not fixed.** The value stays at 32 because it agrees with the reference (0.2420, ratio 1.02). The unclamped overshoot (2.3x the reference) is not explained, so an upward bias in unclamped samples remains. A scale-aware clamp was not implemented.

B7. Guided pdf rejection (`generate_rays.comp` `SurfelSampleGuideMap`, 64 tries). Corner bins (area 0.0032) fail about 15% of the time; the mixture pdf then does not match the sampling density. Small. Guided and unguided agree on the furnace (0.128 vs 0.133, 3.5%) and on white-sky (0.145 vs 0.147, 1.3%), so the pdf is consistent for smooth fields.

### 13.3 Open findings (not located)

O1. Surfel furnace is low. `surf(furnace) / surf(white-sky) = 0.884` against the analytic 1.667 (E = pi LE/(1-A) = 1.571 against pi L = 0.943), so the surfel furnace is 0.53x. The sky-only path is consistent. Hypothesis, not verified: multi-bounce at hits comes from `TraceCachedIndirectAtSurface` (`trace_rays.comp`, about 261-334). It returns false when the summed weight is at most 1e-4, and the sky fallback is zero in the furnace, so the indirect term is dropped at many hits.

O2. Reference-mode composite offset. On the white-sky plane with no sky (`whitesky_nosky`), `indep_term` = 0.102594 (sky-independent). The value is identical (0.102594) with the reference mean forced to 0 (forcing it to 1 gives 0.2586, so the composite does read the mean), and the surfel path gives 0.0 on the same scene. Modes 2, 3 and 5 (IBL, SSGI, LPV) are 0.00003. The furnace at LE 0 gives 0.0221. The source is in the reference-mode output path, not in the reference mean; it was not traced. It enters every `indep` / `indep_term` number that the composite carries.

O3. Sky slope. With the sky scale doubled, the surfel path doubles (0.145 to 0.292). The reference slope is 0.01561 per texel, the surfel slope 0.01467, so the reference is 6.4% higher. Absolute values need O2 resolved.

O4. Guiding off collapses Cornell (0.0201 against 0.2469 at clamp 32; 0.0775 against 0.5557 at 1e6). Not converged in 262 frames: an unguided path rarely hits the emitter, and no light sampling exists. Not a pdf error (B7).

### 13.4 Analytic tests

Scenes: `tools/surfel_gi_analytic_scenes.py` (writes into `build/surfel_gi_analytic`, git-ignored; the engine only opens scene files under the project root). Furnace: closed box, albedo 0.5, emission 0.25, no sky. White-sky: one upward plane, albedo 0.5, sky texel 10 with scale 0.03 (L = 0.3, E = pi L = 0.943).

| Test | Expected | Measured | Result |
|---|---|---|---|
| Furnace surf / white-sky surf (guided) | 1.667 | 0.884 | Not met (O1) |
| Furnace guided / unguided | 1 | 0.966 | within 3.5% |
| White-sky guided / unguided | 1 | 0.987 | within 1.3% |
| Sky linearity, surf (texel 10 / 20) | 2.00 | 2.01 | met |
| Sky linearity, reference (texel 10 / 20, offset removed) | 2.00 | 1.60 raw, slope 2.00 after the O2 offset | not met as measured (O2) |
| Absolute surf vs analytic | needs O2 | not established | not measured |

Reference convergence: the white-sky reference is converged at frame 20 (`final_00020` to `final_00899` identical to 5 digits).

### 13.5 Invariants (Cornell, 262 frames, one repeat, medium budget 2048)

| Invariant | Target | Result |
|---|---|---|
| Guiding on vs off, same mean | within 3% | Not met: 0.2469 vs 0.0201 (clamp 32); 0.5557 vs 0.0775 (clamp 1e6) (O4) |
| Clamp 32 vs 1e6, same mean | within 5% | Not met: 0.2469 vs 0.5557 (+125%) (B6) |
| Reference vs surfel, Cornell | within 5% | 1.02 (surf 0.2469, reference 0.2420) |

### 13.6 Default tier (medium, 2048 rays, one repeat)

Reference = `indep_term` (900 frames, composite debug 4). Noise = `surf_term` corrected std / mean on lit pixels.

| Scene | Reference term | Surfel term | Ratio (target 0.9-1.1) | Noise (target) |
|---|---|---|---|---|
| Cornell | 0.2420 | 0.2469 | 1.02 met | 2.56% (<=2%: not met) |
| scene.json | 0.3854 | 0.1038 | 0.27 not met | 0.94% (<=1.5%: met) |
| NFS night street | 0.0767 | 0.0379 | 0.49 not met | 0.45% |

The reference's own temporal noise is high on scene.json (13%) and NFS (57%) and Cornell (55%), so these ratios carry at least that much uncertainty. Like-for-like `indep` (composite minus off, linear profile, after this work): Cornell 0.0743, scene.json 0.2570, NFS 0.0271. Before (tone-mapped, not comparable): Cornell 0.2773, scene.json 0.2485, NFS 0.0258.

Clamp: kept at 32 (B6). The scale-aware clamp is not implemented, so there is no re-tuned value.

### 13.7 Light step

Not measured. The default `MainDirectionalLight` exists on every scene, and the emitter that dominates Cornell is emissive geometry, which the light step does not scale. Extending the step to emission and sky was not done.

### 13.8 Verification

- `tools/CheckNoRawGL.ps1`, `tools/CheckGenericExe.ps1`, `python tools/CheckVcxprojSources.py`: pass.
- `scripts/validate_surfel_gi_static.py`, `scripts/validate_surfel_gi_correctness.py`: pass.
- `scripts/validate_surfel_environment_miss_radiance.py`: updated to the current contract (sky sampler in `surfel_scene_trace.glsl`, mip 0, no radiance clamp); it failed at `ed55deb` because the sampler moved, and passes now.
- `scripts/validate_surfel_irradiance_energy.py` and `validate_surfel_gi_ray_radiance_contract.py`: fail at `ed55deb` (plan 10.7); not changed.
- `NOX_EDITOR_SMOKE=1 NOX_EXIT_AFTER_FRAMES=150`: 0 FAIL (15 PASS).
- Racing: `[Race] Finished in 47.4994 s (new record)`.
- Frames (surf_term = composite debug 4, frame 260): Cornell and NFS viewed. Before frames were not regenerated.

### 13.9 Not met

Furnace and white-sky analytic checks (O1, O2, O3); guiding and clamp invariances (B6, O4); scene.json and NFS energy targets; Cornell noise (2.56% against 2%); the light step; a scale-aware clamp; the reference-mode offset (O2).
