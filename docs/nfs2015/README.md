# NOX "Night Street" look development (Need for Speed 2015 reference)

Status: milestones M0-M8 implemented on branch `nfs2015-look` (2026-10-09). Measured results in section 5 and `benchmarks.md`.

Contents
1. Requirements (agreed)
2. Evidence table (confirmed / Frostbite-era / observation / unverified)
3. Why the night presentation works (analysis, labelled proposals)
4. Engine gap analysis (verified against source)
5. Milestone plan and acceptance criteria
6. Fidelity limits

See also: `evidence.md` (full source notes), `benchmarks.md` (measurements), `configuration.md` (settings reference).

---

## 1. Requirements (agreed 2026-10-09)

| Item | Decision |
| --- | --- |
| Hardware | RTX 3080 10 GB, Ryzen 7 5800X, 32 GB RAM (detected) |
| Resolution / refresh | Native 3440x1440 @ 165 Hz, "as good graphics as possible" |
| Interpretation | Two tiers: **High** targets the 165 Hz budget (6.06 ms) at native; **Ultra** maximises fidelity with a 60 Hz floor (16.67 ms p99). Both measured at native resolution; numbers reported honestly where a tier misses. |
| Feel | NFS-2015-style gameplay presentation: drivable arcade car, chase camera (with free-camera option), speed FX, HUD. Not a handling-model replica. |
| Assets | Procedural street kit generated in C++ + CC0 PBR textures (Poly Haven / ambientCG) in a git-ignored folder. Existing `models/car.glb` (R33 GT-R) with corrected material overrides. |
| Scope | All existing render modes retained; new features are general and enabled by a scene-level "night street" preset. |
| Reference platform | PC, stock game, max settings, gameplay camera, no ReShade/SweetFX. Promotional/photo-mode images used only for look, never for metric comparison. |

---

## 2. Evidence table

Tags: **[NFS]** confirmed for NFS 2015 by a primary/developer source; **[FB]** documented Frostbite capability of the era, *not* confirmed shipped in NFS 2015; **[OBS]** visual observation (stated source); **[UNV]** common claim, unverified; **[NOX]** proposed NOX solution.

| # | Claim | Tag | Source |
| --- | --- | --- | --- |
| E1 | Cars built from reference photos with camera/lens matching; modern cars from manufacturer CAD. This is photo-referenced modelling, not photogrammetry. | [NFS] | Bryn Alban (Ghost vehicle art director), Speedhunters, Jul 2015 |
| E2 | Frostbite PBR used for car materials; one shader per surface type; real-world material values; ~25 materials and ~50 textures per stock car. | [NFS] | Alban, ibid. |
| E3 | Look-dev prototyped dust/dirt/damage layering and rain on cars. | [NFS] (car look-dev; shipped environment-wetness system not documented) | Alban, ibid. |
| E4 | NFS reflection system at the time of the thesis: SSR + IBL through **parallax-corrected cubemaps**, generated at runtime **one cube face per frame**, **128x128 per face**, **41 cubemaps resident**, **manually placed** by artists; failures at high speed (cubemaps not updated fast enough), visible SSR/IBL seams, poor handling of dynamic lights. | [NFS] (thesis written with Ghost for "Need for Speed"; describes the existing NFS codebase in 2016 - most likely the NFS 2015 tech, not explicitly dated) | Fredriksson & Scott, *Generating Real Time Reflections by Ray Tracing Approximate Geometry*, Chalmers 2016, ch. 1.1, 2.2 |
| E5 | The thesis prototype (ray tracing proxy geometry as SSR fallback) is research, **not** shipping tech. | [NFS] negative | ibid. |
| E6 | Physically based sky/atmosphere listed as "in production" for Need for Speed (Ghost). | [NFS] (in-production slide, not a shipping spec) | Hillaire, SIGGRAPH 2015 Advances, slide 44 |
| E7 | Photometric light units, inverse-square falloff with smooth window `saturate(1-(d^2/r^2)^2)^2`, EV100 exposure `1/(1.2*2^EV100)`, pre-exposed float16 light buffer, specular occlusion `saturate(pow(NdotV+AO, exp2(-16r-1)) - 1 + AO)`, reflection chain SSR -> local probes -> distant probe. | [FB] | Lagarde & de Rousiers, *Moving Frostbite to PBR 3.0*, SIGGRAPH 2014 |
| E8 | Stochastic SSR: tile classification, GGX importance sampling, half-res trace, ray reuse with BRDF/pdf weighting, hit-depth reprojection, cubemap fallback. Mirror's Edge Catalyst shown; NFS not mentioned. | [FB] | Stachowiak & Uludag, SIGGRAPH 2015 Advances |
| E9 | Unified volumetrics: froxel grid (8x8 px tiles, 64 slices), scattering/extinction/phase/emissive media, clustered local lights + CSM sun in froxels, 5% temporal blend, energy-conserving integration. 2.95 ms on PS4 at 900p (8x8). | [FB] | Hillaire, SIGGRAPH 2015 Advances |
| E10 | PC version: unlocked frame rate, 4K support; AA options FXAA/TAA; AO options SSAO/HBAO; quality levels Low-Ultra. | [OBS] secondary (Wikipedia citing IGN; Notebookcheck snippet) | see `evidence.md` |
| E11 | Critics: "stunning night racing and weather effects"; "daft time-of-night effects". | [OBS] | Wikipedia reception (TrustedReviews, IGN) |
| E12 | Promotional stills: very low exposure key; blue/teal ambient vs warm sodium/red emissive separation; wet asphalt with vertically stretched, broken highlights; standing water only in patches; rain beads on paint; tight glare + soft bloom; heavy vignette; one dusk/dawn still with purple sky. | [OBS] (promotional, photo-mode-like with DOF) | Steam store screenshots, app 1262540 |
| E13 | "The whole game is baked." | [UNV] - unsupported; dynamic cars, headlights, runtime cubemap generation (E4) contradict a fully baked pipeline. | - |
| E14 | "Always night to save performance." | [UNV] - no developer statement found. Night + dawn presence observed (E12). Performance motive unknown. | - |
| E15 | "Photogrammetry everywhere." | [UNV] - E1 describes photo-*referenced* modelling only. | - |
| E16 | "Roads are always wet." | [UNV] - commonly observed in footage; no developer documentation of the wetness system. | - |

