# Prefabs

A prefab is a reusable node subtree saved as an asset. It is the engine's equivalent of a Unity prefab or a Godot
packed scene: author a crate once, place many instances, edit an instance in the scene, and keep the link to the
prefab so that changes to the prefab reach every instance that did not override them.

The code is in `src/scene/Prefab.h` and `src/scene/Prefab.cpp`, with the loader and save changes in
`src/SceneLoader.cpp`. Everything is generic: a prefab holds nodes in the scene-file node format, so any registry
component (see `docs/lua_api.md`, "Components") is part of a prefab without engine changes.

## The prefab file

A prefab is a JSON file whose name ends in `.prefab.json`, stored under the project's asset tree (for example
`prefabs/crate.prefab.json`). Its root is one node in the same format a scene file uses (`type`, `name`,
`position`, `rotation`, `scale`, the legacy keys such as `light` or `path`, `collider`, `components` and `children`):

```json
{
  "type": "node",
  "name": "Crate",
  "children": [
    { "type": "node", "name": "Lid", "position": [0, 1, 0] },
    { "type": "light", "name": "Glow", "light": { "type": "point" }, "position": [0, 2, 0] }
  ]
}
```

Only `*.prefab.json` is recognised. The file is read through `AssetIO` (so packed builds read it from the pack), and it
is written only in loose (source) mode, into the project's asset folder.

## Instances in a scene

A scene node may be a reference instead of a plain node:

```json
{
  "prefab": "prefabs/crate.prefab.json",
  "name": "Crate 3",
  "position": [4, 0, -2],
  "components": { "Light": { "intensity": 3.0 } },
  "children": [ { "type": "node", "name": "Handle", "position": [1, 0, 0] } ],
  "overrides": {
    "Lid": { "position": [0, 0.5, 0] },
    "Lid#1": { "remove": true },
    "Glow": { "components": { "Light": { "intensity": 2.0 } } }
  }
}
```

Loading expands the reference into the prefab's subtree and applies, in order:

- **Root keys.** Every key of the reference (`name`, `position`, `rotation`, `scale`, legacy keys) replaces the prefab
  root's value. `components` merges per component and per property.
- **Added children.** `children` are appended to the root's children (they may be references too).
- **Overrides.** `overrides` address descendants by name path (`"Lid"`, `"Arm/Hand"`). Each entry takes the same keys as
  a root reference (`components` merges per property, `children` appends). An entry with `"remove": true` deletes that
  node from the instance.

Duplicate sibling names are addressed with `#k`: in the prefab above the second `Lid` is `Lid#1`. Child names with `/`
are written with `_` in paths. Paths are matched against the expanded tree, so an override whose path no longer exists
after the prefab changed is skipped with a console warning.

The loaded root knows its source: `SceneNode::GetPrefabPath()` returns the prefab path, and the inspector shows
`Prefab: <path>`.

### Nesting

A prefab may contain references to other prefabs (for example a wheel prefab inside a car prefab). Expansion is
recursive with two limits: a prefab that references itself, directly or through others, is an error; and nesting
deeper than 8 levels is an error. A missing prefab is a load error, the same as a missing critical asset.

A nested instance keeps its own link after it is loaded, so it is saved as a reference again when the outer prefab is
saved as a prefab. When an outer instance is saved, its nested content is stored as overrides of the outer instance (see
below), so the outer instance does not repeat the nested prefab's content.

## Saving

Scene save writes each prefab instance as a reference with only the differences from its prefab. The baseline is the
prefab instantiated with no edits, in a scratch graph and a scratch physics world, so the live scene is not touched.
The comparison covers every key of every node in the subtree and every registry property:

- root keys that differ (for example the placement `position`) are written at the top of the reference;
- properties that differ on a descendant are written in `overrides` under its path;
- children that the instance has and the prefab does not are written in `children` (root) or under the path's
  override;
- children the prefab has and the instance does not are written as `"remove": true` under their path.

An instance that matches its prefab is written as the bare reference `{ "prefab": "..." }` plus its placement. If the
prefab file cannot be read at save time, the node is saved as plain nodes with a warning (its link is lost, its content
is kept).

Prefabs themselves are written by `SavePrefab`: the node is written flattened at the root, with nested instances kept as
references.

## Editor

- **Save as Prefab...** (hierarchy, right-click on a node): asks for an asset path (default `prefabs/<node name>.prefab.json`)
  and writes the node and its children. An existing file is replaced. The node stays where it is.
- **Asset browser**: the `prefabs` folder is listed. Right-click a `*.prefab.json` for **Add to Scene**, or drag it onto
  a node in the hierarchy (to make it a child) or onto the empty hierarchy area (scene root). The instance is placed at
  the spawn position and is one undo step.
- **Inspector**: an instance shows `Prefab: <path>` and an **Unpack** button. Unpack clears the link and keeps the content
  as plain nodes (undoable). Undo links the node again.
- Deleting, duplicating and reparenting instances work as for other nodes. Duplicating an instance makes another
  instance of the same prefab with the same overrides.

## Lua

`ctx:instantiate(path, opts)` creates an instance during play and returns its handle. It is an ordinary scene node with
its registry components, not an ECS-only entity. `opts` may set `parent`, `position`, `rotation` (degrees), `scale` and
`name`. Instances created during play are removed at Stop. See `docs/lua_api.md`, "Scene instances".

## Play and Stop

Play records every authored node (its keys, its components and its prefab link). Stop removes nodes created during
play, re-creates authored nodes that a script destroyed, and writes back the registry values that changed. A
prefab instance that was destroyed and re-created keeps its prefab link. Details are in `docs/lua_api.md`,
"Play-world restoration".

## Limits

- An override addresses nodes by name path. Renaming a node in the prefab breaks overrides that name it; they are then
  skipped with a warning.
- Removing a component that the prefab has is not recorded: the save does not write it, so the component comes back when
  the scene is loaded again. Overrides can change component properties and remove whole nodes, but not components.
- A prefab root cannot itself be a reference (`prefab` on the root of a prefab file is an error).
- Nodes generated by a procedural generator or imported from a model file are not saved as children, so they are not
  part of a prefab's override paths either.
- Instances inside a re-created instance (Stop) lose their own links for nested instances; their content and the save
  result are unchanged.

## Code map

| Piece | File |
| --- | --- |
| Expansion (`ExpandReferences`), override paths (`ChildKeys`, `FindPath`), diff (`MakeReference`), file IO | `src/scene/Prefab.cpp` |
| Save as prefab (`SavePrefab`), instantiate (`InstantiatePrefab`) | `src/scene/Prefab.cpp` |
| Reference writing, baseline, node tree serialization | `src/SceneLoader.cpp` (`SerializeNodeTree`, `SerializePrefabReference`, `PrefabBaseline`) |
| Link on the node | `src/SceneNode.h` (`GetPrefabPath`, `SetPrefabPath`) |
| Undoable unpack | `src/editor/InspectorUndo.cpp` (`UnpackPrefabUndoable`) |
| Editor UI | `src/ImGui/EditorWorkspace.cpp` |
| Lua `ctx:instantiate` | `src/scripting/LuaScene.cpp` |
| Tests | `tests/native/test_scene_roundtrip.cpp` (`Prefab.*`, `SceneRoundTrip.*`) |
