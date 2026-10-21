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

- Worker threads: reading and parsing the scene file, expanding prefab references, reading ahead the JSON
  files named in the scene's `game` block (`AssetIO::PrefetchJson`), and decoding glTF models
  (`Scene::PrepareGLTF`: parse and image decode). They touch no GL and no shared engine state.
  `ModelManager::QueuePrefetch` feeds the decode queue for plain, prefab and cooked procedural models;
  `LoadModel` takes the decoded copy.
- Main thread, inside a frame budget of 8 ms: the scene is built in slices (`SliceTask`, a Windows fiber
  that yields when its budget is spent). Yield points are placed per primitive, per texture upload, in the
  vertex loops, per light, per road-network node, per child node, and at the section boundaries of a glTF
  load. The GPU uploads run on the main thread (GL context).
  A root node is built only after its models are decoded, so the build never waits on a decoder.
- Prepare, after the commit and before the loading screen goes away (a slice, `Core::BeginScenePrepare`): the
  renderer builds the first-use state of the scene's passes (`ModularRenderer::PrepareScene`): the interior atlas,
  decal and lens-flare texture arrays (`TextureArrayBuilder`, which yields per layer and charges uploads), and the
  reflection probe captures (one probe per slice step). The first live frame then costs about a normal frame.
  Overlay and loading frames do not advance the scene's animation or jitter state (`SceneFrameClock::live`).
- The environment (HDR decode and radiance pass on a worker, cube conversion, mips, IBL passes) runs as its own
  slice, with yields between stages. The GL state is saved across each yield, so frames drawn meanwhile see
  their own state. The IBL passes use a half-float upload and an RGBA16F environment cube (the RGB16F cube's
  mip generation took about 55 ms per call on the test driver).
- Work that cannot be prefetched (a model that a procedural generator produces without cooked output) is
  parsed on a worker thread while the slice waits: `SliceTask::AwaitYielding(future)` yields to the frame
  between polls. `nox.roads` (declared files and `from_table`) reads, parses and builds the road graph the
  same way, so a city-sized network (tens of MB of JSON) no longer blocks the frame.
- Commit, main thread, sliced as well: clearing the old scene, scene state, shadow maps, lights, render
  settings, game module configure, editor and UI, bounds, acceleration structure. Each step is timed in
  the log (`[Core] Scene commit step '...': N ms`). The old physics bodies are removed at commit. The commit's share of the loading bar moves by the estimated cost
of each step (`CommitStepWeight` in `src/Core.cpp`), not by step count.
- After the commit, the loading overlay stays up over the new scene until its reflection probes are
  captured (sliced, a few probes per frame) and the first frame has been drawn.

API (`src/SceneLoader.h`):

```cpp
bool BeginLoad(const std::string& path);              // false while another load runs
LoadStatus StepLoad(double budgetMs);                 // budgetMs < 0 runs to completion
LoadStatus GetLoadStatus() const;                     // Idle, Running, Ready, Failed
std::shared_ptr<SceneGraph> TakeLoadedScene();        // Ready only; the new scene is committed by Core
void CancelLoad();
LoadProgress GetLoadProgress() const;                 // fraction, stage, sceneName, sceneFile
```

Slicing a long build (`src/SliceTask.h`): `SliceTask task([&] { ... });` runs the body on its own stack.
`task.Resume(budgetMs)` runs it until it finishes or spends the budget. Inside the body,
`SliceTask::YieldIfOverBudget()` is a free call that does nothing outside a task, so code shared with
synchronous callers is unchanged. Only the Windows build has fibers; elsewhere a task runs to completion.

Custom loading screen (`src/SceneLoadingScreen.h`): call `SceneLoadingScreen::SetHook(fn)`. The hook
receives a `SceneLoadingScreenState` every frame (progress, stage, scene name, error) and draws with ImGui
inside the open frame. It returns `SceneLoadingScreenAction::Dismiss` when the user accepts a failed load.
Without a hook the engine draws its default screen: full screen in game mode, and a dimmed overlay over the
editor, whose UI is disabled while the load runs.

Failure: the error stays on the loading screen until the user presses Continue (Quit when no scene is
loaded yet). The current scene keeps running. The failed load leaves nothing behind: its physics bodies are
removed and the physics config is restored.

Environment:

- `NOX_SYNC_LOAD=1`: the startup scene loads blocking (the same steps, without frame pacing). Frame-indexed
  hooks give the same frame numbers either way, so the flag is only needed for timing experiments.
