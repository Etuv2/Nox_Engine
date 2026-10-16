# Rendering components

Rendering features are node components in the reflection registry (`src/reflect/`), so they show in the
inspector, save into the scene, undo, and script like every other component. Lua access is in
`docs/lua_api.md`, section "Rendering".

| Component | Where | Category | Drives |
| --- | --- | --- | --- |
| `WorldEnvironment` | scene root only | Rendering | scene-wide render settings (`render_settings`) |
| `Camera` (`current`) | camera nodes | Rendering | the view while playing |
| `ParticleEmitter` | any node | Rendering | CPU particles (`src/render/ParticleSystem.*`, `ParticlePass`) |
| `Decal` | any node | Rendering | textured quads on surfaces (`DecalPass`) |

## WorldEnvironment

Godot's WorldEnvironment: the scene's look. The component exists on the scene root only (the node the
hierarchy calls the root), so the inspector shows it there. Its properties are the scene's
`render_settings` keys, with the same names, so a value edited in the editor and the value written in the
scene file are the same setting.

- Reads and writes go to the live renderer context. A change is visible on the next frame.
- The scene file stays the source of truth. Saving writes the live values back into `render_settings`
  (keys at their defaults are omitted unless the file already names them; `quality_tier` and other
  non-setting keys are kept). The saved file does not carry a `WorldEnvironment` block.
- Quality tiers: when a tier is active, its values are part of the live state, so saving writes them into
  the base `render_settings` as well. The effective look is unchanged because the tier still applies on
  top when the scene loads.
- Not exposed: the skybox path (`skybox` in the scene; changing it needs a skybox reload) and the
  quality tier name itself.
- Setting a property on any other node is rejected.

Properties (grouped as in the inspector; the group is the label prefix). Ranges are the inspector ranges;
values outside them are clamped.

