# Night-street configuration reference

Everything here is opt-in: scenes without these keys render exactly as before (see the regression
notes in `benchmarks.md`). Reference scenes: `scenes/nfs_night_street.json` (default night, wet),
`scenes/nfs_night_rain.json` (active rain), `scenes/nfs_dawn_street.json` (dawn with a low sun).

## Running

| Purpose | Command (Git Bash, from the repository root) |
| --- | --- |
| Open the night street | `NOX_FIRST_SCENE=scenes/nfs_night_street.json ./build/vs2022-x64/Release/Nox_Engine.exe` |
| Native resolution | add `NOX_WINDOW_MODE=borderless` (desktop resolution, DPI aware) or `fullscreen` |
| Start in drive mode | add `NOX_DRIVE=1` (or press F9 in the editor) |
| Pick a quality tier | add `NOX_QUALITY_TIER=high` or `ultra` |
| Timing benchmark | `NOX_BENCHMARK=all NOX_BENCHMARK_MODE=timing NOX_BENCHMARK_REPEAT=3 NOX_BENCHMARK_OUT=captures/x` |
| Capture stills | `NOX_BENCHMARK=all NOX_BENCHMARK_MODE=capture NOX_BENCHMARK_OUT=captures/x` |

`config/config.json` also accepts `"windowMode": "windowed" | "borderless" | "fullscreen"` and
`"dpiAware": true` (windowed mode keeps the system-scaled editor by default).

### Environment variables (new)

| Variable | Effect |
| --- | --- |
| `NOX_BENCHMARK` | `all` or comma-separated shot names from the scene's `benchmark.shots`; enables the runner |
| `NOX_BENCHMARK_MODE` | `timing` (default, no image writes) or `capture` (PNG at `capture_times`, sequences when `capture_sequence`) |
| `NOX_BENCHMARK_OUT` | Output directory (default `captures/benchmark`): `frames.csv`, `summary.json`, `summary.md`, PNGs |
| `NOX_BENCHMARK_REPEAT` | Repeat the shot list n times |
| `NOX_QUALITY_TIER` | Overrides `render_settings.quality_tier` |
| `NOX_WINDOW_MODE`, `NOX_WINDOW_WIDTH`, `NOX_WINDOW_HEIGHT` | Window overrides |
| `NOX_DRIVE`, `NOX_DRIVE_AUTOPILOT` | Start in drive mode; scripted input for unattended drive captures |
| `NOX_FINAL_DUMP_DIR`, `NOX_FINAL_DUMP_FRAMES` | Save the final backbuffer (including HUD/UI) at listed frames |

Existing hooks (`NOX_FIRST_SCENE`, `NOX_EXIT_AFTER_FRAMES`, `NOX_LOG_PASS_METRICS`,
`NOX_FRAMEBUFFER_DUMP_*`) are unchanged.

## Scene file keys

### `render_settings` (object)

Every key below is reset to its default when a scene with (or after a scene with)
`render_settings` loads, so scenes do not leak their look into each other.