- `NOX_FIXED_DT` runs: loading frames do not step the simulation, so results are unchanged.
- `NOX_UPLOAD_BUDGET_MB=<mb>` (default 2): GPU upload budget per frame. Buffers and textures created with data
  count their bytes against the frame (`SliceTask::ChargeUpload`); once the frame's budget is spent the task yields
  to the frame, so one frame never submits a large backlog.
- `NOX_LOAD_DUMP_DIR=<dir>` with `NOX_LOAD_DUMP_EVERY=<n>` (default 10): a PNG of every Nth load-phase frame
  (`load_NNNNN.png`), taken before the swap. Counts load frames only.
- `NOX_IBL_VALIDATE=1`: runs the IBL validation readbacks (one texel per pass; NaN and sign checks). They are off by
  default because each stalls for the GPU work it reads (about 70 ms on the test scenes).
- `NOX_LOAD_TRACE=1`: one `[LoadTrace]` line per load-phase frame with the loop time (`frame_ms`), the update,
  render and present times, the load service time and the step that ran (`step=`). Use it to find the frames
  that hitch.
- `NOX_SLICE_GAP_MS=<ms>`: logs (stderr, `[SliceGap]`) each stretch of work longer than that between two yield
  points in a slice, with the file and line of both ends. A stretch is only measured while the slice runs, so
  time spent suspended across frames is not counted.

Scene frames (`src/SceneFrameClock.h`): frame-indexed test hooks count scene frames, not raw frames. A
scene frame is a frame shown live: the scene is committed and no loading screen is drawn over it. The commit
frame and the frame(s) that draw the loading overlay (`PresentSceneOverlay`, reflection probe capture) are
not counted. The first live frame is 1. Every scene commit resets the count, so a scene swap numbers its
frames from its own first live frame. `nox::SceneFrameClock::total` is the raw count of updated frames, loading
frames included. `engine.info` reports `scene_frame` next to the raw `frame`.

Hooks indexed by scene frame (all skip loading frames; a dump never shows a loading screen):

| Hook | Scene frame meaning |
|------|---------------------|
| `NOX_FRAMEBUFFER_DUMP_FRAMES`, `NOX_FRAMEBUFFER_DUMP_FRAME` | scene frame of the scene framebuffer dump |
| `NOX_FINAL_DUMP_FRAMES`, `NOX_FINAL_DUMP_EVERY`, `NOX_FINAL_DUMP_START` | scene frame of the backbuffer dump (file `final_NNNNN.png`) |
| `NOX_EXIT_AFTER_FRAMES` | quit at this scene frame (of the scene live at that time) |
| `NOX_CAMERA_PATH` | path frame 0 is the first live frame of the scene |
| `NOX_SWAP_SCENE=frame:scene.json` | the swap is requested at this scene frame of the scene that is live then; fires once |
| `NOX_LIGHT_STEP=frame:scale` | the step starts at this scene frame |
| `NOX_LOG_PASS_METRICS` (`_INTERVAL`) | the log cadence counts live frames |
| `NOX_SSR_DUMP_FRAMES`, `NOX_SURFEL_GI_DUMP_FRAME*` | scene frame of the SSR / surfel GI texture dumps |
| Benchmark shots (`NOX_BENCHMARK`) | warmup and shot timelines advance by live frames only |
| Editor smoke (`NOX_EDITOR_SMOKE`) | the check schedule starts at the first live frame of the editor's startup scene |

`NOX_EXIT_AFTER_FRAMES` has a wall-clock safety net: `NOX_EXIT_AFTER_SECONDS` (default 600) quits the run
when that much wall time has passed, with a warning that names the frame reached. A stuck load then cannot
hang a scripted run.
- Log lines: `[Core] Scene ready: ... (load N ms, longest frame while loading M ms)`,
  `[Core] Scene first frame presented: N ms after the load started`, `[Core] Long frame during scene load:
  N ms (step)` for any loading frame over 50 ms.

Known limits:

- A single primitive's GPU upload (`CreateMesh`) is one call. A very large primitive is one frame's work.
- The game module's configure is one call from the engine's side. The racing module's road network setup
  is the largest commit step on shuto_rep_section; its JSON is read ahead, but the rest of its configure
  runs in one step.
- Texture-array builds that the renderer creates on its first frame are not sliced. They run in the first
  frame of the new scene, under the loading overlay.
- Live procedural generators (not cooked) run on the main thread, and their models decode there.

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
