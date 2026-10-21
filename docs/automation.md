# Automation server and MCP tools

The engine can be driven and inspected from outside while it runs. An in-engine JSON-RPC server
answers commands about the scene, nodes, render settings, log, camera and input. A small MCP server
(`tools/nox_mcp/server.py`) exposes the same commands to Claude Code, and `tools/nox_mcp/nox_client.py`
does the same from a shell. The server is off unless asked for.

- Code: `src/automation/` (`AutomationProtocol` JSON-RPC layer, `AutomationServer` socket thread and frame hooks,
  `AutomationCommands` engine commands). Hooks: one line at the top of `Core::Update`, one before `Present` in
  `MainWindow::Run`, and the start/stop in `RunNoxEngine`.
- Tests: `tests/native/test_automation.cpp` (protocol and options, headless), `tools/nox_mcp/test_e2e.py` (launches the engine).

## Turning it on

```
Nox_Engine.exe --automation                 # 127.0.0.1:47800
Nox_Engine.exe --automation=47810           # 127.0.0.1:47810
NOX_AUTOMATION_PORT=47810 Nox_Engine.exe    # same, from the environment
```

The command line port wins over the variable. A malformed port is reported on stderr and the server stays off.
The server listens on loopback only (`127.0.0.1`); nothing else can reach it.

## Protocol

JSON-RPC 2.0, one JSON object per line, `\n` terminated, UTF-8. Requests and responses use the same framing.

```
-> {"jsonrpc":"2.0","id":1,"method":"engine.info","params":{}}
<- {"jsonrpc":"2.0","id":1,"result":{"automation_port":47800,"mode":"editor", ...}}
```

- `params` must be an object (or absent). `id` is a string, an integer or null.
- A request without `id` is a notification: it runs and gets no reply.
- Batch arrays are rejected (`-32600`).
- Errors come back as `{"error": {"code", "message", "data"?}}`:

| code | meaning |
|---|---|
| -32700 | the line is not JSON |
| -32600 | not a valid request (missing or non-string method, wrong `jsonrpc`, batch) |
| -32601 | unknown method |
| -32602 | invalid parameters (missing, wrong type, out of range, unknown node/component/property) |
| -32603 | internal error (a handler threw; the engine keeps running) |
| -32000 | the command could not be carried out in the current state (not in editor mode, Lua error, file not written, play mode ...) |

A line longer than 16 MB closes the connection. The server answers one request per connection at a time, in
arrival order. A command that has not been picked up within 120 seconds fails with -32000 (for example when the
window is hidden and the main loop is not running).

### Threads and frame safety

A background thread owns the sockets and queues each request. Every command runs on the main thread, either at
the top of `Core::Update` (before any simulation step) or, for screenshots and `engine.step`, after `Render` and
before `Present`. Engine state is never touched from the network thread.

### Node ids

`scene.tree`, `node.find`, `node.create` and the like return node ids. An id is an integer chosen when the node is
first seen, and it stays the same while that node exists. It is not saved in scene files and it changes between
runs. Use `scene.tree` or `node.find` to get ids again.

## Commands

Examples are real responses from a run on `scenes/automation_e2e.json` (abridged where marked).

### engine.info

Engine state. No parameters.

```
-> {"jsonrpc":"2.0","id":1,"method":"engine.info"}
<- {"jsonrpc":"2.0","id":1,"result":{"automation_port":47800,"fps":94.5,"frame":4480,"loading":false,
    "mode":"editor","paused":false,"playing":false,"protocol":"2.0","scene":"scenes/automation_e2e.json",
    "scene_name":"automation e2e","tier":"","version":"1.0.0"}}
```

`mode` is `editor` or `game`. `loading` is true while a scene swap is pending. `frame` counts simulation frames
run. `tier` is the quality tier of the scene (empty when the scene names none).

### engine.quit

Asks the engine to exit after the current frame. Returns `{"quitting": true}`.

### engine.pause / engine.resume

Stop or restart the simulation. Rendering continues while paused, so screenshots and `scene.tree` keep working.
`engine.pause` returns `{"paused": true, "frame": N}`. `engine.resume` returns `{"paused": false, "frame": N}` and
cancels a step in progress.

### engine.step

```
-> {"jsonrpc":"2.0","id":2,"method":"engine.step","params":{"frames":5}}
<- {"jsonrpc":"2.0","id":2,"result":{"frame":9324,"paused":true}}
```

