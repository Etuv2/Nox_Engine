# Scene files

The engine's scene files live in `scenes/`. Each is a JSON document with `nodes` and optional
top-level data (`camera`, `skybox`, `render_settings`, `quality_tiers`, `project`, `scripting`,
`game`, `benchmark`). The engine loads any of them with `NOX_FIRST_SCENE=scenes/<name>.json`, or
through `config/config.json` (`first_scene`) and `config/launch_presets.json`.

Every file below loads through `Nox_Engine.exe --no-game` with exit 0 and no warnings, except the
four racing scenes (see the last section). The hand-authored engine scenes were re-saved by the
engine (`NOX_SAVE_ROUNDTRIP`), so they use the current node format. The Shuto scenes were not
re-saved, because tools generate them.

## Engine scenes

| File | What it is | Used by |
|---|---|---|
| `cornell_box.json` | Cornell box model with a point light and a GUI overlay | `config/config.json` `first_scene`; editor and game presets; `tools/MeasureStartup.ps1`; `docs/building.md` |
| `sponza.json` | Sponza palace and a car model, sun light, authored camera | `config/config.json` `scenes` list; `docs/engine_boundary.md` |
| `scene.json` | "Main Test Scene": physics, animated dinosaur with audio, test room, lights | Default fallback in `Core` when `config.json` has no `first_scene` |
| `animation_test.json` | Skinned `dinosaur_1.glb` with its animation, a light and a GUI | Animation coverage (no code reference) |
| `gui_demo.json` | GUI node with its elements | GUI coverage (no code reference) |
| `lighting_demo.json` | Key, fill, spot and ambient lights on a plane | `docs/nfs2015/benchmarks.md` (lighting row) |
| `multi_light_demo.json` | LPV volume, city model, sun, point and spot lights, audio | Multi-light and LPV coverage (no code reference) |
| `mat_test.json` | Material test model (`materials_test.glb`) | `docs/transmission_completion_audit.md` |
| `lua_sample.json` | Lua project sample scene, `project` = `projects/sample-arena/project.json` | `tests/native/test_lua_project.cpp` |
| `surfel_gi_cornell_validation.json` | Cornell box for surfel GI validation, empty `skybox` on purpose | `packaging/engine.json`; `config/config.json`; `scripts/validate_scene_loading_static.py` |
| `surfel_gi_material_response_validation.json` | Surfel GI material response fixture (sun, `materials_test.glb`) | Validation fixture (no code reference) |
| `surfel_gi_wall_leak_validation.json` | Surfel GI thin-wall leak fixture (cubes and a plane) | Validation fixture; matches the thin wall leak test in `docs/surfel_gi_gibs_implementation_spec.md` |

## Shuto Expressway scenes

| File | What it is | Used by |
|---|---|---|
| `shuto_buildings.json` | Playable Shuto building scene | `docs/shuto/README.md`, `docs/shuto/buildings.md`; written by `tools/shuto/compose_building_scene.py`; `tools/shuto/benchmark_buildings.ps1` default; `tests/native/test_lua_data.cpp` |
| `shuto_rep_section.json` | Representative road section | Written by `tools/shuto/build_scene.py`; base for `compose_building_scene.py`; `docs/shuto/README.md` |
| `shuto_roundtrip.json` | Blender to NOX round-trip scene | Template for `tools/shuto/blender/build_architecture.py`; `docs/shuto/README.md` |
| `shuto_building_probe.json` | Building probe for bulk export checks | Written by `build_architecture.py`; `tools/shuto/validate_building_text.py`; `docs/shuto/buildings.md` |
| `_shuto_traversal_rep_section.json` | Traversal validation events on the representative section | Written by `tools/shuto/traversal.py`; `docs/shuto/README.md` (validation only) |

## Racing scenes (Nox RacingLegacy and the nox-racing project)

These four are owned by the racing content. They are not edited by the engine cleanup.

| File | What it is | Used by |
|---|---|---|
| `nfs_sprint.json` | Night sprint scene, the regression scene for autopilot (`NOX_FIRST_SCENE=nfs_sprint`) | `packaging/nox-racing.json`; `projects/nox-racing/project.json`; `config/launch_presets.json` (racing presets) |
| `nfs_night_street.json` | Night street scene | `packaging/nox-racing.json`; `docs/nfs2015/` |
| `nfs_night_rain.json` | Night street in rain | `packaging/nox-racing.json`; `docs/nfs2015/` |
| `nfs_dawn_street.json` | Dawn street scene | `packaging/nox-racing.json`; `docs/nfs2015/` |

Known issues in these scenes: their procedural street nodes (`street_kit`, `district`) are not
compiled into `Nox_Engine.exe`, so the generic engine loads the cooked results under
`generated/cooked/` instead (log: "not compiled in", which is a notice). The cooked glTF refers to
its textures as `../../../textures/cc0/...`, which does not resolve from `generated/cooked/<key>/`,
so each racing scene logs 28 "Failed to load external uri" errors. The fix belongs to the cook step
or the racing content, not to these scene files.

## Removed in this cleanup

Deleted because nothing references them and they were temporary, duplicate, broken, or superseded:

- Temporary benchmark and roads outputs: `_bench_rep_section`, `_render_quality`,
  `_render_quality_nograin`, `_roads_r3_after`, `_roads_r3_before` (timed out at 120 s),
  `_shuto_traversal_roads_rep_section` (regenerable by `tools/shuto/traversal.py`).
- Duplicates and scratch scenes: `light_test`, `light_test_2`, `studio_lighting` (covered by
  `lighting_demo`), `scene2` (dominoes physics scratch).
- Single-model showcases with no references: `car`, `copied_city`, `graphics_test`, `gundam`
  (the README example now points at `cornell_box.json`), `halo`, `kfc`, `miku`, `otters`,
  `rainbow_road`.
- Test scenes with no references and errors in the generic engine: `shuto_test_site`,
  `nfs_district_test` (its `district` generator is not compiled into `Nox_Engine.exe`, so the
  street node was skipped with an error).

Removed scenes stay in git history if they are needed again.
