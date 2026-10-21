# Lua project API, version 1

Status: phase 1. Implemented in `src/scripting/` (runtime, world, project loader, game module),
headless tests in `tests/native/test_lua_*.cpp` and `tests/native/test_ecs_handles.cpp`, and a sample
project in `projects/sample-arena/` with the scene `scenes/lua_sample.json`. Lua 5.5.1 is vendored
in `external/lua-5.5.1` (see `external/README.md`).

## Project descriptor

A project is a directory with `project.json` (contract C4). The scene selects it with a top-level key:

```json
"project": "projects/sample-arena/project.json"
```

```json
{
  "format": 1,
  "name": "sample-arena",
  "runtime": "lua",
  "api_version": 1,
  "entry": "scripts/main.lua",
  "script_roots": ["scripts"],
  "data_roots": [],
  "assets": []
}
```

- `format` must be `1`, `runtime` must be `"lua"`, `api_version` must be `1`, `entry` is required.
- `entry`, `script_roots` and `data_roots` are relative to the descriptor's directory and resolve
  through `AssetIO::ResolveRelative`.
- `script_roots` are searched in order by `require`. Default: `["scripts"]`.
- `assets` is reserved (accepted, not yet used).
- `audio` (optional): `{ "buses": ["boss_music", ...] }` declares extra mixer buses beyond `master`, `music`,
  `sfx`, `ui` and `voice` (see "Audio"). Invalid or duplicate names are reported when the project loads.
- `authoring_roots` (optional) lists repo-logical files or directories that `nox.save_authoring_json` may write, e.g.
  `["projects/nox-racing/config/hud.json", "data"]`. Unlike the fields above these are not descriptor-relative, because `config/` and
  `data/` are shared roots. See "Authoring saves".

Descriptors are read through AssetIO, so the same project loads from loose files and from packs.

## Loading and modules

The entry module runs once per configuration of the scene (a fresh VM each time). Modules are
loaded with `require`:

- `require("a.b")` loads `<script_root>/a/b.lua` from the first root that has it.
- Module names: dot-separated identifiers. Anything else is rejected.
- A module's return value is cached. A failed module is not cached (a second `require` reports the
  same error). A cycle is reported as `circular require`.
- Chunk names are the logical paths (`projects/sample-arena/scripts/main.lua`), so diagnostics show
  the virtual file and line.

Registration calls (`nox.schema`, `nox.system`, `nox.behavior`, `nox.on`) are valid only while the
project loads. Calling them later is an error.

## Lifecycle

| Hook | Registered with | When |
| --- | --- | --- |
| Load | top-level code of the entry and its `require`d modules | Each scene configuration (Configure) |
| Frame system | `nox.system{ name, order, update = function(ctx, dt) end }` | Each frame while playing and not paused, in `order` then declaration order |
| Fixed system | `nox.system{ ..., fixed = true, update = ... }` | Each physics substep (see below) |
| Behavior | `nox.behavior{ name, update = function(ctx, entity, dt) end }` | After the systems, for each entity with `ctx:attach` |
| Event handler | `nox.on(name, function(ctx, payload) end)` | After systems, in emission order then handler registration order |
| `begin_play` / `end_play` | `nox.on("begin_play", ...)` | At Play and at Stop |

`Update` runs systems in this order: frame systems, events emitted during the frame, behaviors.
Events emitted by an event handler are delivered in the next frame. Systems read a snapshot of the
world as it was when the system started; commands are applied when the callback returns.

## Game module behaviour

- Core calls `Update` every frame. The module runs nothing until Play, and nothing while paused.
- `ctx:input_enabled()` reports the host's input focus. Systems decide what input does; the host does
  not block them.
- Play, Pause and Stop map to `BeginPlay`, `SetPaused` and `EndPlay`.
- Camera: the module keeps its own camera copied from the engine view, as the native game module does.

### Fixed systems and physics substeps

`fixed = true` systems run from `PhysicsEngine::SetSubstepCallback`, once for each fixed physics
step, in the same order as frame systems. The module registers the callback while playing and clears
it at Stop and when the module is destroyed.

- A frame with no full step runs no fixed system (zero substeps).
- A frame with several steps runs the fixed systems once per step with contiguous indices from 0
  (`ctx:substep()`).
- `PhysicsEngine::SingleStep` runs one step with index 0.
- Pause suppresses fixed systems too (the module does not forward the callback while paused).

Physics ownership of entities still applies. Kinematic bodies are pushed from the ECS transform
before stepping (`PreStepSync`). Dynamic bodies are pulled back after stepping (`PostStepSync`), so a
script write to a dynamic body's transform is overwritten by the physics result. Use fixed systems
to drive kinematic or non-physics transforms, not to fight dynamic bodies.

## Entity handles

A handle is an opaque userdata (`nox.entity`) with the native `EntityHandle { world, id, incarnation }`.

- `world` is a process-wide token issued per ECS world (new on every scene load and on `Clear`).
- `incarnation` changes every time the entity ID is destroyed. Retired IDs are never reused.
- A handle is valid only while the world, the ID and the incarnation all still match.
- Every operation that takes a handle checks validity. Stale handles raise
  `stale or invalid entity handle`, except `ctx:is_valid` and `ctx:same`, which never raise.
- Use `ctx:key(h)` for table keys. Userdata compares by identity in Lua tables; `==` between handles
  compares their identity fields.

## Context (`ctx`)

Every callback receives `ctx` as its first argument. Methods use `ctx:name(...)`.

Reads (valid at any time in a callback):

| Function | Returns |
| --- | --- |
| `ctx:find(name)` | handle of the entity whose node name is `name`, or nil. A name with `/` is a path of node names from the scene root: `ctx:find("Props/Deep/Lamp")`. Folders count as path segments, and each segment takes the first child with that name. A bare name ignores folders: it finds the node in any folder (or at the top). A path never falls back to a bare-name lookup, so `ctx:find("Lamp/Props")` is nil |
| `ctx:name(h)` | node name |
| `ctx:is_valid(h)` | boolean, never raises |
| `ctx:same(a, b)` | boolean, never raises |
| `ctx:key(h)` | string usable as a table key |
| `ctx:entities()` | array of all live entities with a transform, ascending ID |
| `ctx:query(schema)` | array of live entities that have the declared schema, ascending ID (snapshot) |
| `ctx:has_component(h, name)` | presence of any registered component (see Components) or native pool: Transform, Renderable, Light, Camera, Animation, Physics, Collider, Audio, LPVVolume. Unknown names raise |
| `ctx:component(h, name)` | component proxy (see Components), or nil when the entity has no such component |
| `ctx:components(h)` | array of component names present on the entity |
| `ctx:parent(h)` / `ctx:children(h)` | parent handle (nil at the top) / array of child handles, for nodes with entities |
| `ctx:raycast(origin, dir, max_dist)` | `{ entity, name, distance, point = {x,y,z}, normal = {x,y,z} }` for the nearest scene node hit, or nil. Reads only |
| `ctx:position(h)` | world position `x, y, z` (copies) |
| `ctx:scale(h)` | world scale `x, y, z` (copies) |
| `ctx:frame()`, `ctx:time()` | frame counter (from Play), play time in seconds |
| `ctx:substep()` | substep index inside a fixed system, 0 otherwise |
| `ctx:input_enabled()` | boolean |
| `ctx:resolve_spawn(token)` | handle created by `ctx:spawn` in an earlier callback, or nil |

Declared data (immediate; Lua-owned, not rolled back):

| Function | Behaviour |
| --- | --- |
| `ctx:has(h, schema)` | boolean |
| `ctx:get(h, schema)` | table copy of all fields, or nil when the entity has no such schema |
| `ctx:get_field(h, schema, field)` | value copy, or nil when missing |
| `ctx:add(h, schema [, values])` | attach the schema with defaults overridden by `values`. Fields with no default must be given. Replaces existing data |
| `ctx:set_field(h, schema, field, value)` | type-checked write. Errors when the entity does not have the schema |
| `ctx:remove(h, schema)` | detach the schema |
| `ctx:attach(h, behavior)` / `ctx:detach(h, behavior)` | behavior membership |

Deferred native commands (applied after the callback returns successfully):

