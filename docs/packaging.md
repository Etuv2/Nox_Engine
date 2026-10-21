# Standalone packaging

## Workflow

Development builds use one source asset tree. Distribution is an explicit, separate step:

```powershell
tools\package.cmd -Plan
tools\package.cmd
tools\package.cmd -SkipBuild
tools\package.cmd -Profile engine -SkipBuild
tools\package.cmd -Layout loose -SkipBuild -Output Package/NOX_Racing_loose
tools\package.cmd -GameScene scenes/my_game.json -Output Package/MyGame -Plan
```

The default profile is `packaging/nox-racing.json`, outputting to `Package/NOX_Racing`.
`packaging/engine.json` selects a small Cornell validation scene and disables the game module.
`-Profile` also accepts a profile JSON path. `-GameScene` replaces the scene list with exactly one
chosen scene; use a dedicated profile for a multi-scene game.

## Creating a new game

A new project from a template ([engine_overview.md](engine_overview.md#making-a-new-project)) comes with its
packaging profile, `packaging/<name>.json`, so no packaging file is written by hand:

```powershell
build\vs2022-x64\Release\Nox_Engine.exe --new-project my_game --template third_person_3d
tools\package.cmd -Profile my_game -SkipBuild        # Package\my_game\Play.cmd
Package\my_game\Play.cmd                             # runs the packaged game
```

The editor's **Build > Package** (and the toolbar Package button) packages the active project with its own
profile when the open scene belongs to a project; with no project open it packages the default profile
(`nox-racing`), as before. A project without a profile is reported instead of packaged with someone else's.

The profile's `project` names `projects/<name>/project.json`, its main scene may be a project scene
(`projects/<name>/scenes/main.json`), and `include` lists the files that Lua loads by path. The packager
follows scene and descriptor references but cannot see a path that a script builds or passes to
`ctx:instantiate`, so such files go in `include` (the third-person template lists its coin prefab there).
Projects must live under `projects/` in the checkout: the engine reads content through the checkout's asset root.

`tests/NewProjectTests.ps1` creates each template, runs it, packages `third_person_3d` and runs the package.

## Layout

The default layout is `packed` (contract C5 and C2):

- `engine.pak`: engine-owned runtime content listed in `packaging/engine_owned.json`
  (shaders, editor fonts, the logo, editor defaults).
- `game.pak`: the profile's selected project content, its scene closure, and per-distribution config
  overrides (`config/config.json` with the packaged first scene and scene list, launch presets, the reduced
  icon manifest). It is always present; the engine-only profile's is small.
- `nox_bootstrap.json`: format 1, `mode: packed`, pack list, `content_version` (sha256 of the pack
  indexes), `user_dir`. `descriptor` is the profile's `project` (`projects/nox-racing/project.json` for the racing profile); it records
  the packaged project; the scene's `"project"` key (or `--project`) selects the game module.
- Loose files: `Nox_Engine.exe`, the runtime DLLs, the VC runtime when available, `licenses/`,
  `Play.cmd`, `Editor.cmd`, and `package-manifest.json`. There are no loose asset trees.

`-Layout loose` keeps the earlier copy-tree output for comparisons. It is not used for distribution.
The engine reads packed builds through AssetIO and the bootstrap ([asset I/O](asset_io.md)).
`tools/TestPackagedRun.ps1` runs a package from a copied install outside the checkout and checks
the mounts, that no checkout path is opened, and that the install's file hashes are unchanged.

## Profiles and ownership

A profile contains `name`, `game`, `scenes`, `include`, `licenses` and optionally `project`.
`scenes[0]` is the launch scene. `include` names only game content that the graph cannot discover.
Profiles must not name engine-owned paths; the build fails if they do. A file reached by both
closures has one owner: engine-owned paths go to `engine.pak`, everything else to `game.pak`.

`project` points at `projects/<name>/project.json` (contract C4). `entry`, `script_roots` and `data_roots`
are project-relative. `script_roots` add every `.lua` file below each root, and `data_roots` add their runtime
files. `scenes` and `assets` are root-logical paths (for example `scenes/nfs_sprint.json`). The entry file must
exist. The bootstrap's `descriptor` is the profile's `project`.

The racing project is `projects/nox-racing`: its scenes list (the four NFS scenes), the R33 vehicle data
(`data/vehicles/r33.json`, extracted from DriveMode defaults and the scene's handling/camera values), the Lua entry
`scripts/main.lua`, the HUD config, fonts, the UI icon manifest, the declared road networks, `input.json` and
`launch_presets.json`. The racing scenes (`scenes/nfs_*.json`, `scenes/shuto_*.json`) carry the `"project"` key, so
the packaged `Nox_Engine.exe` runs this project's Lua module for them. The `project` key is a scene reference,
so the packager follows it into the project's content.

## Dependency graph

Edges come from:

- Registered schema keys in `packaging/dependency_rules.json`. `config/config.json` follows only
  `iconPath`, `text_font`, `first_scene` and `scenes[].path`. Other keys (for example
  `environmentPath`, which no runtime reads) are inert.
- Scene documents (`scenes/*.json`) follow `scene_documents.reference_keys` (skybox, nodes at any depth
  `nodes.**.path` and `nodes.**.font_path`, `game.hud`, `window_room_root`). Each procedural node's
  `params` keys are declared per generator name (`street_kit`, `district`: texture_root, decal_root,
  lens_flare_root, window_room_root). Generator outputs (`output`) are not inputs.
- Path-like scene values that no registration claims are listed as warnings and are not packed.
  Add a real input to the registration rather than relying on the scan.
- Other JSON documents without a registration use a generic reference scan.
- glTF/GLB buffers and images (external URIs), OBJ/MTL sidecars.
- Shader `#include` directives, resolved relative to the including file as `src/ShaderLoader.h` does.
- Project descriptor entries: script roots, data roots, assets, scenes, the input map (`input`, default
  `input.json`), fonts, the icon manifest, declared `roads` and the project's `launch_presets.json`.
- The scene `"project"` key (`projects/<name>/project.json`).

Every selected file keeps a reason, shown by `-Plan` with its pack. Excluded: other projects, tests,
authoring/raw directories (`src/`, `raw/`, `originals/`, Blender files), research, tools and captures.
A reference whose spelling differs from the stored name fails the build, because packs are
case-sensitive while loose Windows folders are not.

## Cook cache and pack writer

`tools/NoxPackWriter.cs` (compiled through `Add-Type`) writes each pack:

- Entries are sorted by ordinal name, with timestamps fixed at 1980-01-01 00:00, no extra fields,
  no data descriptors, no ZIP64 and ASCII names.
- Each entry is Deflate (Optimal) only when it saves at least max(64 bytes, 1%); otherwise it is stored.
- The writer builds a temporary file, reads its central directory back, inflates every entry and
  checks sizes, CRCs and SHA-256 against the plan, then replaces the target.
- A pack whose planned index equals the existing central directory is not rewritten.

Cache: `build/cook-cache/` (git-ignored) holds, per entry, a `.meta` file and, for Deflate, a `.bin`
payload. The key is `SHA-256(SHA-256(source bytes) | platform | policy version | tool version)`. The
policy version is `adaptive-deflate-optimal-v1` and the tool version is `nox-pack-writer-1`. Build
configuration and profile are not part of the key, so Debug, Release, profiles and output folders share
entries. Each entry depends only on its own bytes; the transitive dependency set decides which entries
a pack contains, not how an entry is cooked. A cache entry whose payload hash or metadata does not
match is discarded and cooked again, with a warning. Changed inputs never append to an archive: the
pack is rebuilt from its planned entries.

Only supported runtime formats are cooked this way. No mesh or texture conversion is performed, and
Draco, meshopt, KTX2 and Basis are not enabled.

## Manifest and reruns

`package-manifest.json` (version 2) lists the loose files, both packs with each entry's uncompressed
and stored sizes, method, SHA-256 and reason. Reruns remove unchanged files owned by the previous
manifest that are no longer produced. Unmanaged files and edited stale files are preserved and
reported. Current configs are refreshed. Launchers and the bootstrap are rewritten only when their
text changes. A package made by an older tool has no manifest; its source-identical asset copies
are migrated as before. Use a fresh output for an exact minimal package.

## Verification

```powershell
powershell -NoProfile -File tests\PackagingTests.ps1
```

The fixture tree under `tests/fixtures/packaging/tree` covers closure and reasons, the registered config
keys, shader includes, Lua roots, unsafe, missing and case-mismatched references, ownership, the
byte-identical rebuild in a second output folder, the cache reuse, the no-op rerun, corrupt cache
recovery, dependency removal, the changed-input rebuild and manifest-owned cleanup. The suite needs no
compiled engine or downloaded assets.

## Limits

- Authored models, textures and HDRs are not converted, resized or compressed. Their storage is only
  reduced by the deflate decision.
- FBX sidecars are opaque; list the texture directory in the profile's `include`.
- Scene keys are registered for the racing scenes and the fixture; other scene keys (for example Shuto scenes' materials and environment blocks) are reported as unclaimed until registered.
- Procedural streets are cooked before packing (contract C10, `src/cook/`, `nox_cook`). Each result is written to
  `generated/cooked/<generator>-<key>/`, and the packager adds those directories to `game.pak`. `-SkipCook` skips
  the cook step and relies on results already present. A procedural node without a cooked result is logged with
  the `nox_cook` hint and skipped at load (see [building.md](building.md)).
- Workflow changes (`.github/workflows/packaging.yml` path filters for `tools/NoxPackWriter.cs`) belong
  to the lead.
