# Editor

The editor runs the engine on the open scene. It is an ImGui workspace (`src/ImGui/EditorWorkspace.cpp`) with
a scene viewport, a game viewport for Play mode, and dockable windows. Every change a user makes is an
ordinary scene edit: what the editor writes is the scene file, and what it shows is the node tree and the
registry components described in [engine_overview.md](engine_overview.md).

## Windows

| Window | Purpose |
| --- | --- |
| Hierarchy (`SceneHierarchyWindow`) | The node tree. Select, rename, drag to reparent, right-click for the node menu |
| Inspector | The selected node's components, built generically from the reflection registry |
| Assets | Browse project and engine assets. Drag a prefab onto the viewport or a node to instantiate it |
| Console, Build Output | Log messages and the output of build and package actions |
| Materials | Per-primitive material editing for the selected model: factors, textures, UV transforms, overrides |
| Lighting, Rendering, Camera, Animation, Performance | Scene-wide settings and diagnostics |

Windows are opened from the **Window** menu. Project panels (`nox.editor_panel` in Lua) appear in the same menu
while their project is active. See [lua_api.md](lua_api.md#editor).

## Hierarchy

- The tree shows every node; a node's children are nested under it. Prefab instances are shown as one node
  with its own children.
- Click to select. Ctrl+D duplicates, Delete removes. Both are undoable.
- Drag a node onto another node to reparent it, or onto the root row to make it a root node. The move is undoable.
- Right-click a node for **Add Child**, **Duplicate**, **Save as Prefab...** and **Delete**.

## Inspector

The inspector lists the components the node has. Each property is drawn from its `PropertyDesc`:

- the type sets the widget (number, vector, colour, toggle, enum, text);
- `min` and `max` set the range and a clamp, and the setter rejects values it does not accept;
- properties whose `applies` test is false are hidden (for example `spot_inner_angle` on a point light, or
  `luminous_power` on a light authored in candela);
- read-only properties are shown as values that cannot be edited (for example a model's mesh count).

Editing a value is undoable. Each change is one undo step, labelled with the property name.

**Add Component** lists the components the node does not have yet, grouped by category. **Remove Component**
is in the component's context menu when the component can be removed. Add-node items create a node with a
template: Empty, Directional, Point and Spot Light, Audio Source, Camera, GUI, LPV Volume, or a model from an
asset file.

Some components have editor buttons: a method flagged as an editor button (for example an animation's play
button) is drawn in the inspector and invoked on the node.

## Materials

The Materials window (**Window > Materials**) edits the material of every primitive of the selected model node.

- **Primitive list.** One row per primitive: `[index] glTF material name`. Rows are per primitive, so two
  primitives that share a material name are edited separately.
- **Fields.** Base colour, metallic, roughness, normal scale, occlusion strength, alpha mode (opaque, mask with
  cutoff, blend), double-sided and unlit. Emissive colour and strength. Specular, IOR, clearcoat, transmission,
  thickness and attenuation. The NOX weather, flake and interior fields. Sheen, anisotropy, iridescence and
  dispersion are stored and written to the file but not shaded yet.
- **Textures and UV transforms.** One entry per texture slot: base colour, normal, metallic-roughness, emissive,
  occlusion, specular, specular colour, transmission, clearcoat and clearcoat roughness (the two clearcoat slots are
  stored but not sampled yet, see [rendering_components.md](rendering_components.md#gltf-material-support)). **Pick...** assigns a
  file from `textures/` (sRGB for base colour, emissive and specular colour, linear for the rest), **Clear**
  removes the slot and **Revert** restores the imported texture. Each slot has a UV set (`TEXCOORD_0` or
  `TEXCOORD_1`), an offset, a scale and a rotation in degrees, applied as `uv' = T * R * S * uv`
  (KHR_texture_transform).
- **Live preview.** Every change shows at once. A continuous edit (drag, slider, text) is one undo step labelled
  "Edit material". Each texture pick, clear or revert and each UV change is one step when it completes.
  **Reset to imported** is its own step.
- **Reset to imported** restores the primitive's glTF import values, captured before any scene override, and
  removes this primitive's override entry from the node. Name-only entries in the file are not removed.
- **Save as scene override** writes the primitive's state into the node's `material_overrides` (authored extras),
  replacing the entry for the same primitive. Save the scene (Ctrl+S) to keep it.

### Material overrides in the scene file

```json
"material_overrides": [
  { "match": "paint", "base_color": [0.8, 0.1, 0.1, 1], "roughness": 0.3 },
  { "match": "glass", "slot": 4, "alpha_mode": "blend", "double_sided": true,
    "textures": { "base_color": "textures/decal.png", "emissive": null },
    "texture_transforms": { "base_color": { "offset": [0.5, 0], "scale": [2, 2], "rotation_degrees": 45, "texcoord": 1 } } }
]
```

- `match` is a case-insensitive substring of the glTF material name; `*` matches every material. This is the
  format that predates per-primitive editing and it is unchanged.
- `slot` is an optional primitive index into the model (the number the Materials window shows). When present the
  entry applies only to that primitive, and `match` must also match if it is given. Entries without `slot` behave
  as before.
- Entries apply in file order; a later entry wins for the same primitive.
- `textures` maps a slot name to a file path, or to `null` to clear the slot. Unknown slot names are reported in
  the console and skipped.
- `texture_transforms` maps a slot name to `offset`, `scale`, `rotation_degrees` and `texcoord`; every field is
  optional.
- Slot names: `base_color`, `normal`, `metallic_roughness`, `emissive`, `occlusion`, `specular`,
  `specular_color`, `transmission`, `clearcoat`, `clearcoat_roughness`.
- The numeric and flag keys are listed in [nfs2015/configuration.md](nfs2015/configuration.md#materials).

The imported state each primitive is reset to is captured in `Scene::importedMaterials` when the glTF loads,
before overrides are applied (`src/Scene.cpp`, `src/SceneLoader.cpp` for the override reader).

## Undo and redo

- Ctrl+Z undoes and Ctrl+Y redoes. the **Edit** menu has the same two items.
- Undoable: property edits, transform changes made with the gizmo, add and remove component, add and delete
  node, duplicate, reparent, prefab instantiation.
- Not recorded: anything in play mode.
- Undoing a delete re-creates the node. The editor keeps a handle per node, so the undo entry still refers to the
  right node after the re-creation.
- Lua can add its own steps with `ed.undo(label, undo_fn, redo_fn)` (see [lua_api.md](lua_api.md#undo-edundo)).

## Prefabs

- **Save as Prefab...** on a node writes its subtree to `prefabs/<name>.prefab.json` (the name can be changed in the
  dialog) and the node becomes an instance of it.
- Drag a prefab from the Assets window onto a node in the hierarchy to place an instance under that node.
- An instance keeps its link to the prefab. Changes to the prefab reach every instance that does not override
  them. Overrides are saved as differences only.

The file format, overrides and API are in [prefabs.md](prefabs.md).

## Play mode

- **Play** (F5) starts the scene's game module and Lua project. **Pause** (F6) pauses it. **Stop** (Shift+F5) ends it.
- Play records the authored scene. Stop restores it: nodes created during play are removed, nodes a script
  destroyed are re-created, transforms and bodies return to their recorded state, and registry values that changed
  are written back. The limits are listed in [prefabs.md](prefabs.md#play-and-stop) and `src/PlayWorldSnapshot.h`.
- While playing, the editing menus and Save are disabled, and undo is not recorded. Edits made in play are
  discarded at Stop.

## Saving

Ctrl+S (or **File > Save Scene**) writes the current scene through the scene serializer. The file keeps the keys
the runtime does not model, and a light authored in lumens or lux is written back in that unit, converted from its
current intensity, so the file stays in physical units. The round-trip check is described in
[building.md](building.md).

## Smoke test

`NOX_EDITOR_SMOKE=1` runs scripted checks inside the editor and prints `[Smoke] PASS` or `[Smoke] FAIL` lines
to the console. They cover undo and redo of a transform edit, Play and Stop restoring the pre-play state,
viewport resize and saving. Use it for unattended validation of editor changes:

```powershell
$env:NOX_EDITOR_SMOKE = "1"
.\build\vs2022-x64\Release\Nox_Engine.exe
```

Unset the variable after the run. The script lives in `EditorWorkspace::RunSmokeScript` in
`src/ImGui/EditorWorkspace.cpp`.
