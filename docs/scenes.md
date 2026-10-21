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

## Loading

Scenes load in the background. Every frame the window keeps pumping events and draws a loading screen
with the scene name, a progress bar, the current step and a spinner. The simulation does not run while a
scene loads, and the previous scene stays live until the new one is ready.

These paths load this way: the startup scene, editor scene switches (`Core::SwapScene`), Lua
`ctx.load_scene`, and `NOX_SWAP_SCENE`. The blocking `SceneLoader::LoadScene`, runtime state restore
(`LoadSceneState`) and the round-trip tools still load synchronously.

Where the work runs:

- Worker threads: reading and parsing the scene file, and decoding glTF models (`Scene::PrepareGLTF`:
  parse and image decode). They touch no GL and no shared engine state. `ModelManager::QueuePrefetch`
  feeds the decode queue; `LoadModel` takes the decoded copy.
- Main thread, one step per frame inside an 8 ms budget: building the nodes, ECS entities, physics bodies
  and procedural nodes, and uploading models to the GPU (`Scene::LoadFromGLTF`). A root node is built
  only after its models are decoded, so the build never waits on a decoder.
- Commit, main thread: the new scene replaces the old one (lights and shadows are set up, the game module
  is configured), and the physics bodies of the replaced scene are removed.

API (`src/SceneLoader.h`):

```cpp
bool BeginLoad(const std::string& path);              // false while another load runs
LoadStatus StepLoad(double budgetMs);                 // budgetMs < 0 runs to completion
LoadStatus GetLoadStatus() const;                     // Idle, Running, Ready, Failed
std::shared_ptr<SceneGraph> TakeLoadedScene();        // Ready only; the new scene is committed by Core
void CancelLoad();
LoadProgress GetLoadProgress() const;                 // fraction, stage, sceneName, sceneFile
```

Custom loading screen (`src/SceneLoadingScreen.h`): call `SceneLoadingScreen::SetHook(fn)`. The hook
receives a `SceneLoadingScreenState` every frame (progress, stage, scene name, error) and draws with ImGui
inside the open frame. It returns `SceneLoadingScreenAction::Dismiss` when the user accepts a failed load.
Without a hook the engine draws its default screen: full screen in game mode, and a dimmed overlay over the
editor, whose UI is disabled while the load runs.

Failure: the error stays on the loading screen until the user presses Continue (Quit when no scene is
loaded yet). The current scene keeps running. The failed load leaves nothing behind: its physics bodies are
removed and the physics config is restored.

Environment:

- `NOX_SYNC_LOAD=1`: the startup scene loads blocking, as before.
- `NOX_EXIT_AFTER_FRAMES` counts only frames that are not loading, so a long load does not cut a scripted
  run short. `NOX_FINAL_DUMP_FRAMES` counts every frame, loading frames included.
- `NOX_FIXED_DT` runs: loading frames do not step the simulation, so results are unchanged.
- The log line `[Core] Scene ready: ... (load N ms, longest frame while loading M ms)` reports the load.

Known limits:

- Each model is built in one main-thread step (GL buffers and textures). A large model takes 100 to 400 ms,
  so that is the longest frame during a load of a large scene.
- Commit is a single step. The shadow-array allocation and the game module's configure run there.
- Procedural nodes and prefab references are not prefetched. Their models decode on the main thread.

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