| Key | Type | Range or values |
| --- | --- | --- |
| `physical_camera` | bool | photometric units and pre-exposed lighting |
| `exposure_mode` | enum | manual, auto |
| `exposure_ev100` | float | 0 .. 16 |
| `exposure_compensation` | float | -6 .. 6 |
| `auto_exposure_min_ev100`, `auto_exposure_max_ev100` | float | -10 .. 20 |
| `auto_exposure_speed_up`, `auto_exposure_speed_down` | float | 0 .. 10 |
| `auto_exposure_low_percentile`, `auto_exposure_high_percentile` | float | 0 .. 1 |
| `bloom` | bool | |
| `bloom_mode` | enum | additive, energy_conserving |
| `bloom_strength` | float | 0 .. 4 |
| `bloom_threshold` | float | 0 .. 10 |
| `bloom_knee` | float | 0 .. 1 |
| `bloom_scatter_threshold` | float | 0 .. 10 |
| `bloom_half_res` | bool | |
| `white_balance_temperature` | float | 1000 .. 20000 K |
| `white_balance_tint` | float | -1 .. 1 |
| `contrast`, `saturation` | float | 0 .. 3 |
| `shadow_tint`, `highlight_tint` | vec3 | 0 .. 1 |
| `split_tone_strength` | float | 0 .. 1 |
| `lift` | vec3 | -1 .. 1 |
| `gamma` | vec3 | 0.1 .. 3 |
| `gain` | vec3 | 0 .. 4 |
| `vignette`, `film_grain` | float | 0 .. 1 |
| `chromatic_aberration` | float | 0 .. 20 px |
| `dither` | bool | |
| `tonemap` | enum | gt, aces, gt7, none |
| `ibl_intensity` | float | 0 .. 10 |
| `diffuse_ibl_scale`, `specular_ibl_scale` | float | 0 .. 4 |
| `skybox_exposure` | float | 0 .. 16 |
| `ssao`, `ssgi`, `contact_shadows` | bool | |
| `ssao_intensity` | float | 0 .. 2 |
| `ssao_resolution_scale`, `ssao_strength`, `ssao_power` | float | 0.25 .. 1, 0 .. 4, 0.5 .. 4 |
| `ssao_radius` (m), `ssao_bias` (rad) | float | 0.05 .. 4, 0 .. 1.2 |
| `ssao_blur_radius`, `ssao_directions`, `ssao_steps` | int | 1 .. 8, 4 .. 16, 2 .. 8 |
| `ssao_blur_sharpness` | float | 0 .. 200 |
| `ssgi_strength` | float | 0 .. 4 |
| `ssgi_resolution` | float | 0.25 (quarter, default), 0.5 (half, about 4x the trace cost) |
| `ssgi_history_length` | int | 1 .. 64 (default 12) |
| `ssgi_denoise_passes` | int | 1 .. 3 (default 2) |
| `ssgi_slices`, `ssgi_steps` | int | 1 .. 8 (default 3), 1 .. 64 (default 24) |
| `ssgi_interleaved` | bool | off (default); on traces half the pixels per frame |
| `ssgi_debug_stage` | int | 0 (off); presents one GI stage instead of lighting (measurement aid) |
| `taa` | bool | |
| `taa_blend` | float | 0 .. 1 |
| `taa_rejection_mode` | int | 0 (legacy), 1 (disocclusion) |
| `taa_disocclusion_tolerance` | float | 0.001 .. 0.5 |
| `taa_motion_weight` | float | 0 .. 1 |
| `velocity_from_depth` | bool | |
| `clustered_lighting`, `frustum_culling` | bool | |
| `debug_view` | enum | none, pre_taa, taa, ssr, bloom, velocity, motion_blur, taa_clean |
| `wetness`, `puddles`, `rain` | float | 0 .. 1 |
| `puddle_scale` | float | 0.5 .. 50 m |
| `ssr` | bool | |
| `ssr_max_roughness` | float | 0 .. 1 |
| `ssr_max_distance` | float | 1 .. 500 m |
| `ssr_max_steps` | int | 4 .. 256 |
| `ssr_thickness` | float | 0.01 .. 2 m |
| `ssr_temporal_blend` | float | 0 .. 1 |
| `ssr_resolve_samples` | int | 1 .. 16 |
| `reflection_probes` | bool | |
| `volumetric_fog` | bool | |
| `volumetric_tile_size` | int | 4 .. 32 px |
| `volumetric_slices` | int | 8 .. 256 |
| `volumetric_far` | float | 10 .. 1000 m |
| `fog_density` | float | 0 .. 0.5 (1/m) |
| `fog_base_height` | float | -100 .. 100 m |
| `fog_height_falloff` | float | 0.1 .. 100 m |
| `fog_anisotropy` | float | -0.99 .. 0.99 |
| `fog_noise_strength` | float | 0 .. 2 |
| `fog_noise_scale` | float | 0.5 .. 200 m |
| `fog_temporal_blend` | float | 0 .. 1 |
| `fog_distant_density` | float | 0 .. 0.1 |
| `fog_distant_glow` | float | 0 .. 10 |
| `fog_albedo` | vec3 | 0 .. 1 |
| `fog_wind` | vec3 | -10 .. 10 |
| `fog_ambient` | vec3 | 0 .. 10 (cd/m^2) |
| `motion_blur` | bool | |
| `motion_blur_exposure_time` | float | 0.001 .. 0.5 s |
| `motion_blur_max_pixels` | float | 0 .. 256 px at 1440p |
| `motion_blur_samples` | int | 1 .. 64 |
| `rain_drop_count` | int | 0 .. 200000 |
| `rain_brightness` | float | 0 .. 4 |

The table is `src/render/RenderSettingsTable.cpp`; the scene reader is `SceneRenderSettings::Apply`, and the
writer is `SceneRenderSettings::ToJson` (called by the scene saver). Add a setting in the table and it is
exposed, saved and tested together.

## Screen-space GI (SSGI)

`ssgi` is the visibility-bitmask indirect diffuse pass (`src/passes/IndirectDiffusePass.*`, spec in
`docs/ssgi_v2_visibility_bitmask_spec.md`). Per frame:

1. Gather at the working resolution (quarter by default). Four slices, 32 steps per direction, sky pixels
   skipped, and a slice stops marching once all of its sectors are occluded (exact, no output change).
   Slice rotation and sample offsets use interleaved gradient noise plus an R2 sequence over frames.
