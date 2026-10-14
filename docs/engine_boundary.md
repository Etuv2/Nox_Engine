# Engine / game boundary

NOX is the engine. The racing games (the NFS night sprint and the Shuto Expressway section) are
content plus one game module. This page lists the extension points and what lives on each side.
Rule of thumb: an engine file must build, load and render a scene with `--no-game` and must not
name game content (places, paths, IDs, vehicles, night-look values or a hardware profile).

## Current status: what is outside the engine executable

`Nox_Engine.exe` links `NoxEngineCore` only. It contains no compiled racing code and no procedural generators
(checked by `tools/CheckGenericExe.ps1`). The split is:

| Piece | Where it lives now | Built into |
| --- | --- | --- |
| Engine core, scene loader, renderer, physics, audio, Lua runtime | `src/` (except the groups below) | `NoxEngineCore`, `Nox_Engine.exe`, `Nox_RacingLegacy.exe`, `nox_cook`, `nox_tests` |
| Procedural street generators: `ProceduralStreetKit`, `NightStreetPacks`, the `street_kit` and `district` registration (`src/procedural/`) | `NoxProcedural` | `nox_cook` (packaging-time cook) and `Nox_RacingLegacy.exe` |
| Compiled racing game: `GameSession`, `DriveMode`, `RacingHUD`, `WorldMapUI`, `GameEditorPanels`, `LegacyNfsPolicy`, `DriveAudio`, `NoxRacingGame` | `NoxRacingLegacy` | `Nox_RacingLegacy.exe` only (parity comparisons with the Lua project) |
| Racing content: scenes, the R33 vehicle data, Lua entry | `projects/nox-racing/`, `scenes/`, `data/` | runtime content, loaded by `Nox_Engine.exe` |
| Procedural results of the racing streets | `generated/cooked/<generator>-<key>/` (`nox_cook`) | packaged into `game.pak` |

What `Nox_Engine.exe` does with racing content:

- A scene with a procedural node runs the cooked result when one exists for the node's params, `game`
  keys and inputs (`SceneLoader`, `CookedResults`). Without one it logs an `ERROR` that names the generator and the
  `nox_cook` command, skips the node and loads the rest of the scene.
- Racing logic runs only through a project runtime (Lua, `projects/<name>/project.json`), selected by the scene's
  `"project"` key or `--project`. The racing scenes carry that key.

The legacy executable registers the native game with `RegisterNoxRacingGame()` before the window opens
(`src/legacy/main_racing_legacy.cpp`). `Nox_Engine.exe` has no equivalent call. The native game therefore runs only
in `Nox_RacingLegacy.exe`, and only on a scene without a `"project"` key. `--no-game` is accepted by both
executables and has the same meaning.

## Implementation status (2026-10-14)

- Asset paths, packs, bootstrap and writable locations are engine services now: [asset_io.md](asset_io.md).
  Runtime readers use logical paths; the few physical exceptions are listed in that document.
