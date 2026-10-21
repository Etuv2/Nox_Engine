# NOX engine overview

NOX is a Windows x64 C++20 engine on OpenGL 4.6. A scene is a JSON file of nodes. Each node carries
components. Gameplay is written in Lua, in a project directory the engine loads. The engine knows nothing
about a particular game: a project decides what its nodes mean and how they behave. The rule is in
[engine_boundary.md](engine_boundary.md).

## Architecture

### Scene graph and ECS

- `SceneGraph` (`src/SceneGraph.h`) owns a root `SceneNode` tree and the ECS world it shares with the systems.
- `SceneNode` (`src/SceneNode.h`) is a hierarchy node that wraps an ECS entity. Its transform, renderable,
  animation, camera, light and collider data live in components (`src/ComponentTypes.h`:
  `TransformComponent`, `RenderableComponent`, `LightComponent`, `CameraComponent`, `AnimationComponent`,
  `ColliderComponent`, `PhysicsComponent`, `AudioComponent`, `LPVVolumeComponent`).
- A node has a type that selects its scene-file form and its subclass: `node`, `model`, `light`, `audio`,
  `camera`, `gui`, `lpv_volume`, `skeletal`, `folder`.
- A `folder` node groups other nodes in the hierarchy. It has no transform (not saved, always identity when
  loaded), no renderable, no collider and no ECS data beyond its entity, so it is never drawn and never reaches
  physics or the ray-tracing BVH. Its children keep the transforms they were authored with, and moving a node into
  or out of a folder keeps its world transform.
- Nodes are created from JSON by `SceneLoader` (`src/SceneLoader.h`), which also writes them back. Keys the
  loader does not model are kept and written back unchanged, so a scene survives a load and save cycle.
  See [building.md](building.md) for the round-trip check.
- Prefabs are reusable subtrees saved as `*.prefab.json` files. See [prefabs.md](prefabs.md).

### Reflection registry

`src/reflect/Reflection.h` describes components by name. A `ComponentDesc` lists its `PropertyDesc` entries
(typed, ranged, with a getter, an optional setter and an `applies` test for variants) and its `MethodDesc`
entries. The same descriptors serve four consumers:

- scene files: `"components": { "Light": { "intensity": 2 } }` on any node (`SerializeComponents` and
  `DeserializeComponents`);
- the editor inspector, which builds its widgets from the descriptors and makes edits undoable
  ([editor.md](editor.md));
