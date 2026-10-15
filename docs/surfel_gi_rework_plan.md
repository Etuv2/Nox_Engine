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
