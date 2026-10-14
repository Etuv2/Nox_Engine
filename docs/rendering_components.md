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
| `ssgi_strength` | float | 0 .. 4 |
| `ssgi_resolution` | float | 0.25 (quarter, default), 0.5 (half, about 4x the trace cost) |
| `ssgi_history_length` | int | 1 .. 64 (default 12) |
| `ssgi_denoise_passes` | int | 1 .. 3 (default 2) |
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

Cost scales with the trace resolution and the slice count (Global Illumination tab). Halving the slice count
roughly halves the gather; the spec defaults (4 slices, 32 steps) are kept because fewer steps bias the
irradiance low.

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
