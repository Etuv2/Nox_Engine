# Racing presentation (Lua): HUD, minimap, world map, menus

Pure presentation modules. They read one state table per frame and return UI actions. They never
change race state: the session (race, menus, waypoint, best times) applies the actions.

| File | Role |
| --- | --- |
| `config.lua` | HUD configuration: defaults (the native HudConfig) and `apply(decoded config/hud.json)` |
| `draw.lua` | Drawing helpers over `ui.*`: 640x360 reference canvas, anchors, glow strokes/text, colours |
| `hud.lua` | Cluster (tachometer, speed, gear, boost, N2O), event flag, checkpoint gate, panel, countdown, results, menus, messages, hint |
| `minimap.lua` | Heading-up minimap from `nox.roads` edge polylines |
| `roads.lua` | Per-handle cache of road polylines (shared by minimap and map) |
| `menus.lua` | Menu / START EVENT geometry, pointer hit regions (from the last draw), pointer actions |
| `worldmap.lua` | Full-screen map: pan, zoom, markers, route, waypoint (button, click snapped to a road), labels |
| `fixtures.lua` | Fixture states for the preview (nfs_sprint values, the native capture states) |
| `preview.lua` | Debug preview: cycles the fixtures (90 frames each), declared by `presentation.preview` |

## Session wiring

```lua
local hud = require("ui.hud")
local worldmap = require("ui.worldmap")
local config = require("ui.config")

-- once, when the project loads (after the config binding exists, request R1):
config.apply(nox.json("config/hud.json"))          -- or config.apply(nil) for the defaults

-- system (gameplay thread), once per frame:
local act = hud.frame(hud_state, {
  view = { x = 0, y = 0, w = W, h = H },            -- overlay rect in window coordinates
  pointer = { x = px, y = py },                     -- ctx:pointer_position(), nil when not on the view
  moved = moved, pressed = ctx:pointer_pressed("left"), released = ctx:pointer_released("left"),
  down = ctx:pointer_down("left"), wheel = ctx:pointer_wheel(),
  selected = menu_selected,                         -- the session's 1-based menu selection
}, dt)
-- act.menu_select  -> session: menu_selected = act.menu_select
-- act.menu_click   -> session: menu_selected = act.menu_click; activate that item
-- act.menu_wheel   -> session: step the selection by act.menu_wheel (-1 previous, +1 next)
-- act.start_click  -> session: start the event (the START EVENT prompt)

-- draw handler (GUI thread): the overlay of the last frame
hud.draw(ui, dt)

-- map (while open): a system calls worldmap.frame; the draw handler calls worldmap.draw
local m = worldmap.frame(map_ctx, map_input, dt)
worldmap.draw(ui, dt)
```

Keys and pad stay with the session (it owns selection and activation: `confirm`, `back`, `up/down`
map to `input.json` actions). The pointer is the presentation's: hover, press and click on items
and the START prompt are reported through `act`. The wheel over an open menu is reported as
`menu_wheel`.

## HUD state (hud_state)

Mirrors `HudState` (src/RacingHUD.h). Units: km/h, rpm, seconds, metres; angles in radians
(0 = +z, growing toward +x). Missing fields take the defaults shown.