2. Temporal resolve reprojects the raw GI with depth, normal, and motion tests. The history is clamped to the
   current 3x3 mean +/- 1.25 sigma (intersected with the min/max box). Each pixel keeps an accumulated frame
   count up to `ssgi_history_length`; the current-frame weight is 1 / count, floored by the "Min Current
   Weight" slider (the old "Temporal Alpha").
3. Edge-aware bilateral denoise: a 5x5 pass, then 3x3 passes up to `ssgi_denoise_passes`.
4. Depth/normal/plane-aware bilateral upsample to full resolution. Full-resolution debug outputs are only
   written while a debug stage is selected, and the GI validation export is shown only then.

Cost: the gather is the largest stage (about 0.29 ms at 1080p with 4x32; 0.15 ms with 3x24). The other stages
(depth/normals, radiance downsample, temporal, denoise, upsample) cost about 0.33 ms and do not scale with the
slice or step count. The default is 3 slices x 24 steps: on the Cornell validation scene the indirect-only mean
is 4.6% below 4x32, and temporal noise is about a third of the pre-change pipeline. This is a deviation from the
4x32 quality defaults in `docs/ssgi_v2_visibility_bitmask_spec.md` section 7; set `ssgi_slices` 4 and
`ssgi_steps` 32 to restore them.

Interleaved trace (`ssgi_interleaved`) traces 4-row bands on alternating frames and fills the other bands from
history. It did not reduce gather time at 1080p and roughly tripled the frame-to-frame flicker, so it is off.

## Surfel GI

Surfel GI is the indirect diffuse term that replaces SSGI's far field (`src/passes/SurfelGIPipeline.*`, `SurfelGIManager.*`,
`src/surfel_gi/`, shaders in `shaders/surfel_gi/`). It is off unless `enableSurfelGI` is set. The design and the measurements
behind it are in `docs/surfel_gi_rework_plan.md`. The implementation follows the GIBS talk: surfels on the visible surfaces,
a screen-space population target, and ray-traced radiance integrated per surfel.

### Pipeline (one frame, Execute)

Labels are the GPU subpass names in the `[SurfelGISubpassMetrics]` log.

1. `begin_frame`, `dispatch_args`, `transform_update`: frame state, indirect dispatch arguments, and the transform IDs of
   the traced instances.
2. `recycling`: surfels leave the population when they are over the target, stale (no visible touch and no ray reference for
   30 frames), or far while over the target. Score `0.4 over + 0.3 stale + 0.3 far x over`.
3. `coverage_reset`, `coverage_project`, `coverage_spawn`: the visible G-buffer tiles are projected into the surfel coverage.
   Tiles below the coverage threshold spawn surfels from their candidates (grid query, deduplicated). The spawn budget
   follows half of last frame's under-covered tiles, between `maxSpawnsPerFrame` and a ceiling, and is limited by the
   population deficit to `kappa x visible tiles`.
4. `grid_count`, `grid_scan`, `grid_build`: a uniform grid of surfel cells (non-linear in depth). Each surfel is written into
   the cells its support sphere overlaps, with cell averages from its base cell.
5. `ray_request`, `ray_allocation`, `ray_generation`: each surfel asks for rays from its radial depth and its guide map; the
   ray budget (a tier value, section below) is shared out by demand; rays are generated with guided sampling
   (`guide_pdf.glsl`) and a minimum of four rays for young surfels.
6. `rt_scene_update`: the BVH and instance buffers, rebuilt only when the scene changes.
7. `tracing` (`trace_rays.comp`, `surfel_scene_trace.glsl`): each ray traces the software BVH. The first hit's own terms are
   its emission (MIS-weighted against the ray's density), its direct light, and its indirect light. The indirect light comes
   from the surfel cache (a bounded lookup of the surfels near the hit) at the first `cacheDepth` vertices. Where the cache
   answers, the path ends there; where it does not, the path continues with cosine sampling, next-event estimation of the
   emitters, and Russian roulette, up to `pathMaxHits` vertices. A cache miss in world mode queues a world-surfel request.
8. `temporal_integration` (`integrate.comp`, `msme.comp`): the ray samples update each surfel's irradiance (a Ray Tracing Gems
   style multiscale mean estimator with heuristic terms), and the radial depth is updated (`radial_depth`).