| Function | Behaviour |
| --- | --- |
| `ctx:spawn(name)` | queue creation of a script entity named `name` (must be unused). Returns a token; use `ctx:resolve_spawn(token)` in a later callback |
| `ctx:destroy(h)` | queue destruction. Script-spawned entities are destroyed; scene nodes (authored, or instantiated by `ctx:instantiate`) are removed from the scene at the frame boundary. Authored nodes come back at Stop, nodes created during play do not |
| `ctx:move(h, x, y, z)` | set local position (continuous motion; does not flag a camera cut) |
| `ctx:teleport(h, x, y, z)` | set local position and flag a discontinuous move (`ConsumeTeleport` returns true for that frame, so temporal history resets) |
| `ctx:set_scale(h, x, y, z)` | set scale |
| `ctx:emit(name [, payload])` | queue an event; delivered after the systems |
| `ctx:log(...)` | write to the engine log (`[Lua]` prefix) |

Command targets are validated again when applied. A stale target produces a warning diagnostic and
does not stop other commands. Spawn names that are already in use produce a warning.

Native transform writes preserve hierarchy, transform dirtying and the transform system's
bookkeeping: they call the existing `TransformSystem` setters.

## Components (reflection registry)

Scripts reach the node components that have a registry descriptor (`src/reflect/`). The layer is generic: a
descriptor registered by any subsystem appears here without changes to the scripting code.

- `ctx:component(h, name)` returns a proxy, or nil when the entity has no such component. Names are the descriptor
  names (`nox.components()`); the native pool names are presence-only (`ctx:has_component`).
- Field read `proxy.intensity`, field write `proxy.intensity = 2.0`, both through the descriptor. Vectors are
  `{x=, y=, z=, w=}`, colours `{r=, g=, b=, a=}`, and the array form `{1, 2, 3}` is accepted on input. Enums read and
  write their names; integer indices are accepted on input. Method call `proxy:play("Run")` checks argument count and
  types and returns the descriptor's result.
- A proxy holds the checked entity handle, not the node: every access re-checks the handle and finds the node again.
  A stale handle raises `stale or invalid entity handle`; a component removed from the entity raises
  `component 'X' is not present on entity ...`.
- Errors are Lua errors: unknown property or method, read-only property, property that does not apply to the node, wrong
  value type, a value the descriptor rejects, and a method that fails.
- Writes (field writes, method calls, `ctx:add_component`, `ctx:remove_component`, `ctx:load_scene`) are allowed only in
  systems, behaviors and event handlers. They take effect immediately (not after the callback returns) and a failing
  callback does not roll them back. Reads are allowed wherever `ctx` is.
- `ctx:add_component(h, name)` and `ctx:remove_component(h, name)` return `true` when the component is present (add) or
  absent (remove) afterwards. Otherwise they return `false, reason`, for example for a builtin `Node`.
- `nox.components()` returns the registry in inspector order: each entry has `name`, `category`, `tooltip`, `builtin`,
  `order`, `can_add`, `can_remove`, `properties` and `methods`. A property has `name`, `label`, `type`, `read_only`,
  `hidden`, `no_save`, `angle`, `conditional`, and when set `min`, `max`, `step`, `enum`, `asset_filter`. A method has
  `name`, `tooltip`, `args` (type names), `returns`, `editor_button`. Types: `bool`, `int`, `float`, `vec2`, `vec3`,
  `vec4`, `color3`, `color4`, `string`, `enum`, `asset_path`, `node_ref`.
- Entities spawned by scripts have no scene node, so they have no registry components.

Scene instances:

- `ctx:instantiate(path [, opts])` creates a prefab instance (`path` is a logical `*.prefab.json` asset, see
  `docs/prefabs.md`) and returns its handle. The instance is a normal scene node: it has the node's registry
  components, its children and its prefab link, like a node loaded from the scene file. It is created immediately, so
  the handle can be used in the same callback, and it is removed at Stop.
- `opts` (all optional): `parent` (handle; default the scene root), `position`, `rotation` (degrees, Euler), `scale`
  (each `{x=, y=, z=}` or `{x, y, z}`), `name` (string). Other keys are ignored.
- Only writable callbacks (systems, behaviors, events) may instantiate. A missing or invalid path, a file that is not a
  prefab, or a prefab that fails to expand raises a Lua error.
- `ctx:destroy(h)` removes an instance or any other scene node with its subtree. The removal is queued like the other
  commands and applied at the frame boundary.

Scene requests:

- `ctx:load_scene("scenes/<name>.json")` switches scene at the next frame boundary. The path is logical: under `scenes/`,
  ends in `.json`, uses letters, digits, `_ - . /` and no `..`.
- There is no engine time scale yet, so there is no `ctx:time_scale`.

## Physics

The queries and events use the engine's PhysicsEngine, the world that the RigidBody, Collider, Trigger and
CharacterController components act on (docs/physics.md). Without a physics engine the queries return nil or an empty
array and no physics events are raised. Layers are bit masks: a body is reported when its collision layer intersects
the mask; `mask` is optional and defaults to every layer.

Queries (reads, valid in any callback):

| Call | Result |
|---|---|
| `ctx:physics_raycast(origin, dir, max_dist, mask)` | `{ entity, distance, point = {x,y,z}, normal = {x,y,z} }` for the nearest body hit within `max_dist`, or nil. `dir` need not be normalized. `entity` is absent when the body has no scene node |
| `ctx:overlap_sphere(center, radius, mask)` | array of entity handles (ascending ID) of the bodies the sphere overlaps, touching included; empty when none |
| `ctx:overlap_box(center, half_extents, mask)` | the same for a world-aligned box |

Events are delivered to `nox.on` handlers as `function(ctx, payload)`. They are queued during the physics step and
delivered by the next Lua update, in the same order as the other events of that update.

| Event | Payload |
|---|---|
| `trigger_enter`, `trigger_exit` | `{ trigger = handle, other = handle or nil }`: a body entered or left a Trigger volume. `other` is nil when the body's node has no entity |
| `contact_begin`, `contact_end` | `{ a = handle or nil, b = handle or nil }`: two bodies started or stopped touching. A side without an entity is nil; pairs with no entity on either side are not reported |

```lua
nox.on("trigger_enter", function(ctx, ev)
  if ev.trigger == ctx:find("pickup_zone") and ev.other == ctx:find("player") then
    ctx:emit("picked_up", nil)
  end
end)
```

Components (`ctx:component(h, "CharacterController"):move(...)`, `ctx:component(h, "Trigger").radius`, and the others) go
through the registry like every other component (see Components). Their fields and methods are listed in docs/physics.md.

### Settings (`nox.physics`)