---

## 3. Why the night presentation works (analysis)

Observation-based reasoning; items marked **(proposal)** are NOX design choices, not documented NFS history.

1. **Contrast budget.** A low exposure key lets a few strong local emitters (sodium lamps, signs, tail lights) dominate. Their reflections on wet asphalt double the visible "light count" at no extra lighting cost, because the reflection comes from SSR/probes rather than additional lights. Material fidelity is then judged mostly through specular response: highlights, Fresnel, reflections. That suits a PBR pipeline with calibrated F0/roughness (E2, E7).
2. **Wetness carries the image.** Wet substrate (darkened albedo, lower roughness) plus patchy standing water (near-mirror) gives the stretched, broken highlight streaks. Fully mirror roads look fake; the breakup comes from per-pixel roughness/puddle variation and normal detail.
3. **Atmosphere gives depth.** Height fog and in-scattering around lamps separate planes at night where there is little ambient light to do so. Frostbite had froxel volumetrics in the same era (E9 [FB]).
4. **Constrained lighting envelope (proposal).** Night/dawn states with mostly static emitters allow: authored/static reflection captures (E4 confirms authored probes), cached shadow maps for static lamps, controlled exposure ranges per area (tunnel vs street), and stable indirect light. This is a NOX optimisation strategy, not a documented NFS reason.
5. **Darkness is not free.** Many local lights, wet reflective surfaces (SSR on most of the screen), transparent rain/spray and volumetrics are each expensive. Geometry outside the view still contributes to shadows and probes. The NOX budget therefore funds reflections/volumetrics/lights rather than assuming savings.
6. **Multiple states.** The reference includes dusk/dawn (E12), so NOX supports at least `night`, `night_rain` and `dawn` presets driven by scene data, not one static dark sky.

---

## 4. Engine gap analysis (verified against source, 2026-10-09)

### Works (keep)
- Shared principled BRDF (GGX + height-correlated Smith + Schlick, Burley diffuse), clearcoat lobe with base attenuation, split-sum IBL with multiscatter (`shaders/includes/pbr_common.glsl`).
- Data-driven frame graph with declared inputs/outputs (`src/ModularRenderer.cpp` BuildPassDescriptors / CompileFramePlan).
- Async GPU timestamp ring (4 slots, 3-frame delay) per pass.
- TAA: Halton jitter, Catmull-Rom history, depth/normal rejection, neighbourhood clamp.
- RGB16F linear HDR chain, dual-source transmission for glass, 4-cascade CSM with texel snapping.

