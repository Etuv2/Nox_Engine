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
| Assets (Content Browser) | Browse the project as folders and tiles. Place prefabs and models, open scenes, rename, delete. See [Assets](#assets) |
| Console, Build Output | Log messages and the output of build and package actions |
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

## Assets

The Assets window is the Content Browser. It reads the project through `AssetIO` (`src/ImGui/ContentBrowser.cpp`),
so loose and packed projects show the same tree. Packed assets are read-only: file actions are disabled for them.

- **Folder tree** (left): the project root and every folder below it. Build products and third-party folders
  (`build`, `external`, `output`, `captures`, `snapshots`, `node_modules`) and dot-folders are skipped.
- **Items** (right): tiles with a thumbnail, or a **List** with type, size and modified time. The tile size slider
  sets the tile width (64 to 256 px). Textures show a lazily decoded thumbnail; other types show a typed icon with a
  badge (scene, prefab, model, script, audio, shader, font).
- **Breadcrumb** and the Back, Forward and Up buttons (Alt+Left, Alt+Right, Backspace) navigate the folders.
- **Search** matches logical paths under the current folder (or the whole project from the root). The **type**
  filter narrows the results.
- **Details** (bottom): the selected asset's preview, path, type, size and modified time. Textures also show their
  pixel size.
- **Double-click** (or Enter): a scene is opened (with the unsaved-changes check); a prefab or model is placed in
  the scene; a Lua script opens in VS Code when its `code` command is on PATH, else in the OS default editor;
  other files open with the OS default program. A folder is entered.
- **Drag and drop**: drag an asset onto the Scene viewport to place a prefab or model on the ground under the
  cursor, onto a hierarchy node to place it under that node, or onto an asset field in the inspector.
- **Right-click** an item for Open, Show in Explorer, Copy Path, Duplicate, Rename, Delete and New (folder, scene,
  Lua script). Right-click the empty area for New, Show Folder in Explorer and Refresh. Delete moves the file to
  the Recycle Bin after a confirmation. Renaming or deleting does not rewrite references in scenes or prefabs.
- **Refresh**: a background scan lists the tree every two seconds and rebuilds the index only when a path, size or
  timestamp changed, so files added or edited outside the editor appear without a restart.

Thumbnails are decoded on two worker threads and uploaded to the GPU at most four per frame. Only tiles on screen
request a thumbnail. The cache keeps 256 thumbnails and evicts the least recently used one. Files above 64 MB are
not decoded. Everything that touches the GPU or ImGui runs on the UI thread.

The default layout docks the Assets window along the bottom-left. The layout version was raised, so existing
editor layouts are rebuilt once to show it there.

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
- Drag a prefab from the Assets window onto a node in the hierarchy to place an instance under that node, or onto the
  Scene viewport to place it on the ground under the cursor.
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
viewport resize, saving and the Content Browser (it opens `models/` and lists its contents). Use it for unattended validation of editor changes:

```powershell
$env:NOX_EDITOR_SMOKE = "1"
.\build\vs2022-x64\Release\Nox_Engine.exe
```

Unset the variable after the run. The script lives in `EditorWorkspace::RunSmokeScript` in
`src/ImGui/EditorWorkspace.cpp`.
