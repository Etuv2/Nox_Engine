# NOX night-sprint prototype: launch, play and author

The racing prototype runs on the `scenes/nfs_sprint.json` district (procedural, generated on load). The editor and the standalone game are the same executable.

## Launching

| What | Command (repo root) |
| --- | --- |
| Build Release | `tools\build.cmd` (or `cmake --build --preset vs2022-x64-release`) |
| Editor | `tools\run.cmd -Preset editor` or `build\vs2022-x64\Release\Nox_Engine.exe` |
| Game (borderless, native resolution) | `tools\run.cmd -Preset game` or `Nox_Engine.exe --preset game` |
| Game, windowed 1720x720 | `Nox_Engine.exe --preset game_windowed` |
| Package | `tools\package.cmd`, which writes `Package\NOX_Racing\` (run `Play.cmd` or `Editor.cmd` there) |

- **Command line:** `--preset <name>`, `--game` / `--editor`, `--scene <path>`, `--quality high|ultra`, `--window windowed|borderless|fullscreen`, `--width N --height N`.
- **Presets:** stored in `config/launch_presets.json`. Edit them in the editor under *Build > Launch Presets*.
- **Asset root:** found from the executable's location. A build-tree exe uses the source tree, so editor saves land in the repository. A packaged exe uses its own folder.
- **Environment variables:** no longer required. The `NOX_*` hooks still work for scripted runs; see `configuration.md`.
- **Test scripts:**
  - `NOX_DRIVE_AUTOPILOT=1` drives the race unattended: start, finish, retry, free roam.
  - `NOX_EDITOR_SMOKE=1` exercises the editor: undo/redo, Play/Stop, resize and save.

## Playing

**Loop:** free roam, then drive into the lavender flag and press **Enter** (pad **X**). After the countdown, clear the checkpoints in order to finish. The results screen offers **Retry** or **Free Roam**.

| Action | Keyboard | Gamepad |
| --- | --- | --- |
| Throttle / brake-reverse | W / S (arrows) | RT / LT |
| Steer | A / D (arrows) | left stick |
| Handbrake (drift) | Space | B |
| Nitrous | Shift | A |
| Start event | Enter / Page Down | X |
| Pause menu | Esc / P | Start |
| Menu navigate / select | W, S / Enter | D-pad / A |
| Reset car | R | Back |
| Camera (chase, far, bumper, free) | C | Y |

- **Checkpoints:** a checkpoint counts when the car's swept path crosses its gate moving forward, so high speed cannot skip one. Crossing a later gate first shows *MISSED CHECKPOINT*, and the minimap route keeps pointing at the gate you still need.
- **Wrong way:** a warning appears after about 2 s of driving against the route.
- **Best times:** saved per event in `config/speedwall.json` and shown in the SPEEDWALL panel.
- **BOOST gauge:** shows engine load (throttle x rpm with spool lag). No turbo is simulated.

## Editor

**Layout:**
- Centre: **Scene** (editor camera) and **Game** (chase camera + HUD) tabs.
- Left: Hierarchy and Assets.
- Right: Inspector, Materials, Lighting, Rendering.
- Bottom: Console, Build Output, Profiler, Race & Routes, Drive, HUD.

The layout persists in `config/editor_layout.ini`. *Window > Reset Layout* restores it.

**Toolbar:** scene selector, Save (a dot means unsaved changes), Play / Pause / Stop (F5 / F6 / Shift+F5), Build, Run, Package. Build and Package run `tools/*.ps1` in the background, show progress and log, and only one runs at a time. Run launches the selected preset as a separate process.

**Scene view controls:**

| Control | Action |
| --- | --- |
| Hold RMB + WASD/QE | Fly the camera |
| Wheel | Dolly |
| LMB | Pick a node |
| W / E / R | Gizmo move / rotate / scale |
| F | Focus the selection |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+D | Duplicate |
| Del | Delete |

**Authoring overlay:** the overlay is on by default. Drag the handles on the ground to move:
- the spawn (green),
- event starts (lavender),
- checkpoints (cyan; finish yellow),
- road-network nodes (blue).

Each drag is one undo step.

**Panels:**
- **Race & Routes:** events (id, type, name, difficulty, start, trigger radius, countdown), ordered checkpoints (add, duplicate, reorder, delete, width, heading), speedwall, test buttons and the road network.
- **Drive:** handling, nitrous, spawn, chase camera and headlights. While playing, edits change runtime values only. *Apply to authored* keeps them.
- **HUD:** placement, colours, fonts, gauge ranges and minimap, all live. Save writes `config/hud.json`.
- **Materials:** live material edits. *Save as scene override* stores them in the node's `material_overrides`.

**Play / Stop:** Play starts from the authored scene. Stop restores node transforms and game data to the pre-play state. Saving is disabled while playing.

**Scene switching:** if you have unsaved changes, the editor asks Save / Discard / Cancel.

**Keyboard focus:** keyboard and pad input reach the car only while the Game view has focus and no text field is active.

## Scene data (`"game"` object)

`player_node`, `body_node`, `spawn {position:[x,z], heading_deg}`, `handling {...}`, `camera {...}`, `road_network {road_half_width, sidewalk_width, nodes:[{id,pos}], edges:[{a,b,name}]}`, `events [{id,type,name,difficulty,start,start_heading_deg,trigger_radius,countdown,checkpoints:[{pos,width,heading_deg?}]}]`, `hud`.

- **Shared road network:** the same network drives the generated district (`procedural.generator = "district"`), the minimap, routing and vehicle boundaries.
- **Obstacles:** poles, palms and parked cars come from the generated `generated/<output>_layout.json`.
- **Regenerating geometry:** moving road nodes updates routes, minimap and collision immediately. Save and reopen the scene to regenerate the district geometry.

## Third-party

- Dear ImGui v1.91.9-docking (`include/IMGUI/VERSION.md`).
- Solace UI adaptation at pinned revision `6b88e2e` (`src/editor/solace/THIRD_PARTY.md`).
- HUD fonts: Barlow Condensed and DSEG7, both OFL (`fonts/README.md`).
- Editor font: Geist, OFL (`fonts/solace`).
- Audio is synthesized at runtime (`src/DriveAudio.cpp`).