`nox.physics` reads and changes the running physics world. Keys are the snake_case names of the `physics` object in
`config/config.json` (`fixed_delta_time`, `max_substeps`, `gravity`, `velocity_iterations`, `default_friction`, ...),
plus `layer_matrix` and `layer_names`. The Physics window in the editor edits the same values (see
[editor.md](editor.md#physics)).

| Function | Result |
| --- | --- |
| `nox.physics.settings()` | table of every runtime value |
| `nox.physics.get(key)` | one runtime value; raises on an unknown key |
| `nox.physics.set(key, value)` | changes one runtime value; raises on an unknown key or a rejected value. Numbers are clamped to the engine's ranges |
| `nox.physics.reset()` | runtime values back to the editor's authored values |
| `nox.physics.set_collision(a, b, enabled)` | allows or blocks layers `a` and `b` (0 to 31) in both directions |
| `nox.physics.collides(a, b)` | boolean: whether layers `a` and `b` may collide under the layer matrix |

Script changes are runtime only: they are never written into a saved scene, and Stop restores the authored values.

## Declared schemas

Projects declare component schemas in Lua. No native component type or pool is involved.

```lua
nox.schema("Health", { current = "integer", maximum = "integer" }, { current = 100, maximum = 100 })
```

- Field types: `number` (integer or float; non-finite rejected), `integer`, `boolean`, `string`.
- Defaults are optional. Types are checked on every write, including authored data.
- Schema names and field names are identifiers. Duplicate schema names are rejected.
- Values are stored per checked entity identity (the storage is outside the native ECS). Reads
  return copies.
- Records are removed when their entity is destroyed.

## Authored data (scene)

Scene key `scripting.entities` maps node names to schema values. Node names are the stable authored
references, so they must be unique among scene nodes (the first match is used).

```json
"scripting": {
  "entities": {
    "Player": {
      "Health": { "current": 3, "maximum": 3 },
      "Quest": { "state": "open" }
    }
  }
}
```

Unknown nodes, unknown schemas, unknown fields and wrong types are reported as diagnostics; the
rest of the data still loads. Stop restores the authored values.

## Events

`nox.on(name, handler)` registers a handler; `ctx:emit(name, payload)` queues an event. The payload is
any Lua value (kept alive in the registry until delivery). Delivery order is the emission order, and
for each event the handler registration order. Handlers that fail are disabled (see Errors).

## Errors and diagnostics

Every entry into Lua is protected. A failing callback:

- is reported as a diagnostic (`Diagnostic`: severity, source such as `system 'health'`, file, line,
  message, traceback, frame);
- is disabled (system, behavior or handler) until `RetryFaulted()` (editor retry, phase 2) or a reload;
- has its queued native commands discarded. Lua table changes it already made are not rolled back.

Error objects that are not strings are reported as `(error object is a <type> value)`.

Load failures (syntax, runtime error, missing entry) leave the module unavailable. Core ignores it, and
the diagnostics explain why.

## Limits and sandbox

Defaults (`RuntimeLimits`): 32 MiB of Lua memory per VM, 2,000,000 VM instructions per callback
(checked every 1,000 instructions, coroutines included). Limits are checked per call; host functions
are not preemptible, so exposed engine operations are bounded by design.

Libraries: `base` (without `dofile`, `loadfile`), `string` (without `string.dump`), `table`, `math`,
`utf8`, `coroutine`. Absent: `io`, `os`, `debug`, `package` (no `loadlib`, `cpath`, `path`).

- `load` accepts text chunks only, from string input, with an explicit environment table.
- `require` loads only from project script roots through AssetIO. There is no native module loading.
- `print` writes to the engine log.

## Native runtime

- Lua 5.5.1 is compiled as C++ (the `nox_lua` target). Errors unwind with C++ exceptions, so destructors
  run between `lua_error` and its protected call. Bindings may hold RAII objects. The engine includes
  `lua.h` directly (not `lua.hpp`, which adds `extern "C"`).
- Upstream bug list for 5.5.1 (www.lua.org/bugs.html): one entry (`luaO_applyparam` negative shift). Not
  reachable from NOX (the only call site passes a constant). No source patch applied.
- Allocation failure surfaces as a reported `LUA_ERRMEM` error (`memoryExceeded`), not a crash.
- The VM runs only on the thread that owns the game module (main thread in the current engine).

## Schema versions and migrations

`nox.schema(name, fields, defaults, version)` takes an optional integer `version` (default 1). Stored
records keep the version they were written with. When the version changes, or a field's type changes,
or a field disappears, the stored records of that schema need `nox.migrate`:

```lua
nox.migrate("Health", function(fields, fromVersion)
    return { current = fields.current, shield = 5 }
end)
```

The migration runs in the candidate VM with the stored fields as a table. It must return a table; every
field of the new schema must be present (or have a default). A migration error, a non-table result or a
wrong field type rejects the reload.

## Reload (transactional)

`LuaGameModule::RequestReload()` (the editor button, or the loose-file watcher) is applied at the next
frame boundary, before the frame's systems run:

1. The running world takes a bounded snapshot: declared records of live entities and of the authored
   state, behavior attachments, spawned entities, original transforms of moved entities, the frame
   counter, play time and the playing flag. Values are typed (integers stay integers, finite floats stay
   floats). The snapshot holds no Lua objects, so functions, userdata, metatables and cycles cannot be
   stored; the limit is 100,000 values.
2. A candidate world is built in a fresh VM from one consistent module set. Every file is read through
   the project's file access (AssetIO in the engine), its size and modification time are checked before
   and after the read, and the set is checked again after loading. A change during staging discards the
   candidate and stages again (three attempts).
3. While staging, host side effects are unavailable: no `ctx`, no world writes, no spawns; `print` output
   is discarded and reported as a warning. Registration (`nox.schema`, `nox.system`, `nox.on`,
   `nox.migrate`) only affects the candidate.
4. The snapshot is imported into the candidate: records are migrated and validated against the new
   schemas, entities that no longer exist are dropped with a warning, and stored data for a schema that
   the new code no longer declares rejects the reload.
5. On success the candidate replaces the world at the frame boundary. The old world releases its
   registry references, then its VM closes. No initializer, `begin_play` or `nox.on` setup runs again; the
   candidate continues from the imported state, the frame counter and the play state. Faults of the old
   world are cleared, because the new code is the retry.
6. On failure the candidate is closed. The old code, declared state, world and physics callback are
   unchanged, and the reason is recorded in the reload log (`reload rejected, previous code and state kept`).

What is not migrated: local variables, upvalues, closures, coroutine stacks, open resources and function
identities. Recreate timers and tasks from declared data after a reload. A reload while stopped rebuilds the
world from the authored state.

Camera state is not declared data in API v1: the module camera is a copy of the engine view and is not
migrated.

## Editor panel and watcher

The panel "Lua project" (`lua.project`, registered through `EditorExtensions::RegisterPanel`) shows the
project, its state, the reload counters, the faulted systems and behaviors, every diagnostic with its
virtual file, line, source and traceback, and the reload log. Buttons: Reload (next frame boundary) and
Retry faulted (re-enables faulted systems, behaviors and handlers).

The loose-file watcher polls the module set every 0.25 s and requests a reload once a changed set has been
stable for 0.4 s. It runs only in Loose mode and only when the bootstrap enables reloads (`reloadEnabled`,
off in packed distributions by default). Shipped packs are read-only and never reload.

## Editor support (IDE annotations)

The editor writes two files into the project folder when it loads the project and on **File > Generate Lua Editor
Support**: `.nox/types/nox.lua` (LuaLS annotations, git-ignored) and `.luarc.json` (Lua 5.5 runtime and the library
path). The API annotations are hand-written; each `nox.comp.<Name>` class and each `ctx:component` overload is
generated from the reflection registry (`src/reflect/`). The files, the merge rules and the launch options are in
[editor.md](editor.md#external-script-editor).

## Editor

Project editor code runs in the engine editor (immediate mode ImGui). Two registrations, both in the script:

- `nox.editor_panel{ id = "race", title = "Race & Routes", draw = function(ctx, ed) ... end }` is a dockable panel.
  It is listed and shown only while this project is the active module (`EditorPanel::available`). Declaring an id
  again replaces the panel; at most 16 panels per project.
- `nox.editor_viewport(function(ctx, ed) ... end)` draws on the scene viewport every frame (handles, lines, labels).

A panel that raises an error faults until the next reload (diagnostics show the message). Editor code cannot change
the world: systems, behaviors and draw handlers are not run by it, and commands queued from it are discarded.

### Widgets (`ed`)

Every widget is immediate: it returns the value to keep (`local v = ed.slider("Grip", v, 0, 1)`).

| Call | Returns | Notes |
| --- | --- | --- |
| `ed.text(s)`, `ed.separator()`, `ed.same_line()` | | |
| `ed.tooltip(s)` | | shows `s` while the previous widget is hovered |
| `ed.button(label)` | pressed | |
| `ed.checkbox(label, b)` | b | |
| `ed.selectable(label, selected)` | clicked | list rows (checkpoints, events) |
| `ed.input_float(label, v)`, `ed.input_int(label, n)` | value | |
| `ed.input_text(label, s)` | s | at most 511 bytes are editable |
| `ed.slider(label, v, min, max)` | v | |
| `ed.drag_float(label, v, speed, min, max)` | v | `speed` default 0.1; `min = max = 0` means unbounded |
| `ed.combo(label, index, items)` | index (1-based) | items are strings |
| `ed.tree(label, fn)` | open | `fn(ed)` draws the children while open (nesting up to 8) |
| `ed.disabled(off, fn)` | | greys out the widgets drawn by `fn(ed)` when `off` is true |
| `ed.columns(n, fn)` | | lays out `fn(ed)` in `n` columns (1..16); call `ed.next_column()` between cells |

### Play state (`ed.playing`, `ed.request_play`, `ed.request_stop`)

- `ed.playing()` returns true while the editor is playing the scene.
- `ed.request_play()` and `ed.request_stop()` do what the Play and Stop buttons do; each is a no-op when the editor is
  already in that state.

### Undo (`ed.undo`)

`ed.undo(label, undo_fn, redo_fn)` adds one step to the editor's undo stack (Ctrl+Z and Ctrl+Y cover it). The two
functions run in this project's VM when the step is undone or redone; they replace the authored table the panel
edits. A step belongs to the world that created it:

- a reload, a Reconfigure (any authored-data change that rebuilds the module) or a scene load rebuilds the world, and
  the steps of the old world then do nothing (no error, no diagnostic);
- an error inside a step is a diagnostic (`editor undo (label)`) and does not fault the world;
- at most 4096 steps per world (`ed.undo` raises an error after that).

### Viewport (`ed.handle`, `ed.line3`, `ed.text3`)

Only inside `nox.editor_viewport`; elsewhere these raise `<name> is only valid inside nox.editor_viewport`.

- `ed.handle(id, x, y, z [, { radius = 7, color = 0xAARRGGBB, label = "Move point" }])` returns
  `dragging, x, y, z, clicked`. The engine owns the point: a drag moves it on the ground plane, and releasing the
  button records one undo step named `label`. `clicked` is true for one frame after a press (the overlay reports
  presses after it has drawn the handles), so a click selects an event or checkpoint on the next frame.
- `ed.handle_position(id)` returns `x, y, z` or nil; `ed.handle_set(id, x, y, z)` moves a point from the script (not
  while it is being dragged). At most 256 handles per project.
- `ed.line3(a, b, color, thickness)`: `a` and `b` are `{x, y, z}` or `{x = , y = , z = }`. A segment with an endpoint
  behind the camera is dropped (no clipping). Thickness 0.5..16, default 1.
- `ed.text3(x, y, z, text, color)`: a label at a world point (a point behind the camera is skipped).

Colours are `0xAARRGGBB` everywhere (default white).

### Arrays in saved data

Empty Lua tables save as objects (`{}`). A table that is an array has to be marked: `nox.array(t)` marks `t`
(`nox.array()` returns a new empty array). Arrays read from JSON (`nox.game()`, `nox.json`, `nox.scene()`) are marked
already, so `nox.game()` -> `nox.on_save_game` keeps `[]` as `[]`. A copy made in Lua does not keep the mark: mark the
copy with `nox.array(copy)` when it is an array. A marked table must have keys `1..n` only.

### Panel availability

The native racing panels (Race & Routes, Drive, HUD, Night Street) and their viewport overlay show only while the
native racing module is the active module. Lua panels show only while their project is active. The engine's Rendering
window has a "Look" tab with the weather, reflection, fog, grade, grain, motion blur, physical camera and clustering
controls (live, not saved), which is the same set the Night Street panel edits.

## Play-world restoration

Play captures, for the whole authored scene, every node's own keys (including its registry `components` block) and
prefab link, every live entity's local transform and each rigid body's pose and velocities. Stop (after the module
ended play) restores the scene in this order, then the scene is configured again:

1. Nodes created during play (`ctx:instantiate`, native code) leave the scene with their subtree, bodies and entities.
2. Script-spawned entities (`ctx:spawn`) are destroyed.
3. Authored transforms and body states go back to the captured values.
4. Authored nodes destroyed during play (`ctx:destroy` on an authored node) are re-created from their captured JSON
   under their captured parent and sibling index.
5. Every authored node still in the scene gets the registry property values that differ from the capture written
   back (only the differing properties are set).

Authored declared data is restored by the module at Stop as well.

### Components restored at Stop

- Restored: every authored node's registry component values (`Light` intensity, colour, enabled, `Node.name`, physics
  properties, and so on) as they were at Play, and every entity's local transform, so transform writes made through
  proxies are undone.
- Not restored: a component added during play is not removed at Stop. A component removed during play comes back with
  its captured properties. Nodes inside a re-created prefab instance keep their content, but not their own prefab links.

## Known limits (API v1)

- Component writes go through the registry (see Components). Native subsystems without a descriptor (renderables,
  physics bodies, colliders, audio) stay presence-only.
- Spawned entities are ECS-only (no scene node, no renderable); they are destroyed at Stop.
- Scene nodes created by `ctx:instantiate` are removed at Stop and are not re-created; authored nodes that a script
  destroyed are re-created at Stop.
- Coroutines are allowed, but a coroutine that is suspended across callbacks keeps its state until the
  VM is replaced.

## Project data, hooks and authoring

Implemented in `src/scripting/LuaData.cpp`. Everything here is generic: the project declares what it may read and
write, and the engine enforces the declarations.

### Read-only JSON

`nox.json(logical)` returns a decoded JSON value (a fresh copy) or `nil, reason`. It reads through AssetIO, so loose
and packed projects behave the same. Paths are logical: they are normalized, `..` may not leave the root and absolute
paths are refused. The project can read:

- its own directory (the one holding `project.json`), its script roots, and the files it declares in `roads`;
- `config/`, `data/` and `scenes/`.

Anything else returns `nil, "path is not readable by this project ..."`. Files over 1 MiB, missing files and invalid
JSON return `nil, reason`. Decoded values are bounded to 32 levels and 200000 values.

### Descriptor and scene copies

- `nox.project()` returns a copy of `project.json`.
- `nox.game()` returns a copy of the scene's `game` object, or `nil` when the scene has none. Changing the copy does
  not change the scene.
- `nox.scene()` returns a copy of the whole scene document.
- `nox.roads.from_table(tbl)` builds a road network from an inline table with the schema of `game.road_network`
  (nodes with `pos`, edges with `a`, `b`, `name`, ...). A `file` key is refused: declare files in `roads`. Each call
  uses one of the 8 road slots.

### Hooks

- `nox.on_configure(function(game, scene) ... end)` runs once for every load of the project world: the scene load,
  Reconfigure, and each reload candidate, after its declarations are in place. `game` and `scene` are copies. An error
  fails the load, or the reload (the running code is kept). Register it once per project.
- `nox.on_save_game(function() return value end)` supplies the scene's `game` object for `SaveGameJson` (the editor's
  save). The value follows the save-data rules. If the hook fails, the scene's object is kept and a diagnostic is
  raised. Register it once per project.

### Authoring saves

`nox.save_authoring_json(logical, value)` returns `true` or `false, reason`. It writes through
`AssetIO::SourcePath` (the loose source tree only) as pretty-printed JSON (2-space indentation, final newline), to a
temporary file that is renamed over the target. A write is refused unless all of these hold:

- the distribution is loose; packed distributions return `false, "authoring writes are disabled in packed distributions"`;
- the call runs inside an editor panel (`nox.editor_panel`) or the viewport callback. Systems, behaviors, draw handlers
  and load code cannot write, playing or not. A panel may write while the game plays, because the panel asks for it;
- the world is not a reload candidate that is still staging;
- the path is under one of `authoring_roots` and ends with `.json`.

`generated/` can never be an authoring root. Values follow the save-data rules (no functions, NaN or mixed tables).

### Logging

`nox.log_raw(line)` writes the line exactly as given to the engine log (no `[Lua] ` prefix), for output that other
tools parse. `nox.log` and `ctx:log` keep the prefix.

### Environment (test and diagnostic hooks)

`ctx:env(name)` returns the value of an environment variable, or `nil`. Only names that start with `NOX_`, use
`A-Z`, `0-9` and `_`, and have at most 64 characters are readable. Other names return
`nil, "only NOX_ variables can be read"`; a malformed `NOX_` name raises an error. This is a test and diagnostic
facility (for example `NOX_DRIVE_AUTOPILOT`), not configuration. Shipped project behaviour must not depend on it.

### Window and exit

- `ctx:overlay_rect()` returns `x, y, w, h` of the game overlay in window coordinates, as of the last drawn frame
  (zero before the first draw).
- `ctx:request_exit()` asks the host to close the application. The module reports it from its next `Update`; the host
  decides what that means in the editor.

## Project resources: fonts, icons, input

`project.json` declares resources that the engine loads once, in `InitializeResources` (before the first frame
for the startup scene; a later project switch rebuilds the ImGui font texture automatically):

```json
"fonts": [ { "id": "label", "path": "fonts/BarlowCondensed-SemiBold.ttf", "size": 18 } ],
"icons": { "manifest": "textures/ui_icons/manifest.json", "size": 128 },
"input": "input.json"
```

- `fonts`: ids are unique per project, `size` is 4..256 px (the font is baked at that size). Paths are relative to
  the descriptor and read through AssetIO, so packed projects work. A missing or invalid font reports a warning
  and falls back to the ImGui default font under the same id.
- `icons`: the UiIconAtlas manifest and atlas size (16..512). One icon atlas per engine process (the atlas is a
  singleton); `ui.image` names icons by their manifest ids (for example `hud/speedometer`).
- `input`: the action map file (default `input.json` beside the descriptor; optional, a missing file means no actions).
- Load problems appear in the Lua project panel as `resources` warnings.

## UI (draw handlers)

```lua
nox.on_draw(function(ctx, ui, dt)
  ui.rect_filled(24, 300, 200, 60, { r = 10, g = 12, b = 16, a = 190 }, 8)   -- x, y, w, h, color, rounding
  ui.text("label", "SPEED", 36, 310, 18, 0xFFB8C4D0)                         -- font id, text, x, y, size, color
end)
```

`nox.on_draw(fn)` registers a draw handler. Handlers run once per frame after the systems, on the GUI thread, with
`(ctx, ui, dt)`. They may read the world through `ctx`; world writes they queue are discarded with a warning. A
handler that raises an error is reported as `draw handler #n` and is skipped until reload or retry, like systems.
Draw handlers run also while paused and while the game is not playing (the module's overlay is drawn in the editor
and standalone), so menus can draw in any state.

Coordinates are overlay-relative (the top-left of the game view is 0, 0). `ui` functions only work inside draw
handlers (the table is the same object for every call; a kept reference fails outside a draw).

Colors: a packed `0xAARRGGBB` integer, or a table `{ r, g, b, a }` of integers 0..255 (`a` defaults to 255).

| Function | Arguments | Notes |
| --- | --- | --- |
| `ui.rect(x, y, w, h, color [, thickness, rounding])` | outline rectangle | `w`/`h` may be negative |
| `ui.rect_filled(x, y, w, h, color [, rounding])` | filled rectangle | rounded corners |
| `ui.line(x1, y1, x2, y2, color [, thickness])` | | |
| `ui.polyline(points, color [, thickness, closed])` | open or closed polyline | 2..512 points |
| `ui.polygon_filled(points, color)` | simple polygon (convex or concave) | 3..512 points |
| `ui.circle(cx, cy, r, color [, thickness])` / `ui.circle_filled(cx, cy, r, color)` | | `r > 0` |
| `ui.arc(cx, cy, r, a0, a1, color [, thickness])` | angles in radians | sampled as an open polyline |
| `ui.text(font, text, x, y, size, color [, halign, valign])` | `font` = project font id or `""` for the default; `halign` = `left`/`center`/`right`; `valign` = `top`/`middle`/`bottom` | text up to 512 bytes |
| `ui.text_size(font, text, size)` | returns `width, height` | measure before drawing |
| `ui.image(icon, x, y, w, h [, tint])` | icon id from the project icon atlas | draws nothing when the id is unknown |
| `ui.clip_push(x, y, w, h)` / `ui.clip_pop()` | clip rectangle stack | at most 16 deep; unbalanced clips are closed at the end of the frame |
| `ui.viewport()` | returns `width, height` of the overlay | |
| `ui.pointer()` | returns `x, y` of the pointer relative to the overlay | window pointer, see Input |

Bounds per frame: 4096 commands and 32768 points. Extra commands are dropped and reported once (a warning
`ui command budget exceeded`) until the frame is back under the budget. Bad arguments raise an error that
names the function, which faults the handler.

### Images and icons

- `ui.texture(logical, x, y, w, h [, tint, uv])` draws an image file and returns `true`, or `false, reason` when the
  image is missing, not readable (the project's read policy plus `textures/`, `images/` and `fonts/`), over 16 MiB,
  wider or taller than 4096 pixels, or over budget (16 images and 64 MiB of decoded pixels per module). `uv` is
  `{u0, v0, u1, v1}` and selects part of the image. Images are decoded once and released with the module.
- `ui.icon_exists(id)` is true when the project's icon atlas has the id. Use it to choose a vector fallback.
- `ui.clip_circle` is not provided: ImGui clips to rectangles only, and a circular clip needs stencil support in the RHI.

## UI widgets and menus

Widgets are calls inside `nox.on_draw` handlers, next to the drawing functions above. Each returns its result for the
frame it is drawn in. A widget registers a pointer hit region and a keyboard/pad navigation entry, so one call serves
the mouse, the keyboard and the pad. Nothing is hand-tested.

```lua
nox.on_draw(function(ctx, ui, dt)
  ui.menu()                                          -- this draw is a menu: arrows, Enter and pad navigate it
  local col = ui.column(x, y, 300, 12)               -- vertical stack; col:next(h) -> {x, y, w, h} and advances
  if ui.button("start", col:next(54), "Start") then startGame() end
  volume = ui.slider("volume", col:next(54), volume, 0, 100, { step = 5 })
  hudOn = ui.toggle("hud", col:next(54), "Show HUD", hudOn)
end)
```

| Function | Returns | Notes |
| --- | --- | --- |
| `ui.menu()` | | marks this draw as a menu; keyboard and pad navigation acts only while the last draw called it |
| `ui.button(id, rect, label [, style])` | `clicked` | clicked by a press and release on it, or by Enter / Space / pad A while focused. `style.enabled = false` dims it and removes it from navigation |
| `ui.toggle(id, rect, label, value [, style])` | `value, changed` | a box and a label; click, accept, or left/right sets it |
| `ui.slider(id, rect, value, min, max [, opts])` | `value, changed` | drag with the mouse; left/right step it. `opts`: `step` (default range / 20), `decimals` (0), `show_value` (true), `enabled`, and style keys |
| `ui.label(rect, text [, opts])` | | `opts`: `font`, `size`, `color`, `align` (`left`/`center`/`right`), `valign` (`top`/`middle`/`bottom`, default middle) |
| `ui.panel(rect [, style])` | | rounded background: `bg`, `border`, `rounding` |
| `ui.measure_text(text, font, size)` | `width, height` | `font` = project font id or `""` |
| `ui.column(x, y, w, spacing)` | a layout object | `col:next(h)` returns the next rectangle of height `h` |
| `ui.consumed()` | bool | see Focus and consumption |
| `ui.back_pressed()` | bool | Escape or pad B this frame |

A rectangle is `{x = , y = , w = , h = }` or `{x, y, w, h}`, overlay-relative like the drawing functions. Widget ids are
strings (1..128 bytes) and must stay the same across frames: focus follows the id. The style table takes colours in
`0xAARRGGBB` (`bg`, `hover`, `active`, `text`, `text_disabled`, `focus`, `accent`, `track`, `knob`, `disabled`,
`border`), `rounding` and `size` in pixels, and `font` (a project font id). Unknown keys are ignored. Up to 256
navigable widgets are kept per frame; the rest are drawn but cannot take focus.

Every frame the engine reads the keys and the pad before gameplay runs, so a menu answers while gameplay is stopped
(the draw handlers run in every state). A paused game is paused by its scripts: a system that must stop checks its own
state.

### Focus and navigation

- A menu starts with its first enabled widget focused. One widget has focus at a time.
- Arrow keys, the d-pad and the left stick move focus to the nearest widget in that direction. A focused slider or
  toggle takes left and right to change its value instead.
- Enter, Space and pad A activate the focused widget (`ui.button` returns true, a toggle flips).
- Escape and pad B are "back": `ctx:ui_back_pressed()` is true that frame and the `ui.back` event is queued for the next
  update.
- Mouse hover moves focus while the mouse moves; a press focuses the widget it landed on.
- Keys are not repeated: one press is one step.

### Focus and consumption

`ctx:ui_consumed()` (also `ui.consumed()`) is true for the frame when the menu used a navigation or accept key, or a
press landed on a widget (HUD buttons included). Gameplay input handlers should skip their action then. Consumption
does not change the action map: `ctx:action_pressed` still reports the key. `ctx:over_ui()` stays the pointer-only test.

### Events

| Event | Payload | When |
| --- | --- | --- |
| `ui.back` | nil | Escape or pad B pressed while the module runs its update |
| `ui.click` | `{ id = , action = }` | a GUI node element with `interactive` was clicked or activated (see below) |
| `ui.hover` | `{ id = , entered = }` | the pointer entered (`true`) or left (`false`) an interactive GUI element |

Events are queued during the frame and delivered by the next update. While the module is paused by the host
(`IGameModule::SetPaused`) no update runs, so `ui.back` waits; `ctx:ui_back_pressed()` answers at once.

### GUI node interaction

A `GuiNode` element (scene JSON, `elements` array) takes these optional keys, read and written by the scene file:

| Key | Default | Meaning |
| --- | --- | --- |
| `interactive` | `false` | takes the pointer and keyboard focus, and reports `ui.hover` / `ui.click` |
| `id` | `gui<node>.<element>` | name in the events and in the hit test; keep it unique in the scene |
| `on_click` | `""` | action carried by `ui.click` as `action` (for example `"quit"`) |
| `visible` | `true` | a hidden element is neither drawn nor hit |

Interactive elements are hit-tested in window coordinates above the engine's other pointer regions and below the Lua
widgets. A focused element gets an outline. The element's text, colour and rectangle are not editable in the inspector;
edit them in the scene file.

Known limits: no text input, no scrolling lists, no key repeat, no focus wrap (the edge of a menu stops the focus).

## Input (action maps)

`projects/<name>/input.json` maps names to devices. The engine samples the keyboard and the first attached gamepad
once per frame, on the gameplay thread, before the systems. Every system and fixed substep of the frame sees the
same sample.

```json
{
  "format": 1,
  "actions": {
    "accelerate": { "type": "button", "keys": ["W", "Up"], "pad_buttons": ["a"] },
    "steer": { "type": "axis", "keys_negative": ["A", "Left"], "keys_positive": ["D", "Right"],
               "pad_axis": "leftx", "deadzone": 0.15 }
  }
}
```

- Key names are SDL key names (`W`, `Space`, `Left Shift`); pad buttons and axes are SDL controller names
  (`a`, `leftx`). Unknown names, unknown types and malformed entries are reported and the entry is skipped.
- `button` actions take `keys` and `pad_buttons`. `axis` actions take `keys_negative`, `keys_positive` (each
  contributes -1 or +1) and/or `pad_axis` (-1..1 with the deadzone removed and rescaled to the full range).
  `deadzone` is in [0, 0.9), default 0.15. Axis values are clamped to -1..1.
- Without window focus, or when the host reports input as disabled, every action reads as released.

| Function | Returns |
| --- | --- |
| `ctx:action_down(name)` | true while the action is held this frame |
| `ctx:action_pressed(name)` | true on the frame the action went down |
| `ctx:action_released(name)` | true on the frame the action went up |
| `ctx:axis(name)` | the axis value (buttons give 0 or 1) |
| `ctx:pointer_position()` | window coordinates `x, y` (`ui.pointer()` is relative to the overlay) |
| `ctx:pointer_down(b)` / `ctx:pointer_pressed(b)` / `ctx:pointer_released(b)` | `b` is `"left"`, `"right"` or `"middle"` |
| `ctx:pointer_wheel()` | wheel notches this frame |
| `ctx:over_ui()` | true when the pointer is over an engine UI region |
| `ctx:wants_pointer()` / `ctx:set_wants_pointer(bool)` | the module's pointer policy (absolute, visible pointer for menus). Kept across reloads |

An unknown action name raises `unknown input action 'name'` in the calling system or handler, which faults it.

## Input control (rebinding, rumble)

These functions change the running project's action map (the `input.json` of the project). Rebinds are written to
the user directory (`input_bindings_<path>.json`, never to the project) and applied again the next time the map is
loaded, so the player's keys survive a restart.

| Function | Returns |
| --- | --- |
| `input.rebind(action, key)` | `true`, or `nil, error`. Replaces the keyboard keys of a `button` action with one SDL key name. Axis actions, unknown actions and unknown key names fail. Pad buttons are kept |
| `input.bindings(action)` | `{ keys = {...}, pad_buttons = {...} }` for the action, or `nil, error` |
| `input.save_bindings()` | `true` when the rebinds were written to the user directory, otherwise `false` |
| `input.rumble(low, high, ms)` | `true` when the first attached gamepad accepted the rumble. `low` and `high` are motor strengths in 0..1 (clamped), `ms` is 0..5000 (default 200). `false` without a gamepad |

Without a loaded project these functions fail softly (`nil, error` or `false`). Rebinding changes what
`ctx:action_down` and `ctx:axis` report from the next frame on.

## Audio (buses, listener, one-shots)

The mixer is an engine service (`src/audio/AudioMixer.*`). Every source plays through a bus, and the gain of a
channel is `source volume * bus volume * master volume`. A muted bus, or a muted `master`, is silent.
Details and a short tutorial: `docs/audio.md`.

| Function | Notes |
| --- | --- |
| `audio.play_one_shot(path, opts)` | plays a sound once, at a world position or unspatialised. `opts` (all optional): `position = {x, y, z}` (3D, heard from the current listener), `bus` (default `"sfx"`), `volume` (0..1, default 1), `pitch` (0.1..10, default 1). Returns a handle, or `nil, error`. The gain is set when the sound starts |
| `handle:stop()` / `handle.playing` | stops the sound if it still plays / true while it plays |
| `audio.bus_volume(name)` / `audio.bus_volume(name, v)` | reads or sets a bus volume (0..1, clamped). Returns the volume, or `nil, error` for an unknown bus |
| `audio.bus_mute(name)` / `audio.bus_mute(name, muted)` | reads or sets the mute flag. Returns the flag, or `nil, error` for an unknown bus |
| `audio.buses()` | array of bus names: `master`, `music`, `sfx`, `ui`, `voice`, then the project's extras |

- Paths go through AssetIO like every other sound (loose files and packs, the same sandbox as `AudioNode`).
- Without an audio device, or for a sound that does not load, `play_one_shot` returns `nil, error`; nothing raises.
- Bus changes reach the sounds that are already playing (`AudioNode` sources and the procedural vehicle audio) on the
  next frame. A one-shot keeps the gain it started with.
- The 3D listener is the current `AudioListener` node (scene component `"AudioListener": { "current": true }`), or
  the camera when there is none. The listener is published each frame, so one-shots use the same pose as the
  sources. The listener's forward direction is its local `-Z`.

## Time scale

`ctx:time_scale([v])` reads or sets the global game time scale (`src/TimeScale.*`). Without an argument it returns
the current scale; with one it sets the scale (clamped to 0..10) and returns the new value. A non-finite value raises.

- The scale applies to game time: the Lua systems and behaviours (`dt` and the fixed substeps), animation, physics
  and the native particle pools. It does not apply to input, the editor UI, debug draw lifetimes or the frame timers.
- Scaled time feeds the same fixed-step accumulator as before, so a scale of 0.5 runs half the physics steps per real
  second and a scale of 0 runs none. A given scale and frame sequence give the same steps.
- Reload polling (hot reload of scripts) uses the scaled `dt` as well, so it pauses at scale 0.
- The engine exposes the same value as `Core::GetTimeScale()` / `Core::SetTimeScale()`.

## Debug draw

`debug.*` queues world-space primitives that are drawn after the scene, in the editor viewport and in play mode.
The primitives are drawn through the ImGui draw list (the same path the editor viewport uses), so no renderer is
involved. Source: `src/diagnostics/DebugDraw.*`.

| Function | Notes |
| --- | --- |
| `debug.line(a, b, colour, duration)` | segment between two points |
| `debug.box(center, half, colour, duration)` | axis-aligned box; `half` is `{x, y, z}` or one number (cube half size) |
| `debug.sphere(center, radius, colour, duration)` | three great circles |
| `debug.text(position, text, colour, duration)` | label at a point (skipped when behind the camera) |
| `debug.clear()` / `debug.count()` | removes every primitive / the number queued |

- Points are `{x, y, z}` or `{x = , y = , z = }`. A colour is `0xAARRGGBB` like `ed.line3`; a value up to `0xFFFFFF`
  is opaque RGB. Default white.
- `duration` is in real seconds, unscaled by the time scale. `0` (or omitted) draws for one frame: call it every
  frame to keep a shape visible.
- Primitives behind the camera are skipped per segment; the queue holds at most 4096 primitives.
- The `debug` table is the Lua debug library name; the runtime does not open the standard debug library, so the
  name is free for this API.

## Camera

The module camera (`IGameModule::GetCamera`) is owned by the module. Scripts change its pose and projection
through `ctx`; the values are plain state (not declared data), so a reload keeps the current camera as it is.

| Function | Notes |
| --- | --- |
| `ctx:camera_look_at(px, py, pz, tx, ty, tz)` | sets the position and looks at the target. Pitch is clamped to +-89 degrees by the camera. A target equal to the position is an error |
| `ctx:camera_position()` | returns `x, y, z` |
| `ctx:camera_set_fov(degrees)` | 10..150 |
| `ctx:camera_set_clip(near, far)` | `0.01 <= near < far <= 100000` |
| `ctx:camera_cut()` | reports a discontinuous camera move: the next `ConsumeTeleport()` is true once, so temporal history (TAA, motion blur) resets |
| `ctx:camera_project(x, y, z)` | `nx, ny, distance, visible`: the point in the game overlay, 0..1 with y down, at the overlay aspect. `visible` is false behind the camera or more than a quarter of the view outside it (then `nx, ny` are 0.5) |

Camera calls fail with `no module camera is available` when the scene has no module camera.

## Rendering

Render settings and render effects are components (`docs/rendering_components.md` lists every property):
`WorldEnvironment` (scene root), `ParticleEmitter`, `Decal`, and the `Camera` component's `current` flag. Read and
write them with `ctx:component(h, name)` like any other component. Writes follow the Components rules above.

- `WorldEnvironment` sits on the scene root only. The root is named `Root` (`ctx:find("Root")`), so
  `ctx:component(ctx:find("Root"), "WorldEnvironment")` returns its proxy; a scene that names another node `Root`
  gets that node instead. Field writes such as `env.bloom_strength = 1.2` change the live renderer, and the scene
  saves them to `render_settings`.
- `emitter:burst(n)`, `emitter:restart()`, `emitter:clear()` drive a `ParticleEmitter`. Live counts read through
  `particle_count`.
- Switching the view: set `current = true` on the camera node's `Camera` component and `current = false` on the
  previous one (in Play mode the first node with `current` set drives the render camera; the editor camera is
  used otherwise).
- `ctx:screen_to_ray(nx, ny [, aspect])` returns the ray `ox, oy, oz, dx, dy, dz` through the normalised view point
  (0..1, y down) of the current camera node, or nil when no node has `current` set. `aspect` defaults to 16:9.

Scene raycasts: the engine does not expose the scene's own `CollisionWorld` to modules. Collision queries use the
declared collision files below (`nox.collision`). A scene-wide query helper is not implemented.

## Effects (native emitters)

`ctx:fx_smoke(x, y, z, vx, vy, vz, intensity, dt [, floorY])` and `ctx:fx_spray(...)` queue smoke or spray at a
world position with a base velocity (VehicleFX emitters). `intensity` is 0..1, `dt` 0..1 seconds (the rate is
rate-independent, as in `VehicleFX::EmitSmoke`), and `floorY` stops droplets at the ground.

- Scripts only emit. The pools (integration, culling, GPU list) are native and tick once per frame, after the
  systems, while the module's world is running. No Lua runs per particle.
- At most 512 emitter calls per frame. A call over the budget returns `false` and does nothing.
- `ctx:fx_set_enabled(bool)` stops emission and hides existing particles; `ctx:fx_clear()` removes them.
- `ctx:fx_count()` returns the number of live native particles (all pools), for diagnostic logs.
- The pools are process-wide (one VehicleFX system); only the active project module emits.

## Declared files: navigation and collision

Road networks and collision meshes are loaded only when the project declares them in `project.json`:

```json
"roads": [ "data/town_roads.json" ],
"collision": [ "models/town_collision.glb" ]
```

- `nox.roads(name)` returns a road network handle. The name is the one written in `project.json`. The file is
  read through AssetIO (packed projects work) and must hold a `RoadNetwork` JSON with nodes and edges.
  `nox.roads.from_table(tbl)` returns a handle for an inline network instead (see "Project data").
- `nox.collision(name)` returns a collision handle built from the declared glTF (triangles only, world transforms
  applied). Undeclared names raise `... is not declared in project.json`.
- `nox.collision_file(logical)` does the same for a glTF path that the project declares in `collision` or that the
  scene names in `game.collision.files` (logical paths). Any other path raises an error.
- At most 8 live road networks and 8 collision worlds per VM. Inline networks are shared when their tables are
  identical; `nox.roads.release(road)` frees an inline network (handles that share it become invalid, declared
  networks cannot be released). 256 road slots are created per VM at most.

Road network methods (`net:name(...)`, colon syntax):

| Method | Returns |
| --- | --- |
| `net:route(x1, z1, x2, z2)` | array of `{x=, z=}` along the streets (at most 4096 points), or `nil` when there is no route |
| `net:nearest(x, z)` | `x, z, distance, edge` (edge is 1-based) of the closest point on the centre lines; `nil` for an empty network |
| `net:edge_polylines([segments])` | one flat `{x1, z1, x2, z2, ...}` array per edge (1..8 segments, default 4), ready for `ui.polyline`; bounded to 32768 segment points per call |
| `net:bounds()` | `minx, minz, maxx, maxz` |
| `net:edge_count()` | number of edges |
| `net:edges_in_rect(minx, minz, maxx, maxz)` | `{edge indices}` (1-based) of the edges whose band (road plus sidewalk) meets the rectangle |
| `net:edge_polyline(i [, segments])` | flat `{x1, z1, ...}` for one edge (1..8 segments, default 4); `i` is 1-based |
| `net:edge_info(i)` | `halfWidth, y` (mean node height), then a table `{name, oneway, lanes, lane_width, carriageway, from, to}` (`from`/`to` are node ids) |

Collision methods (`col:name(...)`):

| Method | Returns |
| --- | --- |
| `col:ground_height(x, z [, fromY, maxDrop])` | height of the walkable surface below `fromY` (default 1000) within `maxDrop` (default 2000), or `nil` |
| `col:raycast(ox, oy, oz, dx, dy, dz, maxT [, mask])` | `{t, x, y, z, nx, ny, nz}` of the first hit, or `nil`. The direction is normalised; `maxT` 0..100000 |
| `col:sphere_push(cx, cy, cz, radius [, mask])` | the centre pushed out of the surfaces (`x, y, z`) and the contact normal (`nx, ny, nz`, zero without contact). Overlap query, not a sweep |

`mask` is the class mask: `1` ground, `2` wall, `4` ceiling, `7` any (the default).

A handle kept across a reload refers to the old world's data and raises `this ... handle is no longer valid`.

## Save data

Save data lives in the user directory (`UserSettings`), never in the project. Names are relative and validated:
1 to 4 segments of letters, digits, `_`, `-` and `.`, the last ending in `.json`, at most 96 characters.

- `ctx:load_user_json(name)` returns the decoded value, or `nil, reason` when the file is missing, larger than
  1 MiB or not valid JSON.
- `ctx:save_user_json(name, value)` returns `true`, or `false, reason`. The value is a table that converts to JSON
  (arrays are tables with keys `1..n`, objects have string keys; no functions, userdata, threads, mixed tables,
  NaN or infinity). Limits: 20000 values, 16 levels, 64 KiB per string, 1 MiB in total.
- Both are synchronous and run on the gameplay thread. Authoring saves (scenes, configs) are not available here.

## Vehicle audio (procedural)

The synthesis is an engine service (`src/audio/ProceduralVehicleAudio.*`): an engine with harmonics driven by rpm and
load, intake noise, a rev-limiter bounce, tyre squeal from slip, wind from speed, a boost hiss and short cues. Every
sound is rendered in the SDL audio callback, so no sound files are needed.

The gameplay thread publishes parameters once per frame; the callback only reads the last published copy and the
queued cues. Lua never runs in the callback.

| Function | Notes |
| --- | --- |
| `ctx:audio_set({ ... })` | partial update of the engine state. Keys: `active` (engine audible), `paused` (fades out; cues still play), `rpm` (0..30000), `rpm_max` (1000..30000), `throttle`, `speed01`, `slip01` (0..1), `boost` (boolean, boost hiss), `limiter` (boolean, rev-limiter cut this frame), `master_volume` (0..1). Numbers are clamped; unknown keys and wrong types raise an error |
| `ctx:audio_cue(name)` | queues a cue: `select`, `back`, `tick`, `start`, `marker`, `complete`, `fail`, `record`, `gear_shift`. Returns `true`, or `false` when the frame's budget (16 cues) is used. Unknown names raise an error |

- The module publishes the state after the systems each frame (also while paused, with `paused` forced on). When no
  script touches audio, nothing is published (the module still opens its output device at startup).
- `paused` set by the host (`SetPaused`) fades the engine out in the same way, whatever the script sets.
- Without an output device (headless machines, CI), `Init` reports why, and every audio call is a no-op.
- A project that stops playing should send `ctx:audio_set({ active = false })` (the sample does this at `end_play`).

The racing game keeps its `DriveAudio` API (`src/DriveAudio.*`), now a thin wrapper that maps its cue names onto the
engine names above. Each owner opens its own output device.

## Vehicles

Status: generic vehicle API (`ctx.vehicle`, host in `src/scripting/LuaVehicleBindings.*`) and the Lua vehicle controller
(`src/scripting/LuaVehiclePolicy.*`) over the native backend (`src/vehicle/`). Transitional: the racing game selects the
Lua controller with `NOX_VEHICLE_POLICY=lua` (see "Transitional selection"). Engine code holds no vehicle formulas or names.

### Division of work

The native backend (`VehicleService`) evaluates tyres, load transfer, drive and brake forces from actuation values,
integration, collision circles, suspension, rig posing and the mechanical gearbox (ratios, rpm, limiter). The controller
decides everything else: pedal mapping, reverse, gear selection and shifts, boosts, drift entry and exit, steering
assists, yaw assists, the velocity swing, burnout and the brake split, handbrake lock and the rest hold. A vehicle has
exactly one controller. There is no native default policy, so no hidden policy can run underneath a Lua controller.

### Definition data

`ctx.vehicle.create({ definition = "<logical path>", ... })` reads a JSON object and overlays the call's fields on it
(shallow, top level). Accepted keys (anything else is an error, so typos fail loudly):

| Key | Content |
| --- | --- |
| `geometry` | `front_axle_m`, `rear_axle_m`, `track_m`, `wheel_radius_m` (FL FR RL RR), `wheel_rest_m` (4 points, required for a wheel rig), `collision_radius_m`, `collision_offset_m` |
| `tuning` | `top_speed_kmh`, `drive_accel`, `brake_decel`, `reverse_max_kmh`, `mass`, `cg_height`, `tire_grip`, `tire_grip_long`, `rpm_idle`, `rpm_limiter`, `gear_ratios` (1..8, falling), `suspension_freq`, `suspension_damping`, `bump_amplitude`, `roof_height` |
| `rig` | node names: `root`, `model`, `body`, `wheels` (4). Resolved through the host's node resolver; missing names leave the slot empty (planar vehicle) |
| `spawn` | `x`, `z`, `heading_deg`, optional `y` |
| `controller` | `module` (required), `params` (object passed to `new`) |
| `format`, `id`, `comment`, `presentation` | project metadata, accepted and not read |

`projects/nox-racing/data/vehicles/r33.json` is the first definition. Its `tuning` and `controller.params.handling` hold
the same numbers for shared keys (a test checks this and checks the NFS scene's `game.handling`).

Geometry, backend, drive and rig are fixed for a live vehicle. `ctx.vehicle.set_profile` accepts numeric tuning only and
returns `nil, "... restart Play to change the geometry or rig"` for the rest.

### `ctx.vehicle`

| Function | Behaviour |
| --- | --- |
| `create(opts)` | Returns a handle, or `nil, reason` (unreadable definition, unknown key, missing or unloadable controller). |
| `destroy(h)` | Removes the vehicle. Stale handles are ignored. |
| `valid(h)` | Boolean, never raises. |
| `count()` | Live vehicles. |
| `step(dt [, frozen])` | Steps every vehicle with its latched input (the service clamps dt and runs 240 Hz sub-steps). Called by the project; nothing runs on its own clock. |
| `set_input(h, { throttle, brake, steer, handbrake, aux })` | Latched until changed. `aux` is a table of named auxiliary inputs (`name = number or boolean`, booleans as 1 / 0) that the controller interprets; a name not given reads as 0. Ranges are checked (raises on bad values). |
| `telemetry(h)` | Table: `x`, `z`, `heading_deg`, `vx`, `vy`, `speed_kmh`, `yaw_rate`, `steer_deg`, `gear`, `rpm`, `limiter`, `slip`, `load`, `airborne`, `shift_timer`, `channels` (the controller's named channels, flags as 1 / 0), `faulted`. |
| `teleport(h, x, z [, heading_deg])` | Discontinuous move (the service flags it). |
| `reset(h)` | Back to the spawn (controller `on_respawn`). |
| `set_controller(h, module [, params])` | Swaps the controller; params default to the vehicle's current ones. Returns `nil, reason` when the module cannot be loaded; the vehicle then runs the faulted controller (coasts). |
| `set_profile(h, tuning)` | See above. |
| `faults(h)` | Strings `phase (xN): message` for the vehicle's controller faults. |

A stale handle raises `stale or invalid vehicle handle` in every function that takes one, except `valid` and `destroy`.

### Controller protocol

A controller is a module returning a table with `new(params)`. Required functions: `control`, `yaw_assist`, `velocity`,
`post_step`. Optional: `on_teleport`, `on_respawn`, `on_hold`.

```lua
local C = {}
C.__index = C
function C.new(params) return setmetatable({ debug = { my_channel = 1.0 } }, C) end
function C:control(tel, inp, act)      -- every field of act must be set
function C:yaw_assist(tel, inp, io)    -- io.yaw_acc, io.ax in and out
function C:velocity(tel, inp, io)      -- io.vx, io.vy in and out
function C:post_step(tel, inp, io)     -- io.gear in and out; io.gear_shifting out
return C
```

Per sub-step the engine makes four calls (control, yaw, velocity, post step) and no others. The telemetry, input and io
tables are reused: read them during the call only. `act` is cleared before each `control` call, so a missing field is a
fault. `debug` (optional) is read after each call. `shift_timer` and `shift_event` (one-shot, for `ConsumeShift`) are the
gearbox fields the engine reads by name. Every other number or boolean field becomes a named channel (booleans as 1 / 0)
that `telemetry(h).channels` reports and the facade reads; the engine assigns no meaning to channel names.

Telemetry fields (snake case, floats): `time`, `h`, `pos_x`, `pos_y`, `vel_x`, `vel_y`, `heading`, `vx`, `vy`, `yaw_rate`,
`ax`, `ay`, `steer_angle`, `speed`, `speed01`, `beta`, `vx_abs`, `wheelbase`, `track`, `top_speed`, `rear_spin`, `rpm`,
`wheel_speed` (valid in `post_step`), `gear`, `gear_count`, `limiter`, `airborne`, `surface`, `ground_pitch`,
`ground_roll`, `rpm_idle`, `final_drive`, `rear_wheel_radius`. Input: `throttle`, `brake`, `steer`, `handbrake`, and `aux`
(named auxiliary inputs, see `set_input`).

Actuation fields set by `control`: `gear`, `steer_angle`, `drive_scale`, `drive_shift_cut`, `drag_scale`,
`rear_grip_scale`, `drive_throttle`, `reverse_drive`, `brake_command`, `brake_front_share`, `brake_rear_share`,
`brake_at_standstill`, `rear_drive_cut`, `rear_spin_floor`, `rear_lock`, `hold_at_rest`. Ranges: gear -1 or 1..gear_count,
steer within 1.6 rad, scales within their documented bounds, pedals and shares in [0, 1].

### Faults and safe actuation

Any of these faults the controller: a Lua error, the instruction budget (per call; the runtime's limit, which the
transitional path sets from `controller.instruction_budget` in the data file), a missing or out-of-range output, or a
non-finite value. While faulted, the vehicle receives the safe actuation: no drive,
no boost, steering centred, a brake command of 0.25 (coast to a stop), the gear it had, no shift. No other policy takes
over. The fault is recorded in `faults(h)`. Under `NOX_VEHICLE_POLICY=lua` the car's facade prints each new fault once.
`LuaVehiclePolicy::RetryFaulted()` (C++, not exposed to scripts) re-enables a faulted controller; replacing it with
`set_controller` is the script-side recovery.

### Transitional selection and recording

- `NOX_VEHICLE_POLICY=lua`: the racing car's policy is the project controller (`vehicle.nfs_controller` from r33 data,
  scene handling as its `handling`). `LegacyNfsPolicy` is not called for the car. The facade keeps `LegacyNfsPolicy` only
  as the handling store. Without the variable, the native policy is used (parity reference).
- `NOX_VEHICLE_POLICY_TRACE=<file.jsonl>`: every policy call (inputs, outputs, debug state) is written as one JSON line
  (`%.9g`, non-finite values as null). `VehiclePolicyTrace::Replay` feeds the recorded calls into another policy and
  reports the error per output field (see `tests/native/test_vehicle_policy_lua.cpp`,
  `NOX_VEHICLE_POLICY_REPLAY=<file>`).
- `tools/VehicleParity.ps1 -PolicyTrace` records the traces for all parity scenarios.

This section describes transitional behaviour: the racing game's presentation (camera, HUD, effects, audio) still reads
the facade, and the scene-driven handling is copied into the controller when the scene is configured.