9. `irradiance_snapshot`, `irradiance_sharing`: the irradiance state is snapshotted and lightly shared between neighbours.
   `world_spawn` (world mode only): the cache-miss requests queued by this frame's tracing become world surfels, up to
   `worldSurfelSpawnsPerFrame`, under their own population cap (a quarter of the screen cap). They are in the grid from the
   next frame.
10. `final_gather` (`apply_indirect.comp`): every texel of the indirect target (half resolution) gathers the first N accepted
    surfels of its receiver's 3x3x3 cells, weighted by distance, normal, plane and confidence, with an examined bound of 2N.
    Where the summed weight is below one, the remainder is the receiver cell's average.
11. `upscale_filter` (`upscale_filter.comp`): a four-tap joint bilateral upsample on depth and normal, with one minimal
    temporal step (the previous output is reprojected with the velocity buffer, clamped to the gather neighbourhood, and
    blended at weight 0.85, zero above 6 px of motion or during a lighting change).
12. Composition (`deferred_lighting_frag.glsl`): the far field (surfel, LPV) is multiplied by SSAO and material AO only. SSGI
    does not darken it. Diffuse IBL and specular occlusion keep `min(SSAO, SSGI visibility) * material AO`.

### Quality tiers

The tier is `SurfelGIQualityTier` (`Low`, `Medium` (default), `High`, `Ultra`), set in the Rendering settings, by
`surfelGIQualityTier` in the render context, or by `NOX_SURFEL_GI_QUALITY_TIER` (0 to 3). Each tier sets two groups of values in
`src/ModularRenderer.cpp`:

- `GetSurfelGIBudgetCaps`: the ray budget, the population caps, the gather counts and the spawn limits.
- `GetSurfelGIPathPreset`: world mode, the path depth, the cache depth and the cache budgets, the hybrid confidence threshold
  and the emitter sampling cut.

| Setting | Low | Medium (default) | High | Ultra |
|---|---|---|---|---|
| Ray budget per frame | 1024 | 2048 | 4096 | 8192 |
| World mode (cache-first, misses queue world surfels) | on | on | on | off |
| Path depth past the first hit (`pathMaxHits`) | 1 (none) | 3 | 4 | 32 |
| Cache read at vertices below (`cacheDepth`) | 1 | 3 | 3 | 2 |
| Sun BVH fallback below vertex (`sunShadowBVHDepth`) | 1 | 1 | 2 | 32 |
| Irradiance lookup budget: surfels visited / per cell | 16 / 1 | 16 / 1 | 32 / 2 | 128 / 4 |
| Cached-albedo lookup budget: surfels / per cell | 4 / 1 | 4 / 1 | 8 / 1 | 32 / 2 |
| Emitter next-event distance cut (m, 0 = none) | 40 | 40 | 40 | 0 |
| Software BVH trace | on | on | on | on |
| Gather accepted surfels per texel | 16 | 32 | 48 | 64 |
| Hybrid continuation (`cacheMinConfidence`) | 0 (off) | 0 (off) | 0 (off) | 0 (off) |

The hybrid threshold is kept as a knob but is 0 in every tier. Measured on scene.json (plan section 20.2), continuing only at low-confidence cache answers lowered the energy at both depth 2 and depth 3 and cost more tracing time.

### Settings

Each preset value is the default of an environment variable, read in the validation block (`NOX_ENABLE_SURFEL_GI=1`):
`NOX_SURFEL_GI_RAY_BUDGET`, `NOX_SURFEL_GI_WORLD` (0 or 1), `NOX_SURFEL_GI_PATH_DEPTH`, `NOX_SURFEL_GI_CACHE_DEPTH`,
`NOX_SURFEL_GI_SUN_BVH_DEPTH`, `NOX_SURFEL_GI_CACHE_MIN_CONF`, `NOX_SURFEL_GI_CACHE_CANDIDATES`, `NOX_SURFEL_GI_CACHE_PER_CELL`,
`NOX_SURFEL_GI_CACHE_ALBEDO_CANDIDATES`, `NOX_SURFEL_GI_CACHE_ALBEDO_PER_CELL`, `NOX_SURFEL_GI_EMITTER_CULL` (metres, 0 = off),
and `NOX_SURFEL_GI_QUALITY_TIER`. The debug views are `NOX_SURFEL_GI_DEBUG_VIEW` and `NOX_SURFEL_GI_COMPOSITE_DEBUG` (4 = the
surfel term alone). The independent reference (`NOX_GI_REFERENCE=1`) does not use the cache or the tier path presets. Its trace is a
per-texel path tracer with its own depth.