| Field | Type | Meaning |
| --- | --- | --- |
| `speed_kmh` | number | speed; the unit label comes from config (`gauges.speed_units`) |
| `rpm` | number | engine rpm |
| `load` | number 0..1 | boost gauge (`gauges.boost_max` scales it) |
| `nitro` | number 0..1 | N2O tank |
| `nitro_active`, `nitro_recharging` | bool | N2O flicker / recharge comet |
| `gear` | int | -1 = R, 0 = N, 1..6 |
| `redline`, `limiter` | bool | redline flash (rpm at the redline / rev limiter hit) |
| `car` | `{x, z}` | player position (world metres) |
| `heading` | number | player heading, radians |
| `layered` | bool | roads on other levels are drawn dimmed (needs R9) |
| `roads` | `nox.roads` handle or nil | minimap and map road network |
| `route` | array of `{x, z}` | guidance polyline |
| `markers` | array of `{x, z, kind, label}` | minimap markers; `kind` = `event` (label = event type), `checkpoint`, `next_checkpoint`, `finish`, `start`, `place` |
| `phase` | string | `free_roam`, `near_event`, `countdown`, `racing`, `finished`, `paused` |
| `event_type`, `event_name`, `event_difficulty` | string | the shown event |
| `start_key` | string | label of the start key/button (`Enter` or `X`) |
| `countdown` | number | seconds left (countdown phase) |
| `race_time` | number | race clock |
| `checkpoint`, `checkpoint_count` | int | checkpoints cleared / total |
| `best_time` | number | local best of the shown event (negative = none) |
| `best_holder` | string | name on the best time (default `You`) |
| `last_time` | number | finish time of the last run (negative = none) |
| `new_record` | bool | results: new local record |
| `splits` | array of numbers | split deltas; the panel shows the last one |
| `message`, `message_alpha` | string, number | centre message; alpha 0..1 (the session fades it: `min(timer*2, 1)`) |
| `menu` | `{title, items, selected}` or nil | open menu (`PAUSED`, `RESULTS`); `items` are strings, `selected` 1-based |
| `hint` | string | small top-left hint (camera view etc.) |
| `event_marker` | `{x, y, distance}` or nil | world-anchored event flag; `x, y` normalised 0..1 in the view (y down), `distance` metres. Visible only in free roam (`visible = true`) |
| `checkpoint_marker` | `{x, y, distance}` or nil | next checkpoint gate, same convention |

`event_marker` and `checkpoint_marker` need a camera projection (request R3). Until it exists the
session passes nil and nothing is drawn.

## Map context (map_ctx) and input

```
map_ctx = {
  events = { { x, z, title, type, has_end, end_x, end_z } ... },  -- event starts (world metres)
  places = { { x, z } ... },                                       -- parking areas
  player = { x, z }, heading = number,
  route = array of {x, z} or nil,                                  -- active guidance route
  preview_route = array of {x, z} or nil,                          -- displayed event's routed checkpoints
  has_waypoint = bool, waypoint = { x, z } or nil,
  selected_event = int (1-based),
  roads = nox.roads handle or nil,
}
map_input = {
  view = { x, y, w, h },            -- overlay rect in window coordinates
  pointer = { x, y } | nil,         -- window coordinates
  pressed, released, down = bool (left button), moved, wheel = number,
  right_clicked = bool,
  pan = { x, y } (-1..1),           -- keys / left stick
  zoom_rate = number (-1..1), zoom_steps = number,
  recenter, confirm, close = bool,  -- F / C / pad Y; Enter / pad A; M / Backspace / pad B
  cycle = -1 | 0 | 1,               -- previous / next event
}
```

`worldmap.frame` returns `{ close, toggle_waypoint = i, selected = i, place_waypoint = {x, z},
clear_waypoint }`:
- `toggle_waypoint = i`: set or clear the waypoint on event `i` (session: the native rule, "WAYPOINT SET - name"
  or "WAYPOINT CLEARED" when it was already there).
- `place_waypoint`: a click on the map snapped to the nearest road point within 40 px (the session sets
  the waypoint there). A click that is not near a road does nothing.
- `clear_waypoint`: right click or the Clear button.
- `selected = i`: a marker click changed the selection (the session plays the select sound).
- `close`: the map closed (the session stops drawing it).

## Limits and requests

Requests for the lead and the bindings owners are in `%TEMP%\nox_team\impl\presentation\requests_for_lead.md`.
Summary of what the presentation waits for:

- R1 `nox.json(logical)` read-only decoded JSON through AssetIO (config/hud.json, map metadata).
- R2 an authoring save (`nox.save_authoring_json(logical, value)`) for the HUD panel (loose mode only).
- R3 camera projection (`ctx:camera_project(x, y, z)`): event flag and checkpoint gate.
- R4 a textured image primitive for the world-map base image.
- R5 `ui.icon_exists(id)` so vector fallbacks draw when the atlas lacks an icon.
- R9 per-view road queries (edges within a rectangle, half-width per edge), since `edge_polylines`
  returns every edge each call and the draw budget is 32768 points per frame.
- R11 a read-only project descriptor access (for the `presentation.preview` flag).

Known differences from the native HUD until those land: no world-anchored flag/gate (nil), no
map base image, no carriageway classes or district/water labels, plain square clip for the minimap
(native: circular), road widths constant, vector fallbacks for icons not drawn when the icon is
missing, and the glow is an approximation (offset copies, not a blurred texture).