| Key | Default | Meaning |
| --- | --- | --- |
| `physical_camera` | false | Photometric units + pre-exposed lighting (required for the night look) |
| `exposure_mode` | `"manual"` | `"manual"` (EV100) or `"auto"` (histogram metering) |
| `exposure_ev100` | 4.0 | Manual EV100; also the starting value for auto exposure |
| `exposure_compensation` | 0 | EV; negative keeps night scenes dark |
| `auto_exposure_min_ev100` / `_max_ev100` | -2 / 12 | Clamp of the metered EV |
| `auto_exposure_speed_up` / `_speed_down` | 2.5 / 1.2 | Adaptation rates (1/s) toward brighter / darker scenes |
| `auto_exposure_low_percentile` / `_high_percentile` | 0.55 / 0.95 | Histogram range averaged |
| `bloom` | true | Enable bloom |
| `bloom_mode` | `"additive"` | `"additive"` (thresholded, legacy) or `"energy_conserving"` (blurred image mixed in) |
| `bloom_strength`, `bloom_threshold`, `bloom_knee` | 0.8, 1.0, 0.5 | Bloom controls (threshold in exposed units) |
| `white_balance_temperature`, `white_balance_tint` | 6500, 0 | Illuminant to neutralise (Bradford, D65 target) |
| `contrast`, `saturation` | 1, 1 | Scene-linear grade around 0.18 |
| `shadow_tint`, `highlight_tint`, `split_tone_strength` | 0 | Multiplicative split toning |
| `lift`, `gamma`, `gain` | 0, 1, 1 | Display-space adjustments after tone mapping |
| `vignette`, `film_grain`, `chromatic_aberration`, `dither` | 0, 0, 0, true | Lens / output effects |
| `tonemap` | `"gt"` | `"gt"`, `"gt7"`, `"aces"`, `"none"` |
| `ibl_intensity`, `diffuse_ibl_scale`, `specular_ibl_scale`, `skybox_exposure` | 0.1, 0.3, 0.45, 1 | Environment; photometric scenes use 1 = HDR value in cd/m^2 |
| `ssao`, `ssao_intensity`, `ssgi`, `ssgi_strength`, `contact_shadows`, `taa` | engine defaults | Existing features |
| `clustered_lighting`, `frustum_culling` | true, true | Light clustering / camera culling |
| `wetness`, `puddles`, `rain`, `puddle_scale` | 0, 0, 0, 4 | Weather (materials opt in, see below) |
| `ssr`, `ssr_max_roughness`, `ssr_max_distance`, `ssr_max_steps`, `ssr_thickness`, `ssr_temporal_blend`, `ssr_resolve_samples` | false, 0.6, 80, 64, 0.25, 0.12, 9 | Screen-space reflections |
| `reflection_probes` | true | Use the scene's reflection probes |
| `volumetric_fog`, `volumetric_tile_size`, `volumetric_slices`, `volumetric_far` | false, 16, 64, 200 | Froxel fog grid |
| `fog_density`, `fog_base_height`, `fog_height_falloff`, `fog_albedo`, `fog_anisotropy` | 0.01, 0, 12, (0.9,0.92,0.95), 0.55 | Media (extinction 1/m) |
| `fog_noise_strength`, `fog_noise_scale`, `fog_wind`, `fog_ambient`, `fog_temporal_blend` | 0.5, 12, (0.6,0.05,0.4), (0.02,0.025,0.035), 0.1 | Mist breakup, sky light in the medium (cd/m^2) |
| `fog_distant_density`, `fog_distant_glow` | 0.004, 1 | Analytic haze beyond `volumetric_far` |
| `motion_blur`, `motion_blur_exposure_time`, `motion_blur_max_pixels`, `motion_blur_samples` | false, 1/60, 48, 12 | Physical-shutter motion blur |
| `rain_drop_count`, `rain_brightness` | 60000, 0.6 | Rain streaks (drawn when `rain` > 0) |
| `quality_tier` | none | Name of an entry in `quality_tiers` |

### `quality_tiers` (object)

`{ "high": { ...render_settings keys... }, "ultra": { ... } }`. The selected tier's keys are applied
on top of `render_settings`. The night scenes ship:

| Key | High (165 Hz target) | Ultra (fidelity, 60 Hz floor) |
| --- | --- | --- |
| `ssao` | false | true |
| `ssr_resolve_samples` | 4 | 9 |
| `ssr_max_steps` | 32 | 80 |
| `motion_blur_samples` | 6 | 16 |
| `volumetric_tile_size` | 16 | 8 |
| `rain_drop_count` | 40000 | 80000 |

### Lights

Photometric keys on `light` objects (Frostbite PBR 3.0 conventions):

| Key | Meaning |
| --- | --- |
| `luminous_power` | Point/spot luminous power in lumens. Stored as candela (point: lm / 4pi, spot: lm / pi, so the cone angle does not change brightness); attenuation becomes inverse-square (1 cm minimum) with the existing smooth range window. |
| `illuminance` | Directional light in lux |
| `source_radius` | Emitter radius in metres; widens specular lobes (no pinpoint glints on wet surfaces) |

Legacy `intensity` / `attenuation` keep working when these keys are absent.

### Materials

glTF materials may carry `"extras": { "nox": { ... } }`; scene model nodes may override any material
with `"material_overrides": [ { "match": "<substring or *>", ... } ]` (applied in order, case-insensitive
name match; overrides change the shared model asset, so all nodes using the file see them).