### Measurement

Measured on the harness (plan section 20.4), frames 200 to 260 unless stated, with the independent reference as the
denominator. Medium (default): on scene.json 4.4 ms of surfel GPU time (2.8 ms of it tracing), a lit-mask term of 0.63 at
frames 200 to 260 that settles at 0.72 by frame 800 to 900 (0.86 all-pixel), and about 300 frames to 90% of that steady value.
On Cornell the all-pixel term is 1.06 of the reference. On NFS the term is 0.68 (lit) and 0.56 (all-pixel), with 2.9% noise.
The pre-rework main, on the same harness, reaches 0.27 (scene.json) and 0.50 (NFS). Ultra is the reference-quality setting and
costs 70 ms on scene.json. Known limits: the cache lookup's neighbour-cell walk is the largest remaining cost, the emitter
distance cut (40 m) is not checked against a far-emitter analytic scene, and the scene.json and NFS energies stay below the
[0.9, 1.1] band at steady state.

## Camera current

`Camera` (on a camera node) has a `current` flag. In Play mode the first node in the hierarchy whose camera
has `current` set drives the render camera: each frame the render camera takes the node's world transform
and its `fov`, `near_plane` and `far_plane`. Editor mode keeps the editor camera. Setting `current` on one
camera and clearing it on the others switches the view; Lua does the same through
`ctx:component(h, "Camera").current = true`.

The camera drive is a perspective lens; `projection` is not driven.

## ParticleEmitter

A generic CPU particle system: camera-facing billboards, simulated on the CPU, drawn by `ParticlePass`
after the vehicle effects and before TAA. Works on any node; the node's transform is the emitter.

