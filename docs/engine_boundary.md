# Engine / game boundary

NOX is the engine. The racing games (the NFS night sprint and the Shuto Expressway section) are
content plus one game module. This page lists the extension points and what lives on each side.
Rule of thumb: an engine file must build, load and render a scene with `--no-game` and must not
name game content (places, paths, IDs, vehicles, night-look values or a hardware profile).

## Extension points (engine side)

| Point | Header | Used by the game for |
| --- | --- | --- |
| Game module | `src/GameModule.h` (`IGameModule`, `GameModules::SetFactory`) | `GameSession`: car, races, HUD, menus, world map, camera |
| Procedural generators | `src/SceneExtensions.h` (`RegisterGenerator`) | `street_kit` and `district` (ProceduralStreetKit) |
| Scene keys | `src/SceneExtensions.h` (`RegisterSceneKey`) | `window_room_root`: the room atlas for interior mapping |
| Editor panels, overlay and toolbar icons | `src/EditorExtensions.h` (`EditorHost`, `RegisterPanel`) | Race and route, drive tuning, HUD and night-street panels (`src/GameEditorPanels.cpp`) |
| Pointer input | `src/ui/Pointer.h` | Menu and world-map hit regions, capture and focus policy |
| Environment | scene `"skybox"` object (`src/EnvironmentMap.h`) | Night HDRI with rotation, intensity, clamp and ground replacement |
| Interior atlas | `SceneRenderExtras::interiorAtlas`, glTF `extras.nox.interior` | Room atlas descriptor (faces per room, styles, pane layout) from `textures/window_rooms/atlas_manifest.json` |
| Collision | `CollisionWorld::TriangleFilter`, `ResolveSphere(..., contactMaxY)` | Wall-sliver filter values and the vehicle roof line (`game.collision`, handling `roof_height`) |
| Particles | `VehicleFX::Emit*(..., floorY)` | Spray and smoke on elevated decks (tyre contact height) |
| Render settings | scene `"render_settings"` and `"quality_tiers"` | The night look (content profile in `tools/shuto/build_scene.py`) |
| Diagnostics | `FrameProfiler` (`NOX_LOG_FRAME_PHASES`) and benchmark `cpu_ms` | Phase marks inside the game module |

The application wires the game in `src/main.cpp` with `RegisterNoxRacingGame()` (`src/NoxRacingGame.cpp`).
`--no-game` skips the registration and runs the bare engine.

The [Lua gameplay scripting proposal](lua_scripting_research.md) builds on this contract so
project rules and component schemas can load as content. It documents ECS lifetime, Play/Stop,
reload, and packaging prerequisites; Lua is not integrated into the engine yet.
Its [NFS car migration](lua_scripting_research.md#nfs-car-migration-into-project-scripts) explicitly
moves driving policies/presentation to project Lua while extracting reusable native vehicle
calculations. A Lua wrapper around the current compiled DriveMode is an intermediate step.

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

- `RoadNetwork` still reads `traffic_side` and `band_walls`, and its edges carry `lanes`,
  `lane_width` and `carriageway`. These are road-graph concepts shared by the game's routing, map
  and autopilot; moving them into opaque per-edge tags is left for a later change.
- `BenchmarkRunner` keeps a default follow-camera offset for shots that do not give one.
- `MotionBlurPass` and the lens-flare reference scale by a 1440-pixel reference height (see the
  rendering report).
- The `main.cpp` window title and the launch presets name the racing game. These are application
  data, not engine code.

## Validation

- Bare engine:
  - `Nox_Engine.exe --no-game --game --scene scenes/sponza.json` exits 0.
  - `--no-game --editor --scene scenes/cornell_box.json` exits 0.
  - A scene that needs the game's generator under `--no-game` fails to load, reports the error and
    exits 1. Before this round it crashed in shutdown because the GPU device was released first.
- The editor smoke test passes 11 of 11 with the module, and 8 of 8 without it (the play checks need
  a module). The test no longer rewrites the scene file: it restores it byte-for-byte after
  exercising the save path. Its earlier rewrites put runtime car poses into scenes, which made the
  car float.
- `tools/CheckNoRawGL.ps1` passes.