- Lua, through `ctx:component`, `ctx:add_component` and `nox.components()` ([lua_api.md](lua_api.md#components-reflection-registry));
- prefab overrides.

Descriptor files: `src/reflect/BuiltinComponents.cpp` (Node, Transform, Light) and one file per group in
`src/reflect/components/`. Property names are snake_case and stable because they are scene keys.

### Components

| Category | Components | What it does |
| --- | --- | --- |
| Core | `Node`, `Transform` | Name and hierarchy data; position, rotation (degrees) and scale |
| Rendering | `Light` | Directional, point, spot, area or studio light owned by a light node; photometric units |
| Rendering | `Camera` | Projection, field of view, clip planes; `current` selects the camera that drives the play view |
| Rendering | `MeshRenderer` | Model facet: culling, reflection-capture exclusion, bounding radius (mesh and material counts are read-only) |
| Rendering | `WorldEnvironment` | Environment node (root): the scene's `render_settings` |
| Rendering | `ParticleEmitter`, `Decal` | GPU particles and projected decals (`src/render/`) |
| Environment | `LightPropagationVolume` | Light propagation volume for global illumination: centre, extent, voxel size, grid resolution |
| Physics | `RigidBody`, `Collider` | Body type, mass, velocities, damping, material; collision shape, layer and mask |
| Physics | `Trigger`, `CharacterController` | Trigger volumes that raise events; character controller with height, radius, step and slope limits |
| Audio | `AudioSource`, `AudioListener` | Sound playback on a node and the 3D listener |
| Animation | `Animator` | Clip playback (play, stop, pause, resume), speed, loop, time and morph weights |
| UI | `Gui` | Screen-space elements (text, rectangles, progress bars, images) |

Pages for the subsystems behind them: [rendering_components.md](rendering_components.md) (world environment, camera,
particles, decals), [physics.md](physics.md) and [audio.md](audio.md).

### Systems

- **Rendering**: `ModularRenderer` and the render passes (`src/passes/`) on the RHI in `src/rhi/` (see
  [rhi.md](rhi.md)). Deferred shading with cascaded shadows, reflection probes, decals and particles.
  Scene render settings are stored in the scene file (`render_settings`).
- **Physics**: `PhysicsEngine` (`src/PhysicsEngine.*`) with bodies, colliders, character control, trigger
  events and raycasts. See [physics.md](physics.md).
- **Audio**: `AudioMixer` buses, `AudioListener`, one-shot playback and procedural vehicle audio
  (`src/audio/`). See [audio.md](audio.md).
- **Input**: `InputIntegration` maps device input to named actions. Actions come from the project's
  `input.json`; bindings can be changed and saved at run time (see "Input" in [lua_api.md](lua_api.md)).
- **Animation**: `AnimationSystem` and skeletal animation for imported models.
- **UI**: `GuiNode` for screen-space elements, and `ui` draw handlers in Lua for game HUDs and menus.
- **Scripting**: an embedded Lua runtime (`src/scripting/`). Projects use it for behaviour, systems, events
  and editor panels. See [lua_api.md](lua_api.md).
- **Assets**: `AssetIO` resolves logical paths over loose folders or packs, and manages user, cache and
  save folders. See [asset_io.md](asset_io.md).
- **Packaging**: `tools/package.cmd` stages a standalone distribution, a game or the bare engine. See
  [packaging.md](packaging.md).
- **Diagnostics**: `src/diagnostics/` (debug draw and `debug.*` in Lua), frame profiling and the editor
  smoke test ([editor.md](editor.md#smoke-test)).

## Projects

A game or demo is a project: a directory under `projects/<name>/`. The engine ships two examples:
`projects/sample-arena/` (small, Lua only) and `projects/nox-racing/` (a larger game with data files).

```
projects/<name>/
  project.json     descriptor: format, runtime, api_version, entry, script_roots, data_roots, fonts, icons, input
  input.json       action map (optional, referenced from project.json)
  scripts/         Lua: main.lua (entry), systems/, behaviours, HUD and editor panels
  data/            project data read through AssetIO (optional)
```

A scene selects its project with a top-level key. The entry script runs when that scene is configured:

```json
"project": "projects/sample-arena/project.json"
```

`projects/sample-arena/project.json` is the minimal example: `format` 1, `runtime` "lua", `api_version` 1,
`entry` "scripts/main.lua", `script_roots` ["scripts"]. Fields are specified in
[lua_api.md](lua_api.md#project-descriptor).

## Making a new project

The quick way is a template. In the editor, **File > New Project...** takes a name, a location (a folder directly
under `projects/`, default `projects/<name>`) and a template, then opens the project's main scene. From a shell
(for tests and scripts) the same thing is:

```powershell
build\vs2022-x64\Release\Nox_Engine.exe --new-project my_game --template third_person_3d [--path projects/my_game] [--force]
```

It runs without a window, prints the files it made, and refuses an existing folder unless `--force` is given
(`--force` overwrites files; it never deletes). The templates live in `templates/` (`empty_3d`: a sun, a floor, a sky
and the camera pose; `third_person_3d`: a walking character, a follow camera, coins and a HUD). Each template has `project/` (the project
tree), `packaging.json` (the packaging profile template) and `template.json` (the name shown in the picker).
Creating a project also writes `packaging/<name>.json`, so the project can be packaged at once
([packaging.md](packaging.md#creating-a-new-game)). Templates need a source checkout; a packaged build cannot
create projects.

By hand, a project is the same tree as a template:

1. Create `projects/<name>/project.json` with the fields above. Copy `projects/sample-arena/project.json`
   and change `name`.
2. Write `scripts/main.lua`. It runs once when the project loads and can `require` other files from
   `script_roots`. Lifecycle, entities and the `ctx` object are in [lua_api.md](lua_api.md#lifecycle).
3. Make the scene. Use `"project"` at the top level, and place nodes with `"components"` for physics, audio,
   lights and the rest. Save reusable parts as prefabs ([prefabs.md](prefabs.md)).
4. Add physics, audio and rendering behaviour through the component reference:
   [physics.md](physics.md), [audio.md](audio.md), [rendering_components.md](rendering_components.md).
5. Run the engine on the scene: `NOX_FIRST_SCENE=scenes/<name>.json`, or open it in the editor and press Play.
   Script errors are listed in the editor console and the Lua diagnostics (see [lua_api.md](lua_api.md)).

Projects read their content through logical paths, so the same project works from loose files and from a
package. Do not hard-code absolute paths or engine source paths in a project.

## Where to read next

- [Editor](editor.md): hierarchy, inspector, undo, prefabs, play mode.
- [Lua project API](lua_api.md): the contract scripts use.
- [Building and validation](building.md) and [packaging](packaging.md).
- [Asset I/O](asset_io.md) and [engine/game boundary](engine_boundary.md).
