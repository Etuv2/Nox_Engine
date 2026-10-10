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

## Vehicle, camera and effects (rework, 2026-10-10)

**Car rig:** `models/car_r33_rig.glb` was split from `models/car.glb` with headless Blender. It contains:
- `Body`, sitting on the ground origin;
- `Wheel_FL`, `Wheel_FR`, `Wheel_RL` and `Wheel_RR`, each pivoting on its hub;
- `ContactShadow`.

The Blender scripts are under `nox_ext/blender`, outside the repo; to rebuild, re-run `rig_car.py` and then `fit_hubs.py`.

**Wheel animation:**
- Wheels spin with rolling speed, plus wheelspin on the rear axle.
- The front wheels steer.
- Each wheel follows the road height within the suspension travel.
- Display is smoothed per frame, and the spin step is capped so wheels don't strobe at speed.

**Dynamics** (`DriveMode.cpp`, 240 Hz sub-steps):
- Two-axle tyre model with a friction ellipse, plus longitudinal and lateral load transfer.
- Rear-wheel drive through 6 gears. Drive force is traction-limited, so excess torque becomes wheelspin.
- Burnout: hold throttle and brake while stopped.
- Arcade aids: grip-mode stability, slip-rate damping, countersteer assist and a spin-out guard.

**Drifts:**
- Entry by handbrake, by a brake tap under 0.4 s, or by power-over.
- The drift side is latched at entry.
- Steering into the drift holds the angle; neutral or countersteer straightens out.
- An arcade velocity swing carries the slide around 90-degree corners.

**Suspension:** the sprung body (heave, pitch, roll) reacts to load transfer and to road bumps and curbs. Tuning is under `game.handling` and in the editor's Drive panel.

**Camera** (`game.camera`, `version` 2):
- Set to 4.8 m behind the car, 1.72 m high, about 4.5 deg down, 54 deg vertical FOV.
- Measured against the reference at 640x360:
  - roof at 50% of frame height (reference 50%);
  - bumper at 93% (reference 96%);
  - horizon at 43% (reference 42%).
- The follow spring works on the car-relative offset, so there is no speed lag.
- Drift framing blends the heading with the velocity direction.
- Speed and nitrous effects are restrained.

**Tyre effects:** `VehicleFX` with `passes/VehicleFXPass` (soft particles lit from the fog volume):
- Smoke comes from wheelspin, sliding and locked wheels.
- Wet spray scales with speed.
- Both are emitted from grounded wheels only.

**City:**
- CC0 Poly Haven props (`models/cc0`, `docs/nfs2015/assets.md`) are placed by the district generator and are solid to the car.
- The district is denser: alleys, fire escapes, parking lots, signal gantries and street signs.

**Windows:** parallax interior mapping (`shaders/includes/interior_mapping.glsl`) on district windows, with rooms, lamps, blinds, curtains and occupancy.

**Engine fixes found while validating:**
- SSE2/AVX2 `UpdateWorldBounds` skipped the last `count % 4` (or `% 8`) render items in dense mode, which left them frustum-culled.
- Imported glTF sub-nodes are no longer written back into scene files on save.
- The VehicleFX pass is always scheduled, because the frame plan is cached.

**Test hooks:**

| Variable | Effect |
| --- | --- |
| `NOX_DRIVE_SCRIPT=maneuvers` | Burnout, launch, brake, weave, two drifts; prints telemetry |
| `NOX_DRIVE_AUTOPILOT=1` | Drives the race |
| `NOX_FIXED_DT=<s>` | Fixed timestep |
| `NOX_FINAL_DUMP_EVERY=N` | Frame sequences |
| `NOX_CAM_ORBIT=<deg>` | Rotates the chase camera |
| `NOX_CAM_POSE=ex,ey,ez,tx,ty,tz` | Fixed camera pose |
| `NOX_TEST_WHEELSPIN=<rad>` | Sets the wheel spin angle |
