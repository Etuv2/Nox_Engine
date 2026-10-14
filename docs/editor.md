# Editor

The editor runs the engine on the open scene. It is an ImGui workspace (`src/ImGui/EditorWorkspace.cpp`) with
a scene viewport, a game viewport for Play mode, and dockable windows. Every change a user makes is an
ordinary scene edit: what the editor writes is the scene file, and what it shows is the node tree and the
registry components described in [engine_overview.md](engine_overview.md).

## Windows

| Window | Purpose |
| --- | --- |
| Hierarchy | The node tree with folders. Select, rename, drag to reparent or group, search, right-click for the node menu |
| Inspector | The selected node's components, built generically from the reflection registry |
| Assets (Content Browser) | Browse the project as folders and tiles. Place prefabs and models, open scenes, rename, delete. See [Assets](#assets) |
| Console, Build Output | Log messages and the output of build and package actions |
| Lighting, Rendering, Camera, Animation, Performance | Scene-wide settings and diagnostics |

Windows are opened from the **Window** menu. Project panels (`nox.editor_panel` in Lua) appear in the same menu
while their project is active. See [lua_api.md](lua_api.md#editor).

## Hierarchy

- The tree shows every node; a node's children are nested under it. Prefab instances are shown as one node
  with its own children. Rows are drawn flat, indented by depth; the arrow on a row collapses or expands it.
- Click to select. Ctrl+D duplicates, Delete removes. Both are undoable. Selecting a node in the viewport expands
  the path to it.
- **Folders** group nodes without affecting them. A folder is a node of type `folder` (tag FOLDER): it has no
  transform, is never drawn, and its children keep their own transforms. Create one with **Node > Add Node > Folder**,
  **New Folder** in the row menu (created under that row) or in the empty area of the tree (created under the
  selected node, or at the root). **Group into Folder** in a row's menu puts that node into a new folder in its
  place. Folder creation, grouping and renaming are undoable (rename is the usual Rename, F2).
- Drag rows to move nodes. Drop on the middle of a row to make the dragged node its child (this moves a node into a
  folder). Drop on the top or bottom quarter of a row to place the dragged node just before or after it under the
  same parent; that is how a node leaves a folder. Drop on the empty area below the rows to move it to the root.
  Every move is undoable and keeps the world transform. A node cannot be dropped into its own subtree.
- **Search** (the box at the top) filters by name: every whitespace-separated term must be part of the name,
  case-insensitively. `t:<type>` filters by node type: `t:folder`, `t:light`, `t:mesh`, `t:audio`, `t:camera`,
  `t:gui`, `t:lpv`, `t:skeletal`, `t:node`. A matching node shows with its ancestors, which are expanded.
- **Collapse** and **Expand** set every row at once. **Sort** chooses scene order (default) or name order with
  folders first. Sort is an editor setting and does not change the scene file.
- The collapse state is kept per scene in the user editor settings (`settings/editor_settings.json`, `hierarchy`
  key), keyed by node path (`Props/Deep`). Renaming a node or one of its ancestors resets the state of that subtree.
- Right-click a node for **New Folder**, **Group into Folder**, **Add Child**, **Duplicate**, **Save as Prefab...**
  and **Delete**.
- Generated nodes (rebuilt on load, not saved) are listed only while a search is active.

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
  node, duplicate, reparent (including drops before or after a row), new folder, group into folder, prefab
  instantiation.
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

## Selection outline

The selected node in the Scene viewport is outlined in orange, so it is clear which node a click or a gizmo
drag is working on. The outline covers the selected node and all of its descendants, skinned meshes included.

- The outline is drawn by `SelectionOutlinePass` (`src/passes/SelectionOutlinePass.cpp`). It runs after tone
  mapping and TAA, on the viewport target, before the editor UI. A mask pass draws the selected meshes into an
  RGBA8 target with their own depth buffer; a fullscreen pass then draws the outline in the pixels just outside
  the silhouette.
- Parts of the selection behind other geometry are drawn dashed and semi-transparent (occluded). Turn this off
  to hide them.
- Hovering a node in the Scene viewport draws a thinner, lighter outline on it. The hovered node is picked about
  ten times a second and not while a gizmo or handle is in use, while the camera flies, or during Play.
- A selected node with no mesh (a light, camera or empty node) gets a wire box and a ring at its origin instead.
- The editor selects one node at a time, so the outline is for that node only.

**Outline** in the Scene viewport toolbar opens the style popup: colour, width (1 to 4 px), dashed occluded parts,
and hover highlight. The values are saved per user under `selection_outline` in `settings/editor_settings.json`.

The outline is editor-only. Only the Scene viewport sets `RenderContext::selectionOutline`; the game viewport,
`--game`, benchmark and packaged builds never set it, so the pass is not planned and costs nothing there.

## External script editor

Lua scripts open in an IDE outside the engine (VS Code by default). Every entry point runs the same launch:

- **Assets** window: double-click a `.lua` file, or right-click it for **Open in External Editor**. Right-click a
  file inside a project for **Open Project Folder in External Editor**.
- **Lua project** panel: **Open Project Folder in External Editor** and **Open Entry in External Editor**. Each
  diagnostic with a file, and each faulted system, behavior or handler that has an error, gets an **Open error** button
  that opens the file at the line.
- **Inspector**: a script reference (an asset path ending in `.lua`, for example a behavior's script) shows
  **Open in external editor** under the field.
- **Edit > Preferences...** chooses the editor. The choice is saved with the editor settings (the `external_editor`
  key of `settings/editor_settings.json` in the user directory):
  - **Auto (VS Code, else OS default)**, the default: VS Code when it is found (`code` on PATH, or `Code.exe` under
    `%LOCALAPPDATA%\Programs`, `Program Files` or `Program Files (x86)`). A file runs `"<exe>" -g "{file}:{line}"`,
    which opens it at the line in the open VS Code window. Without VS Code the file goes to the OS default handler
    (ShellExecute "open").
  - **OS default handler**: always ShellExecute "open".
  - **Custom command**: a template with the placeholders `<exe>` (the detected `Code.exe`, or `code` when none),
    `{file}` (absolute path), `{line}` (1-based; 1 when unknown) and `{root}` (the project folder). Example:
    `"<exe>" --goto "{file}:{line}"`. Folders always open as a VS Code workspace when VS Code is found, else through
    the OS handler.

Launches never block the editor. Each one writes `[ExternalEditor] launched: <command line>` (or `shell open`) to
the console. A failed launch shows a warning toast. Only the loose source tree can be opened (`AssetIO` loose mode);
packed projects have no files on disk.

The launch decisions live in `ExternalEditor::PlanFile` and `PlanFolder` (`src/editor/ExternalEditor.cpp`), and
`tests/native/test_external_editor.cpp` checks them as text. No test starts an IDE.

### IDE support for project Lua

Opening a Lua project in the editor (and **File > Generate Lua Editor Support**) writes two files into the project
folder, `projects/<name>/`:

| File | Content | Tracking |
| --- | --- | --- |
| `.nox/types/nox.lua` | LuaLS annotations (`---@meta`): the API of `nox`, `ctx`, `ed`, `ui`, `audio`, `input` and `debug`, and one `nox.comp.<Name>` class per reflection component with a typed field per property and a function field per method. `ctx:component(h, "Name")` is typed through an overload per component. | Git-ignored (`.nox/`). Rewritten only when its text changes. |
| `.luarc.json` | LuaLS config: `runtime.version` = `Lua 5.5`, `workspace.library` = `./.nox/types`, `workspace.checkThirdParty` = `false`, `diagnostics.globals` = `nox`, `audio`, `input`, `debug`. | Not ignored: project config, commit it if the team shares it. |

- The hand-written part of the API is in `kStubParts` in `src/scripting/LuaEditorSupport.cpp`. The component classes
  come from the reflection registry (`src/reflect/`) each time the file is written, so a new component appears after
  the next write with no change to the scripting code.
- `.luarc.json` is merged, not replaced: keys you set (for example another `runtime.version`, or your own
  `library` entries) are kept, and the engine entries are added to your arrays. A file with comments, or one that is
  not a JSON object, is left unchanged and the reason goes to the console.
- Open the **project folder** (`projects/<name>/`) as the VS Code workspace, not the repository root, so the
  `.luarc.json` at the project root is the one that applies. The stub is read through the library entry.
- The annotations describe Lua 5.5. LuaLS releases differ in whether they accept the `Lua 5.5` runtime name; if the
  language server reports an unknown runtime, use `Lua 5.4` for editing (the code is the same).
- Install a LuaLS client extension (for example "Lua" by sumneko, `sumneko.lua`). The engine does not install it.

### Hot reload from the external editor

A script saved in VS Code is picked up by the running project. The loose-file watcher polls the project's script
modules every 0.25 s and reloads once a changed set has been stable for 0.4 s (see
[lua_api.md](lua_api.md#editor-panel-and-watcher)). It runs in development (the loose source tree). A bootstrap file turns it off unless it sets `reload` to `true`, as
packed distributions do. A rejected reload keeps the previous code and reports the reason in the Lua
project panel and the console.

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

`NOX_EDITOR_SELECT=<node name>` (with the smoke test on) selects that node at frame 40, checks that the selection
outline covers its subtree, and skips the edit checks so the selection stays for a frame dump with
`NOX_FINAL_DUMP_DIR` and `NOX_FINAL_DUMP_FRAMES`. Use the Scene viewport tab: the outline is drawn only when the
Scene tab is the visible one.