| Property | Type | Notes |
| --- | --- | --- |
| `emitting` | bool | emission on or off (existing particles finish) |
| `rate` | float | particles per second, 0 .. 1000 |
| `lifetime_min`, `lifetime_max` | float | seconds; the setters keep min <= max |
| `start_speed` | float | m/s along the cone axis (the node's up) |
| `spread_angle` | float | full cone angle in degrees, 0 .. 180 |
| `gravity` | vec3 | m/s^2 in world units |
| `start_size`, `end_size` | float | billboard radius in metres at birth and death |
| `start_color`, `end_color` | color4 | straight alpha; colour and opacity interpolate over the lifetime |
| `texture` | asset path | optional image (`.png`, `.jpg`); empty draws a soft disc |
| `max_particles` | int | cap on live particles; emission at the cap is dropped |
| `world_space` | bool | particles stay where they spawned instead of following the node |
| `blend` | enum | alpha (premultiplied), additive |
| `particle_count` | int, read only | live particles (not saved) |

Methods: `burst(count)` spawns count particles now (capped by `max_particles`), `restart()` clears the
particles and the emission remainder, `clear()` removes live particles.

Notes:
- Simulation runs once per frame in `Core::Update` (`nox::render::UpdateSceneEmitters`), so several views
  do not speed it up. A frame longer than 0.1 s is clamped, so a hitch does not flood the emitter.
- Emitters run in the editor too (a preview), as in Unity.
- The random sequence is seeded per emitter; a given settings set and frame time gives the same spawn count
  (tests rely on this for counts, not positions).

## Decal

A textured quad on the surface at the node, drawn by `DecalPass` in the G-buffer stage, like the scene's
generator decals (road paint). Node decals are gathered every frame into one set per texture and merged with
the generator sets.

| Property | Type | Notes |
| --- | --- | --- |
| `texture` | asset path | straight-alpha colour image (`.png`, `.jpg`) |
| `size` | vec3 | metres: `x` across and `z` along the node's local axes (scaled by the node transform); all components > 0 |
| `color` | color4 | rgb multiplies the albedo, alpha multiplies coverage |
| `opacity` | float | 0 .. 1 coverage multiplier on top of `color.a` |

Behaviour and limits:
- The decal is planar: a quad in the node's local XZ plane, facing the node's up (+Y). `size.y` is reserved
  for projected decals and is not used yet.
- It replaces albedo and roughness only (the pass's existing contract). Lighting is what the surface receives,
  so a decal on an unlit surface stays dark.
- `normal_blend` is not supported: the pass does not write normals.

## GlTF material support

The importer (`src/Scene.cpp`, `LoadFromGLTF`) logs one line per model listing the extensions it uses and how
each is handled: `rendered`, `parsed-only`, `image-fallback` or `UNSUPPORTED`. The table is the status as of this
change.

| Feature | Status |
| --- | --- |
| Core metallic-roughness: base colour, metallic (B) and roughness (G) in one texture, occlusion (R), emissive | Rendered. Base colour and emissive are sRGB textures; normal, metallic-roughness and occlusion are linear |
| Normal maps | Rendered with the glTF convention (+Y up in tangent space, bitangent = cross(N, T) * w, no green flip). Tangents are read from TANGENT or generated |
| Missing tangents | Generated per vertex from the triangle UV gradients in the normal map's UV set, after its texture transform. Orthonormalised, with handedness from the UV winding. This is not full MikkTSpace |
| `TEXCOORD_1` | Used by any texture reference that names it (`texCoord`) |
| `KHR_texture_transform` | Rendered per texture slot: `uv' = T * R * S * uv`, with the `texCoord` override. Also read for specular and transmission references (clearcoat references are stored, see below) |
| `COLOR_0` | Multiplies the base colour (vec4, or vec3 with alpha 1). Absent = white |
| `alphaMode` OPAQUE / MASK / BLEND, `alphaCutoff` | Rendered. MASK discards in the G-buffer, forward and shadow passes |
| `doubleSided` | Rendered. Back faces shade with the reversed normal |
| `KHR_materials_unlit` | Rendered: base colour is written as emission and no lighting term applies |
| `KHR_materials_emissive_strength`, `KHR_materials_ior`, `KHR_materials_specular` | Rendered |
| `KHR_materials_clearcoat` | The factors are rendered with the base normal. The intensity and roughness textures are parsed and stored (editable in the Materials window) but not sampled yet: the deferred and forward passes use every texture unit below 32, and fragment samplers cannot use units 32 and up (binding one crashed the driver at shutdown). The clearcoat normal texture is not parsed |
| `KHR_materials_transmission`, `KHR_materials_volume` | Rendered (see `transmission_completion_audit.md`) |
| `KHR_materials_pbrSpecularGlossiness` | Converted to metallic-roughness on import (approximate for the non-diffuse textures) |
| `KHR_materials_sheen`, `KHR_materials_anisotropy`, `KHR_materials_iridescence`, `KHR_materials_dispersion` | Parsed and stored (editable in the Materials window); not shaded |
| `KHR_texture_basisu`, `EXT_texture_webp` | stb_image cannot decode KTX2 or WebP. A texture uses its core `source` image when it has one; otherwise it is skipped and the material keeps its factors. The file still loads |
| `KHR_mesh_quantization` | Accepted: normalized integer accessors are read as floats |

## Screen-space reflections

Reflections on glossy surfaces (`SSRPass`, with `ReflectionProbePass` as the fallback). The settings are
`render_settings` keys in the "Reflections" group of the render settings table, so they show in the
Rendering window's Look tab and load and save like the other scene keys.

| Key | Default | Range | Meaning |
| --- | --- | --- | --- |
| `ssr` | `false` | bool | enables the pass (needs TAA) |
| `ssr_max_roughness` | `0.6` | 0 .. 1 | roughness above which a surface skips SSR and uses probes and sky |
| `ssr_blur_strength` | `1.0` | 0 .. 4 | scales the GGX cone footprint that picks the blur level; `0` keeps every reflection sharp, higher values hide more noise and soften more |
| `ssr_max_blur_level` | `6` | 0 .. 6 | coarsest blur level a reflection may read (`0` = sharp only) |
| `ssr_edge_fade` | `0.08` | 0 .. 0.3 | screen-edge fade of traced hits (fraction of the screen) |
| `ssr_probe_blend` | `1.0` | 0 .. 1 | weight of local probes where SSR misses (`0` = environment only) |
| `ssr_temporal_blend` | `0.12` | 0.02 .. 1 | weight of the current frame in the temporal accumulation |

Pipeline (each step is one compute dispatch, driven from `src/passes/SSRPass.cpp`):
1. Trace (half resolution, GGX-VNDF rays, `ssr_trace_comp.glsl`) and resolve (4 or 9 reused rays,
   `ssr_resolve_comp.glsl`).
2. Temporal (`ssr_temporal_comp.glsl`): reprojects with the velocity buffer. Mirror-like surfaces reproject
   through the reflected point, so a reflection follows the object it shows, not the floor. Variance
   clipping against the 3x3 neighbourhood limits how much stale history survives.
3. Blur chain (`ssr_blur_down_comp.glsl`, `ssr_blur_up_comp.glsl`): a dual-filter Kawase chain of seven levels
   (Bjorge, SIGGRAPH 2015): 5-tap downsample, 8-tap upsample, level 0 at half resolution. The first stage
   premultiplies the temporal output by its confidence, so misses add nothing and the blur does not pull
   background colour into a reflection. The temporal history itself is never blurred, so blur does not
   compound from frame to frame.
4. Composite (`ssr_composite_comp.glsl`): each pixel's cone is the GGX lobe half-angle `atan(alpha)`, with
   `alpha = roughness^2` (Stochastic SSR / Frostbite-style approximation), so `tan(cone) = alpha`. The blur radius
   in full-resolution pixels is `2 * hitDist * alpha / (viewDepth * angPerPixel)`, and the level is
   `log2(radius / 2)`, because chain level 0 has a 2-pixel texel radius. Mirror-smooth pixels (roughness below
   0.1) stay on the sharp history. Sharp and blurred values are mixed between levels below 0, and the result is
   un-premultiplied into `ctx.ssrTexture`.

Combining with probes and sky: `ComputeIBL` in `shaders/deferred_lighting_frag.glsl` takes the SSR result
first, then the local probe where SSR has no hit (scaled by `ssr_probe_blend`), then the prefiltered
environment. The traced value replaces the lobe's prefiltered radiance inside the split-sum
(`EvaluatePrincipledIBLEx`), so Fresnel and the BRDF terms are applied once. Probes use the same
roughness-based mip as the environment. The SSR blur width follows the GGX cone and hit distance, so
the two only match in roughness, not in blur width: the handover was checked by eye on the materials test
scene (probes on and off), not measured on a scene with many probe misses.

Debugging: `NOX_SSR_DEBUG_VIEW` replaces the lit image with the SSR signal alone, so each stage can be measured
without lighting: `1` raw resolve, `2` temporal output (the blur chain's input, which is what the pass produced before
the blur existed), `3` composite, `4` chosen blur level as a colour ramp (blue sharp, red top level), `5` chosen level
as raw float in R (exact, alpha = hit). `NOX_SSR_DUMP_DIR` with `NOX_SSR_DUMP_FRAMES` (comma list of SSR frame numbers)
writes the texture the view selects as raw float RGBA, without tonemapping, to `ssr_view<N>_f<frame>.f32`: a text header
line `width height channels`, then rows bottom-to-top as float32. Use it for offline measurement; the lit image goes
through the post-process.

Limits:
- The blur ignores depth edges. A reflection that samples a silhouette can blur across it; the premultiplied
  confidence keeps misses and sky out of the blend, which is the main leak it prevents.
- Temporal rejection has no previous-depth test. Ghosting is limited by the neighbourhood clamp only, and
  was checked on static frames, not on fast camera motion.
- The chain is half resolution at its finest, so very sharp glossy reflections come from the full-resolution
  history at level 0 instead.