Pauses, then simulates exactly `frames` frames (1 to 100000, default 1). The reply comes after the last of them has
rendered. Frames are counted in simulation steps, so a paused engine advances only through `engine.step`.

### scene.list

Scene files under `scenes/`, as logical paths. `["scenes/automation_e2e.json", "scenes/cornell_box.json", ...]`

### scene.load

```
-> {"jsonrpc":"2.0","id":3,"method":"scene.load","params":{"path":"scenes/cornell_box.json"}}
<- {"jsonrpc":"2.0","id":3,"result":{"path":"scenes/cornell_box.json","requested":true}}
```

Queues the normal scene swap, applied at the start of the next frame. Poll `engine.info` until `loading` is false and
`scene` shows the new path. A path that does not exist is -32602.

### scene.save

`{}` saves the authored scene to its source file (the same as the editor's Save). Failures are written to the log;
read them with `log.tail`. `{"path": "captures/automation/copy.json"}` writes the scene graph to that file (`.json`
only). It does not write the game object or project data, so it is for inspection, not for authoring.

### scene.tree

Node hierarchy. `depth` limits the levels below the root (`-1`, the default, is unlimited).

```
-> {"jsonrpc":"2.0","id":4,"method":"scene.tree","params":{"depth":1}}
<- {"jsonrpc":"2.0","id":4,"result":{"id":1,"name":"Node","type":"node","folder":false,
    "components":["Node","Transform","WorldEnvironment"],"child_count":2,"children":[
      {"id":2,"name":"E2E_Box","type":"model","folder":false,"components":["Node","Transform","MeshRenderer"],
       "child_count":1,"children":[]},
      {"id":4,"name":"E2E_Light","type":"light","folder":false,"components":["Node","Transform","Light"],
       "child_count":0,"children":[]}]}}
```

`type` is one of `node`, `model`, `light`, `audio`, `camera`, `gui`, `lpv_volume`, `skeletal`, `folder`.

### node.find

`{"name": "E2E_Light"}` (exact name) and/or `{"type": "light"}`. At least one is required. Returns the matching
nodes in the same shape as `scene.tree` entries, without children. Up to 1000 matches.

### node.get

All reflected properties of a node, grouped by component.

```
-> {"jsonrpc":"2.0","id":5,"method":"node.get","params":{"id":4}}
<- {"jsonrpc":"2.0","id":5,"result":{"id":4,"name":"E2E_Light","type":"light",...,
    "properties":{"Light":{"cast_shadows":true,"color":[1.0,1.0,1.0],"enabled":true,"intensity":2.5,
                           "kind":"point","range":40.0,"source_radius":0.0},
                  "Node":{"child_count":0,"generated":false,"name":"E2E_Light","node_type":"light"},
                  "Transform":{"position":[0.0,15.0,0.0],"rotation":[0.0,-0.0,0.0],"scale":[1.0,1.0,1.0]}}}}
```

Property names are the same snake_case names the scene file and Lua use. Read-only and runtime-only properties
are included as their current value.

### node.set

```
-> {"jsonrpc":"2.0","id":6,"method":"node.set","params":{"id":4,"component":"Light","property":"intensity","value":3.5}}
<- {"jsonrpc":"2.0","id":6,"result":{"id":4,"component":"Light","property":"intensity","value":3.5}}
```

The value is converted to the property's type and range-checked by the engine's reflection registry, the same
path the inspector and scene loading use. A wrong type, an out-of-range value or a read-only property is -32602
with the reason. Vectors are JSON arrays; enum properties accept their name.

### node.create

Creates a node from scene-file node JSON under `parent` (default: scene root). Returns the node summary.

```
-> {"jsonrpc":"2.0","id":7,"method":"node.create","params":{"parent":2,"node":{"name":"Probe","type":"node","position":[1,2,3]}}}
<- {"jsonrpc":"2.0","id":7,"result":{"id":14,"name":"Probe","type":"node","folder":false,"components":["Node","Transform"],"child_count":0,"children"...}}
```

### node.delete

`{"id": 14}` removes the node and its subtree. Returns `{"deleted": 14}`. The root cannot be deleted.

### node.reparent

`{"id": 14, "parent": 4, "index": 0, "keep_world": true}`. `parent` null means the scene root. `index` defaults to
-1 (last). `keep_world` keeps the node where it is in the world. Returns `{"id", "parent"}`.

### render.screenshot

Writes the backbuffer as a PNG (UI included, the same readback as `NOX_FINAL_DUMP_DIR`) and returns its path.

```
-> {"jsonrpc":"2.0","id":8,"method":"render.screenshot","params":{"path":"captures/automation/e2e_shot.png"}}
<- {"jsonrpc":"2.0","id":8,"result":{"bytes":479180,"frame":14,"height":1080,"path":"captures/automation/e2e_shot.png","width":1920}}
```

`path` is relative to the engine's working directory (the repository root when launched by the MCP tools). The
default is `captures/automation/screenshot_<frame>.png`. `captures/` is git-ignored.

### render.settings.get

Every `render_settings` key, plus `surfel_gi_enabled`, `surfel_gi_quality_tier`, `surfel_gi_debug_view`,
`lighting_composite_debug_mode`, `post_debug_view`, `ssr_debug_view` and `quality_tier`. Pass `keys` to read a few.

```
-> {"jsonrpc":"2.0","id":9,"method":"render.settings.get","params":{"keys":["bloom","surfel_gi_debug_view"]}}
<- {"jsonrpc":"2.0","id":9,"result":{"bloom":true,"surfel_gi_debug_view":0}}
```

### render.settings.set

One setting, or several at once. Every value is checked before any is applied, so a bad key changes nothing.

```
-> {"jsonrpc":"2.0","id":10,"method":"render.settings.set","params":{"key":"bloom_strength","value":0.5}}
<- {"jsonrpc":"2.0","id":10,"result":{"applied":{"bloom_strength":0.5}}}
-> {"jsonrpc":"2.0","id":11,"method":"render.settings.set","params":{"key":"bloom_strength","value":99999.0}}
<- {"jsonrpc":"2.0","id":11,"error":{"code":-32602,"message":"value for 'bloom_strength' is outside [0.000000, 4.000000]"}}
```

`{"settings": {"bloom": false, "ssao": true}}` sets several. `quality_tier` (a name) applies the `quality_tiers`
entry of the current scene file on top of the current values.

### render.pass_timings

Per-pass timings of the last rendered frame (the same data as `NOX_LOG_PASS_METRICS`).

```
<- {"jsonrpc":"2.0","id":12,"result":{"cpu_total_ms":0.77,"frame":14,"gpu_total_ms":1.82,"passes":[
    {"cpu_ms":0.016,"cpu_wait_ms":0.0006,"dispatches":0,"draw_calls":0,"gpu_ms":0.0061,"name":"ExposureBeginPass"}, ...]}}
```

GPU times are measured with timer queries and are noisy when other work shares the GPU.

### log.tail

The last `lines` engine log lines (stdout and stderr, default 200), with level `info`, `warning` or `error`.

```
-> {"jsonrpc":"2.0","id":13,"method":"log.tail","params":{"lines":2}}
<- {"jsonrpc":"2.0","id":13,"result":{"total":290,"lines":[
    {"level":"info","text":"[InputManager] Set context 'camera' enabled: 0","time":39.774},
    {"level":"info","text":"[InputManager] Set context 'editor' enabled: 1","time":39.774}]}}
```

`total` is the number of lines so far, so a later call can tell what is new.

### lua.exec

Runs a Lua chunk and returns its first return value (strings, numbers, booleans and null; other values come back as
`"<table>"` style type names).

```
-> {"jsonrpc":"2.0","id":14,"method":"lua.exec","params":{"code":"return 1 + 2"}}
<- {"jsonrpc":"2.0","id":14,"result":3}
-> {"jsonrpc":"2.0","id":15,"method":"lua.exec","params":{"code":"error('boom')"}}
<- {"jsonrpc":"2.0","id":15,"error":{"code":-32000,"message":"=automation:1: boom\nstack traceback: ..."}}
```

The chunk runs in its own VM with the engine's usual memory and instruction limits. It opens the base, string, table,
math, utf8 and coroutine libraries (no `io` or `os`) and has no project scripts. Project worlds keep their own VM;
this command does not reach it yet.

### camera.get / camera.set

```
-> {"jsonrpc":"2.0","id":16,"method":"camera.set","params":{"lookAt":[0,0,0],"fov":50}}
<- {"jsonrpc":"2.0","id":16,"result":{"fov":50.0,"position":[-1.0,1.0,-1.0]}}
```

`camera.get` returns `position`, `front`, `up`, `rotation` (`[yaw, pitch]` in degrees), `fov`, `near`, `far`, `aspect`.
`camera.set` takes `position`, either `rotation` (`[yaw, pitch]`, degrees) or `lookAt` (a point), and `fov` / `near` /
`far` (perspective only). In game mode a game module may move the camera every frame.

### input.inject

Injects one key or mouse-button event. It goes through SDL's event queue, so it takes the same path as real input and
applies on the next frame.

```
-> {"jsonrpc":"2.0","id":17,"method":"input.inject","params":{"key":"W","state":"down"}}
<- {"jsonrpc":"2.0","id":17,"result":{"applies_at":"next frame","key":"W","state":"down"}}
-> {"jsonrpc":"2.0","id":18,"method":"input.inject","params":{"mouse":"left","state":"down","x":10,"y":10}}
```

`key` takes an SDL key name (`W`, `Space`, `Left`, ...). `mouse` is `left`, `right` or `middle`. Hold a key by
sending `down`, then `up` later. Actions (`action`) are not supported yet and return -32602.

### editor.select

Selects a node in the editor (editor mode only; otherwise -32000). `{"id": 4}` selects, `{"id": null}` clears.

## Using it from Claude Code

The repository's `.mcp.json` registers the server as `nox`, so Claude Code starts it with
`python tools/nox_mcp/server.py` when the project is open. (Claude Code asks you to approve project servers the first time.)

Tools (names as Claude Code shows them, `mcp__nox__<name>`):

- Process: `engine_build` (Release build; `configure: true` the first time), `engine_launch` (preset or
  `mode` game/editor, `scene`, `quality`, `port`, `env`, `extra_args`), `engine_wait_ready`, `engine_stop`.
- One per command: `engine_info`, `engine_quit`, `engine_pause`, `engine_resume`, `engine_step`, `scene_list`,
  `scene_load`, `scene_save`, `scene_tree`, `node_get`, `node_set`, `node_create`, `node_delete`, `node_reparent`,
  `node_find`, `render_settings_get`, `render_settings_set`, `render_pass_timings`, `log_tail`, `lua_exec`, `camera_get`,
  `camera_set`, `input_inject`, `editor_select`, `render_screenshot`.

`render_screenshot` returns the PNG as an image block, so the model can look at the result. A typical loop:

1. `engine_build`, then `engine_launch` with `scene` (or `preset`), then `engine_wait_ready`.
2. `scene_tree` to find ids, `node_get` to read, `node_set` to change, `render_screenshot` to look.
3. `engine_step` with `frames` to advance a known number of frames, `log_tail` for errors.
4. `engine_stop` when done.

`engine_launch` writes the engine's stdout and stderr to `captures/automation/engine_<port>.log`. The MCP server
logs to stderr. Only one engine is launched per server; a second `engine_launch` is refused until `engine_stop`.
If the engine was not started by the MCP server, the tools still work when it listens on the port the server uses
(47800 by default).

From a shell, `nox_client.py` does the same thing:

```
python tools/nox_mcp/nox_client.py --port 47800 scene.tree
python tools/nox_mcp/nox_client.py node.get '{"id": 4}'
```

Run the end-to-end check (needs the Release build and a desktop session) with:

```
python tools/nox_mcp/test_e2e.py
```

It launches the engine on `scenes/automation_e2e.json` and checks each command group above.

## Safety

- The server accepts connections only from the local machine, and it has no authentication. Any local process can
  change the scene and can run Lua. Do not enable it on a machine where you do not trust the other local users or
  programs, and do not run it with an untrusted scene.
- `scene.save` with a path and `render.screenshot` with a path write where asked, relative to the working directory.
  Keep them inside the repository's `captures/` or a scratch directory.
- `lua.exec` runs arbitrary Lua in its own VM. That VM has no `io` or `os`, but the base library still has
  `loadfile` and `dofile`, which can read any file the engine's user can read. It does not change the project's
  scripts or the scene's Lua, and it cannot write files.
- The server is off unless the flag or variable is set. Nothing changes when it is off: the hook returns at once.