- Project runtimes: a scene's `"project"` key or `--project` selects a descriptor, and its `runtime` picks a
  factory registered in `main.cpp` (`lua` today). The native `GameModules::Create` factory is used only when no
  project is selected and the application registered one (`Nox_RacingLegacy.exe`). See
  [asset_io.md](asset_io.md#project-selection).
- Procedural generators have a cooked-result path (`src/cook/`) so a scene can load without the generator code.
- The Lua runtime and the native vehicle service exist (see [lua_scripting_research.md](lua_scripting_research.md)
  and [lua_api.md](lua_api.md)). Racing scenes run their gameplay from the Lua project.
- Racing data lives in the project: `projects/nox-racing/config/hud.json` (HUD), `projects/nox-racing/data/speedwall.json`
  (sprint data) and `projects/nox-racing/launch_presets.json` (racing presets). `config/` holds engine data only.

## Extension points (engine side)

| Point | Header | Used by the game for |
| --- | --- | --- |
| Game module | `src/GameModule.h` (`IGameModule`, `GameModules::SetFactory`) | `GameSession`: car, races, HUD, menus, world map, camera |
| Procedural generators | `src/SceneExtensions.h` (`RegisterGenerator`) | `street_kit` and `district` (`src/procedural/`, linked into `NoxProcedural` only) |
| Scene keys | `src/SceneExtensions.h` (`RegisterSceneKey`) | `window_room_root`: the room atlas for interior mapping |
| Editor panels, overlay and toolbar icons | `src/EditorExtensions.h` (`EditorHost`, `RegisterPanel`) | Race and route, drive tuning, HUD and night-street panels (`src/GameEditorPanels.cpp`, `Nox_RacingLegacy.exe` only). Lua projects register their own panels (`nox.editor_panel`). |
| Pointer input | `src/ui/Pointer.h` | Menu and world-map hit regions, capture and focus policy |
| Environment | scene `"skybox"` object (`src/EnvironmentMap.h`) | Night HDRI with rotation, intensity, clamp and ground replacement |
| Interior atlas | `SceneRenderExtras::interiorAtlas`, glTF `extras.nox.interior` | Room atlas descriptor (faces per room, styles, pane layout) from `textures/window_rooms/atlas_manifest.json` |
| Collision | `CollisionWorld::TriangleFilter`, `ResolveSphere(..., contactMaxY)` | Wall-sliver filter values and the vehicle roof line (`game.collision`, handling `roof_height`) |
| Particles | `VehicleFX::Emit*(..., floorY)` | Spray and smoke on elevated decks (tyre contact height) |
| Render settings | scene `"render_settings"` and `"quality_tiers"` | The night look (content profile in `tools/shuto/build_scene.py`) |
| Diagnostics | `FrameProfiler` (`NOX_LOG_FRAME_PHASES`) and benchmark `cpu_ms` | Phase marks inside the game module |

The legacy application wires the native game in `src/legacy/main_racing_legacy.cpp` with `RegisterNoxRacingGame()`
(`src/NoxRacingGame.cpp`). The generic `src/main.cpp` registers no native game; it registers the Lua project
runtime. `--no-game` runs the bare engine in both.

The [Lua gameplay scripting research](lua_scripting_research.md) describes the contract that project rules and
component schemas load through: ECS lifetime, Play/Stop, reload, and packaging. The runtime is implemented; the
API is in [lua_api.md](lua_api.md). The [NFS car migration](lua_scripting_research.md#nfs-car-migration-into-project-scripts)
moves driving policies and presentation into project Lua. The native DriveMode remains the parity reference
(`NOX_VEHICLE_POLICY=lua` selects the Lua controller).

## What moved out of the engine this round

- **Core:** owned `GameSession` and initialised the HUD fonts, icons and drive audio. Core now
  holds an `IGameModule` from the registered factory. The `NOX_DRIVE` auto-play check moved into the
  module.
- **SceneLoader:** included `ProceduralStreetKit` and `NightStreetPacks`, dispatched the `street_kit`
  and `district` generator names, built a `RoadNetwork` itself, and owned the `window_room_root` key
  and the `atlas_manifest.json` file name. These are now registry lookups; the game registers the
  producers.
- **Editor:** included `GameSession`, `RacingHUD` and `DriveMode`. The race, drive and HUD panels,
  the race authoring overlay and the "Night Street" rendering tab are now game panels. Toolbar
  icons come from a resolver the game registers, with vector glyphs as the fallback. Resolution
  presets come from `config/render_presets.json` plus the current window size.
- **CollisionWorld:** the wall-sliver filter (0.15 m, 0.05 aspect) was hard-coded for every mesh.
  It now defaults to neutral (no filter); the game sets the values per collision set.
- **DriveMode:** the 1.6 m roof line is now handling data (`roof_height`).
- **RoadNetwork:** `BuildDefaultDistrict` (a demo grid) moved to `ProceduralStreetKit`.
- **VehicleFX:** test emitter positions at the NFS spawn and the "road is at y = 0" floor. Emitters
  are now supplied by the game and carry their own floor height.
- **Interior mapping:** room count, faces per room and style count were compile-time constants,
  and the shader carried game room art (shelves, TV flicker, silhouettes). These are now
  descriptor data with logged limits; the no-atlas fallback is a neutral room. A pre-existing
  mirrored-room bug was fixed.
- **Material identity:** batching and the uniform cache keyed materials by file and primitive. They
  now use a content key over all material fields and bound textures, so identical materials from
  different files share state. This cut G-buffer draws from 1,155 to 220 on the Shuto section.

## Known remaining coupling

- `RoadNetwork` edges still carry typed `lanes`, `lane_width` and `carriageway` fields. These are road-graph
  concepts shared by the game's routing, map and autopilot; moving them into opaque per-edge tags is left for a
  later change. `traffic_side` and `band_walls` are generic network attributes now: they are kept as written and
  read by name (`RoadNetwork::Attribute`, `FlagAttribute`), so the engine assigns them no meaning beyond the lookup.
- `BenchmarkRunner` keeps a default follow-camera offset for shots that do not give one.
- `MotionBlurPass` and the lens-flare reference scale by a 1440-pixel reference height (see the
  rendering report).
- The window title of the racing project comes from its descriptor (`window_title`). Engine presets
  (`config/launch_presets.json`) open `scenes/cornell_box.json`; the racing presets are in
  `projects/nox-racing/launch_presets.json` and name the racing project. These are application data, not engine code.

## Validation

- Bare engine:
  - `Nox_Engine.exe --no-game --game --scene scenes/sponza.json` exits 0.
  - `--no-game --editor --scene scenes/cornell_box.json` exits 0.
  - A scene with a procedural node whose cooked result is missing loads without that node and logs an
    `ERROR` with the `nox_cook` hint (it used to abort the scene load). Before the earlier round it crashed
    in shutdown because the GPU device was released first.
- The editor smoke test passes 11 of 11 with the module, and 8 of 8 without it (the play checks need
  a module). The test no longer rewrites the scene file: it restores it byte-for-byte after
  exercising the save path. Its earlier rewrites put runtime car poses into scenes, which made the
  car float.
- `tools/CheckNoRawGL.ps1` passes.
