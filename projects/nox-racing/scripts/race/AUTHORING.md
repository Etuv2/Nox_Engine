# Race project: authoring data model

Status: phase 3 (session worker). Owner of this file: session worker. The editor panels (Race & Routes,
Drive tuning, viewport handles) are a separate worker and read this model.

## Where the data lives

- A scene's top-level `"project": "projects/nox-racing/project.json"` selects the Lua game.
- The scene's `"game"` object is the authoring data. Keys are the same as the native GameSession. Unknown
  keys are preserved on save (`SaveGameJson`, bindings2 hook). The Lua side reads it with `nox.game()`.
- Vehicle definition: `projects/nox-racing/data/vehicles/r33.json` (geometry, tuning, rig, default spawn,
  controller params). Scene overrides in `game` win for: `spawn`, `handling` (controller handling and the
  shared tuning keys), `camera`.
- Save data (best times, runtime): user directory, `speedwall.json`, seeded from `config/speedwall.json`
  when absent. Never written into the project or the scene.

## `game` keys (read by scripts/race/session.lua)

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `player_node` | string | `PlayerCar` | scene node that carries the declared race state (`SessionRace`) |
| `body_node` | string | `PlayerCarBody` | visual body node (camera and pose) |
| `spawn` | `{position:[x,z] or [x,y,z], heading_deg}` | r33 data | car spawn for free roam and reset |
| `handling` | object | r33 controller handling | overrides of the controller handling and shared tuning |
| `camera` | object | native defaults | chase rig: `distance`, `height`, `look_ahead`, `look_height`, `heading_lag`, `velocity_follow`, `stiffness`, `pull_back`, `fov`, `fov_speed_gain`, `fov_nitro`, `version` |
| `road_network` | inline RoadNetwork JSON | none | routes and guidance (`nox.roads.from_table`) |
| `events` | array | `[]` | race events (below) |
| `places` | array | `[]` | parking areas (below) |
| `hud` | string | `config/hud.json` | HUD configuration path (read by the presentation through `nox.json`) |
| `collision` | `{files, height_tolerance, ground_min_normal_y, wall_min_height, wall_min_aspect}` | none | stacked-road height gate; files are glTF paths (declared collision, see the gaps list) |
| `pause_on_focus_loss` | bool | true | losing window focus opens the pause menu (never in autopilot or fixture runs) |

## Event (`events[i]`)

| Key | Default | Meaning |
| --- | --- | --- |
| `id` | `sprint_01` | save key of the best time |
| `type`, `name`, `difficulty` | `SPRINT RACE`, `A Legend's Wheels`, `Hard` | shown in the HUD and results |
| `start` | required | `[x,z]` or `[x,y,z]`, the event marker on the ground |
| `start_heading_deg` | 0 | heading at the start |
| `trigger_radius` | 9 | free roam: distance that opens the start prompt |
| `countdown` | 3 | seconds (minimum 1) |
| `checkpoints` | required | array of `{pos, width (18), heading_deg?}` or `[x,z]`; the last one is the finish |

Gate heading: `heading_deg` when authored, else the direction of the routed path from the previous gate
(or the start) to the gate. The gate is crossed when the swept car segment of the frame intersects the gate
line within `height_tolerance` (default 4 m) of the gate height when the gate has a `y`. A gate crossed
backwards does not count.

## Place (`places[i]`)

`{id, type = "PARKING AREA", name, name_ja, pos, radius = 40}`. Arriving slowly (under 80 km/h) refills the
car's nitrous and shows the place name and the events nearby (1500 m).

## Declared race state (`SessionRace` schema, on the player node)

Fields: `phase` (0 free roam, 1 countdown, 2 racing, 3 finished), `event`, `time`, `countdown`, `next_cp`,
`beep`, `last_time`, `new_record`, `wrong_way`, `paused`. It survives a code reload through the declared
state. The chase camera smoothing is declared the same way (`ChaseCamera` on the body node).

## Validation rules (authoring errors)

- An event without checkpoints is skipped with a log line, not an error.
- An unknown `id` or a duplicate `id` shares the best-time record (warning logged).
- `spawn.position` must be finite; otherwise the r33 default is used with a log line.