### Needs correction (bugs found)
| Issue | Location | Effect |
| --- | --- | --- |
| TAA jitter applied at half amplitude (`jitter/W`, needs `2*jitter/W`) | `ModularRenderer.cpp` UpdateContext | Under-resolved sub-pixel detail, more aliasing on thin wet highlights |
| Sky writes zero velocity | `TAAPass.cpp` velocity clear | Sky/lamp-halo smearing during camera rotation |
| Skinned meshes produce bind-pose velocity | `velocity_vert.glsl` | Ghosting on characters (not central to this scene) |
| Bloom added after exposure, extracted from unexposed HDR | `postprocess_frag.glsl`, `bloom_extract.glsl` | Bloom does not follow exposure; breaks EV workflow |
| Output uses pow(1/2.2) not sRGB OETF | `postprocess_frag.glsl` | Crushed/shifted near-blacks - critical in night scenes |
| Anisotropic filtering never applied | UI writes unused value | Blurry road textures at grazing angles |
| Shadow cache invalidated by any moving object | `LightManager.cpp` | All shadow slices re-render while the car moves |
| No TAA invalidation on camera cut / toggle | `TAAPass.cpp` | Ghosting across camera-path cuts |
| Light loop hard-capped at 64, no culling (stub) | `deferred_lighting_frag.glsl`, `LightManager.cpp` | Street with dozens of lamps impossible; O(lights) per pixel |
| Pass metrics write 0/1 into "draw call" counts | `ModularRenderer.cpp` | Misleading CSVs |
| Dead PCSS / bias UI controls | `RenderingSettingsWindow.cpp` | Confusing tuning |

### Genuinely missing
- Reflections: no SSR, no local/parallax-corrected probes, no planar reflections (IBL only).
- Wetness: no wet/puddle/rain material response, no rain on cars, no droplets.
- Lights: arbitrary units, polynomial attenuation, no light radius for specular (pinpoint glints), no clustered culling.
- Atmosphere: no fog, height fog, volumetric scattering, rain particles, spray.
- Image formation: no EV100/auto exposure, white balance, grading, vignette, grain, motion blur, lens effects.
- Materials: no paint flakes, no specular AA (Toksvig/geometric), 8-bit G-buffer normals/roughness, no per-scene material overrides, no anisotropic filtering.
- Tooling: no camera paths, no fixed-timestep benchmark mode, no frame-time distribution report, no CLI-triggered recording.
- Gameplay: no vehicle controller, chase camera, or racing HUD.

### Asset deficiencies (separate from renderer)
- No street content at all (asphalt, curbs, lamps, signage, buildings, tunnel).
- `models/car.glb` (R33 GT-R) materials: many dielectric parts authored as metallic 0.5-0.76; paint authored as metallic=1. Requires overrides.
- `models/car_gtr.glb` is 1.15 M triangles in 68 meshes with mostly untextured materials (usable for close-ups only).
- HDR skies: only one night HDR (`hdrs/skybox_night.hdr`); no dawn HDR besides `morning_sky.hdr`.

---

## 5. Milestones: status and results

Each milestone was measured with the benchmark runner (`benchmarks.md`) and compared on the same
scripted shots (`captures/nfs/*`, git-ignored).