| Override key | Meaning |
| --- | --- |
| `base_color`, `metallic`, `roughness`, `specular`, `specular_color`, `ior` | Core PBR |
| `clearcoat`, `clearcoat_roughness` | Coat layer |
| `emissive`, `emissive_strength` | Emission (cd/m^2 in photometric scenes) |
| `transmission`, `thickness`, `attenuation_color`, `attenuation_distance`, `alpha_mode`, `double_sided` | Transparency |
| `normal_scale`, `occlusion_strength` | Detail |
| `disable_textures` | Drop mis-authored maps: `base_color`, `normal`, `metallic_roughness`, `emissive`, `occlusion`, `specular`, `specular_color`, `transmission` |
| `wetness_response`, `puddle_response`, `rain_bead_response` | Weather response [0,1] |
| `flake_intensity`, `flake_density` | Reserved for metallic flakes (stored and uploaded; shading not implemented yet) |
| `unlit`, `alpha_cutoff`, `sheen_color`, `sheen_roughness`, `anisotropy_strength`, `anisotropy_rotation`, `iridescence`, `iridescence_ior`, `iridescence_thickness_min`, `iridescence_thickness_max`, `dispersion` | Unlit (KHR_materials_unlit) and the mask cutoff; the sheen, anisotropy, iridescence and dispersion values are stored, not shaded |
| `interior_mapping`, `interior_depth`, `interior_strength`, `interior_seed`, `interior_lighting`, `interior_blinds`, `interior_style` | Window interior mapping (see the NOX surface section of the editor) |
| `slot` | Index of one primitive in the model (per-primitive override); `match` still applies when given |
| `textures` | Slot name to a texture file path (or `null` to clear). Slots: `base_color`, `normal`, `metallic_roughness`, `emissive`, `occlusion`, `specular`, `specular_color`, `transmission`, `clearcoat`, `clearcoat_roughness` |
| `texture_transforms` | Slot name to `{ offset, scale, rotation_degrees, texcoord }` (KHR_texture_transform) |

`"exclude_from_reflection_capture": true` on a model node keeps dynamic objects (the player car) out
of static probe captures.

### `reflection_probes` (array)

`{ "position": [x,y,z], "box_min": [...], "box_max": [...], "blend_distance": m, "intensity": 1 }`.
Captured once, two frames after load (256^2 faces, GGX prefiltered, 7 mips). The street kit adds its
own probes (per 30 m street segment, junction, tunnel, industrial strip).

### `procedural` on a model node

`{ "generator": "street_kit", "params": { "seed", "length", "output", "texture_root",
"streetlight_spacing", "neon_lights", "tunnel", "industrial", "shadowed_streetlights" } }` generates
`generated/<output>.gltf/.bin` plus window/shop emissive textures on load. Generated light nodes are
not written back when the scene is saved.

### `benchmark`

`{ "fixed_hz": 60, "warmup_frames": 120, "shots": [ { "name", "duration", "camera": { "keys": [ {t, position, look_at, fov} ] | "follow": { node, offset, look_offset, damping, fov, speed_fov_gain } }, "animate": [ { "node", "keys": [ {t, position} ], "face_motion" } ], "capture_times": [...], "capture_sequence": bool } ] }`.

### `game`

`{ "player_node", "body_node", "top_speed_kmh", "start_in_drive_mode", "drivable_areas": [[xmin, zmin, xmax, zmax], ...] }`.

## Drive mode controls

| Input | Keyboard | Gamepad |
| --- | --- | --- |
| Toggle drive / editor | F9 | - |
| Throttle / brake-reverse | W / S (arrows) | RT / LT |
| Steer | A / D (arrows) | Left stick |
| Handbrake (drift) | Space | B |
| Nitrous | Shift | A |
| Camera (chase, far chase, bumper, free) | C | - |
| Reset to spawn | R | - |

## Editor

Rendering Settings (F6) has a **Night Street** tab exposing every setting above (exposure, weather,
reflections incl. probe recapture, atmosphere, image formation, motion, culling). Changes are live and
not saved; edit the scene's `render_settings` to persist them.