| M | Delivered | Result |
| --- | --- | --- |
| M0 | Benchmark/camera-path runner (fixed step, timing vs capture modes, CSV + percentile summaries), native-res borderless/fullscreen with DPI awareness, procedural street kit (road, markings, curbs, sidewalks, buildings with generated window/shop interiors, neon, 46 photometric lights, tunnel, industrial strip, 14 reflection probes), CC0 textures, scene `material_overrides`, glTF `extras.nox`, scene `render_settings` | Repeatable shots; legacy baseline recorded |
| M1 | TAA jitter amplitude fix, sky motion vectors, closest-depth velocity dilation, luminance-weighted (Karis) resolve, history reset on TAA toggle, sRGB OETF, bloom follows exposure, stale previous-transform fix (static objects reported motion), zero motion on camera cuts/teleports, geometric-normal shadow bias, RGBA16 G-buffer normals, frustum culling | Legacy scenes unchanged except intended sRGB/dither differences |
| M2 | Clustered light culling (no 64-light cap), photometric lights (lm, cd, lux; inverse-square + smooth window), emitter radius lobe widening | Lighting 1.1 ms for 46 lights at native res |
| M3 | Physical camera: pre-exposed lighting, EV100 manual/auto exposure (GPU histogram, no readback), energy-conserving bloom, white balance (Bradford), scene-linear grade + split tone, lift/gamma/gain, vignette, grain, CA, dither | Night key without crushed blacks; emissives no longer clip into blocks |
| M4 | Wetness: porous darkening, water-film roughness, normal flattening, patchy puddles with cavity term, rain ripples, rain beads on paint/glass; specular anti-aliasing (Tokuyoshi-Kaplanyan + Toksvig) | Broken, stretched wet highlights; puddles only in patches |
| M5 | Stochastic SSR (VNDF, half-res, McGuire DDA with adaptive stride + refinement, ray reuse, hit-depth temporal), parallax-corrected local probes (auto-captured, GGX prefiltered, frustum culled), single reflection chain SSR -> probes -> sky inside the split-sum | Road reflects the street canyon instead of the sky; neon/shop streaks on wet asphalt |
| M6 | Froxel volumetric fog (Hillaire-style, clustered lights + shadows, temporal), analytic distant haze, rain streaks lit from the fog volume; night-rain and dawn scene variants | Headlight/lamp beams in mist, depth haze, rain at 0.03 ms |
| M7 | Motion blur (McGuire reconstruction, physical shutter), drive mode: arcade car, NFS-style chase camera (lag, FOV, pull-back, shake), bumper/far/free cameras, HUD (speed, rpm, gear, nitrous, rotating minimap), gamepad | 0.4-0.8 ms blur; car stays sharp under the chase camera |
| M8 | Quality tiers (High / Ultra), SSGI dropped from the night preset (A/B), editor "Night Street" tab, VS project entries, docs | High 6.09 ms median (164 fps), Ultra 8.74 ms (114 fps) at 3440x1440 |

### Remaining differences and open issues (honest list)

1. **Content, not renderer, is the largest gap.** The street kit is boxes with tiling CC0 textures,
   generated window atlases and simple props; Ventura Bay has authored architecture, decals, clutter,
   traffic, foliage, hills and far more light sources. The car's source materials are miscalibrated
   (fixed by overrides); there is no damage/dirt layering or livery system (E3).
2. **SSR temporal shimmer** on wet asphalt under very bright sources (temporal std p95 8.4 / 255 vs
   1.75 without SSR, `benchmarks.md`). Likely fix: exempt SSR-lit pixels from the main TAA's
   reactive/edge mask or feed SSR confidence into TAA, and add a small bilateral spatial filter.
3. **165 Hz at p95/p99**: High meets the median (6.09 ms) but p95 is 6.89 ms (145 fps) and p99
   7.16 ms. Further headroom needs dynamic resolution or half-res SSR resolve.
4. **Paint flakes are not shaded** (`flake_intensity` is stored and uploaded only). Real flakes are
   sub-pixel; a credible version needs a filtered stochastic-glint model (Zirr & Kaplanyan 2016).
5. **Shadowed streetlights** use very wide spot frusta (136 degrees), which costs shadow-map
   resolution; only 2 lamps and 1 floodlight cast shadows. Static-caster shadow caching is not done.
6. **Dawn state is untuned**: the CC0 sunrise sky is pink, not the reference's purple; it is a
   demonstration of the time-of-day path, not a calibrated match.
7. **No reference-matched image metrics**: no gameplay footage with matching cameras was available
   (`evidence.md`), so comparisons are NOX-vs-NOX (before/after, tiers, temporal noise) plus
   qualitative comparison with promotional stills.
8. **Gameplay scope**: driving is an arcade approximation; no traffic, police, races, garage or menus.
9. **Probes are static**: captured once per scene load (two frames after load) and excluding dynamic
   objects; NFS refreshed probes at runtime (E4). Moving lights are not reflected in probes.
10. Volumetric fog shows faint slice rings around very bright small sources in the temporal-noise map.

## 6. Fidelity limits (known up front)

- No access to NFS 2015 assets, shaders, or exact parameters; matching is perceptual and reference-driven.
- No matched camera/scene correspondence with the game, so image metrics (PSNR/SSIM) against game captures are not meaningful; NOX-vs-NOX metrics (before/after, temporal stability) are used instead.
- Gameplay references were found by title only; the stills used are promotional (photo-mode-like) and exaggerate DOF and grading.
- The street kit is procedural; it cannot match Ventura Bay's authored content density.
