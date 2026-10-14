# Asset I/O, bootstrap and writable data

Status: implemented on 2026-10-12. Code: `src/assets/`, `src/app/ProjectBootstrap.*`,
`src/app/UserSettings.*`, `src/cook/`, `src/InteriorAtlasManifest.*`. Research and measurements that led
here: [pak_vfs_research.md](pak_vfs_research.md). Packaging workflow: [packaging.md](packaging.md).

Every runtime read goes through one service, `AssetIO` (`src/assets/AssetIO.h`), backed by PhysicsFS 3.2.0
(`external/physfs-3.2.0`, ZIP and directory archivers only). The same logical paths work from a source
checkout (loose mode) and from read-only packs (packed mode). Writes never go through `AssetIO`; each kind of
write has its own physical location (see [Writable data](#writable-data)).

```mermaid
flowchart TD
  A[Nox_Engine.exe starts] --> B{--bootstrap file given?}
  B -- yes --> C[read that file]
  B -- no --> D{nox_bootstrap.json beside the exe?}
  D -- yes --> C
  D -- no --> E{checkout (CMakeLists.txt, shaders/, config/) above the exe?}
  E -- no --> X[fatal: message box, exit 1]
  E -- yes --> F[development: loose root = checkout<br/>cache = root/generated, user = root/build/user]
  C --> G{mode}
  G -- packed --> H[packs mounted at root<br/>cache = LocalAppData/user_dir/cache/generated]
  G -- loose --> I[loose root from bootstrap<br/>cache and user as above]
```

## Logical paths

A logical path is relative, `/`-separated and keeps its authored spelling, for example
`shaders/includes/pbr_common.glsl`, `models/car_r33_rig.glb` or `generated/cooked/...`.
`AssetIO::Normalize` (`AssetIO.cpp`, `SplitAndResolve`) applies these rules before every call:

- `\` is treated as `/`. Empty segments and `.` are removed. `..` removes the previous segment.
- A `..` that would leave the root is an error (`path escapes the asset root`).
- Rejected: NUL and control characters, any `:` (drive letters and URIs), and a leading `/` or `\`.
- Case is not changed. Lookups are case-sensitive in packs and follow the file system in loose mode
  (see [Mounts](#mounts)).
- `ResolveRelative(base, ref)` joins `ref` to the directory of `base` and normalizes the result.
  Project files use it so references stay inside the project tree.
- `Exists("")` and `Stat("")` report the root as a directory.

## Mounts

| Mode | Mounted at root (`""`) | Notes |
| --- | --- | --- |
| Loose | the checkout (`Config::looseRoot`), one mount | Windows is case-insensitive; `CheckSpellingLoose` warns once per path whose spelling differs from disk (in every build, not only debug). |
| Packed | the packs in list order, `PHYSFS_mount(..., append)` | Earlier entries are searched first. Entries are ZIP and case-sensitive. Only the stored spelling resolves. |

The cache is a separate writable directory mounted only under the `generated/` prefix, prepended so its
files win for that prefix (`AssetIO.cpp`, `InitializeLocked`). Packed files under `generated/` (for example
cooked results in `game.pak`) stay visible when the cache does not have them. In development the cache is
`<root>/generated`, which is already visible, so it is not mounted twice (`CacheInsideRootIsNotListedTwice`).
The user directory is never mounted, so user files cannot override shaders or content.

Using `AssetIO` before `Initialize` mounts the current directory loosely and logs once
(`LazyInitIfNeeded`). This is a development safety net, not a supported mode.

## Bootstrap

`ProjectBootstrap::Initialize` (`src/app/ProjectBootstrap.cpp`) runs first in `main()` (`src/main.cpp`), before
launch presets, configuration or scenes are read. The file is read with a plain `std::ifstream`, because it is
the input to AssetIO.

Lookup order: `--bootstrap <file>` (a missing file is an error), else `nox_bootstrap.json` beside the
executable, else development mode.

Fields (`format` must be `1`, otherwise the file is rejected):

| Field | Default | Use |
| --- | --- | --- |
| `format` | required | `1` |
| `mode` | `"packed"` | `"packed"` or `"loose"`. Other values are rejected. |
| `packs` | required in packed mode | Relative file names, resolved against the bootstrap's folder, mounted in this order. Must not be empty. |
| `root` | `"."` | Loose mode only: source root, relative to the bootstrap's folder. |
| `user_dir` | `"NOX/<project>"` (or `NOX/Engine`) | Relative folder under `%LOCALAPPDATA%`. |
| `project` | `""` | Name used for the default `user_dir` and logs. |
| `reload` | `false` | Stored as `reloadEnabled`. Only consumer: `LuaGameModule` watches files when this is true and the mode is loose. |
| `descriptor` | `""` | Parsed and stored; **no code reads it** (see [Known gaps](#known-gaps)). |
| `content_version` | `""` | Parsed and stored; **no code reads it**. The packager writes the hash of the pack indexes. |

Derived paths: user directory `%LOCALAPPDATA%\<user_dir>`, cache `<user_dir>\cache\generated`. Both are
created at startup.

Errors (`ProjectBootstrap::Initialize` returns a message; `main()` shows it in a message box and exits 1):
invalid JSON or `format` other than 1, unknown `mode`, a pack or `user_dir` that is absolute or contains `..`,
no packs in packed mode, an explicit `--bootstrap` file that does not exist, no `%LOCALAPPDATA%`, and no
bootstrap and no checkout above the executable. Pack or cache mount failures inside `AssetIO::Initialize`
produce the message `Asset I/O initialization failed: ...`.

Startup log (`ProjectBootstrap.cpp`): `[Bootstrap] mode=... project=... root=... user=...`, then
`[Bootstrap] mounted <pack>` for each pack. `tools/TestPackagedRun.ps1` checks that each pack name appears in the
run log and that the run does not report loose mode.

## Development mode

Without a bootstrap file, `FindSourceRoot` walks up to six folders from the executable looking for
`CMakeLists.txt`, `shaders/` and `config/` (a loose tree beside the exe also qualifies). Then:

- loose mount of the checkout;
- cache `<root>/generated` (ignored by `.gitignore`, rule `/generated/`);
- user directory `<root>/build/user` (ignored, under `build/`). It is flat: no per-project folder, which
  differs from the `build/user/<project>` wording in the early contract;
- the current directory is set to the checkout, so legacy relative paths keep working. Packed shipping never
  depends on the working directory.

## Writable data

| Kind | Physical place | API | Notes |
| --- | --- | --- | --- |
| Settings with defaults | defaults through AssetIO, overrides in `UserDir` | `UserSettings::LoadJsonWithUserOverride`; `SaveUserOverride` | RFC 7386 merge patch in `settings/<file>`. `Core.cpp` and `MainWindow.cpp` load `config/config.json` with `settings/config.json` as the patch. `SaveUserOverride` (writes only the changed keys, `DiffPatch`) has no caller yet. |
| Whole user files | `UserDir` | `UserSettings::LoadUserJson/SaveUserJson`, `LoadUserText/SaveUserText` | Saves are atomic (temp file, then rename). Examples: `saves/speedwall.json` (`GameSession.cpp`, seeded when missing from the file named by the scene's `game.speedwall_seed`; no shipped scene sets it, and the racing data is `projects/nox-racing/data/speedwall.json`), `settings/editor_settings.json` (`EditorWorkspace.cpp`), `editor_layout.ini` (`Core.cpp` sets ImGui's ini file there, seeded from `config/editor_layout.ini`). |
| Runtime outputs | `UserDir` | `AssetIO::UserOutputPath(name, createParents)` | Relative names go under `UserDir`; absolute names are used as given. Used for performance recordings (`PerformanceRecorder.cpp`), runtime state snapshots (`RuntimeStateManager.cpp`), and benchmark output (`BenchmarkRunner.cpp`, default `captures/benchmark`). `SceneLoader.cpp` resolves its runtime-state files the same way with `ResolveUserPath`. |
| Authoring (scenes, HUD config, launch presets, Lua authoring data) | loose source tree only | `AssetIO::SourcePath(logical)` | `SourcePath` returns nothing in packed mode, so the write is refused. See the refusal points below. |
| Generated output | `CacheDir` | `AssetIO::GeneratedOutputPath(logical "generated/...")` | Creates parent folders. Rejects paths outside `generated/`. |

Authoring refusals in packed mode:

- scene save: `Core.cpp` (`scenes are read-only in a packaged build`) and `SceneLoader.cpp` (`scene authoring is disabled in packed (read-only) builds`);
- HUD config save: `RacingHUD.cpp` returns false;
- launch presets: `EditorWorkspace.cpp` shows the toast `Launch presets are read-only in this build`;
- Lua `nox.save_authoring_json`: `LuaData.cpp` returns `authoring writes are disabled in packed distributions`.
  It also needs an editor context, a path under a declared `authoring_roots` entry, and no reload in progress.
  `LuaProject.cpp` rejects `authoring_roots` that name `generated/`.

Tests: `SourcePathOnlyInLooseMode` (`tests/native/test_asset_io.cpp`) and `tests/native/test_lua_data.cpp`.

## Generated cache and cooked procedural results

Procedural generators (`street_kit`, `district`) produce scene geometry. Their output is cached by
content-addressed identity, under `generated/cooked/<generator>-<key>/` (`src/cook/CookedResults.h`):

```
result.json      format 1, generator, generator_version, params (without "output"), scene game keys,
                 input file hashes, children, reflection probes, render extras, side file list
model.gltf       generated model (+ model.bin and generated textures)
model<suffix>    side files, for example model_layout.json
```

Key (`CookedResults::ComputeKey`): SHA-256 over canonical JSON of `{format, generator, generator_version,
params without output, scene_game subset, inputs}`. `inputs` holds the SHA-256 of every file under the
generator's declared input directories, by logical path (`HashInputs`). Editing a declared input file or
its scene game keys changes the key.

Loading (`SceneLoader.cpp`, `ResolveProceduralNode`):

1. Generator compiled in and `NOX_PROCEDURAL_LIVE` is not `1`: compute the key (this hashes the input
   directories on every load), then `LoadDir`. A result whose key matches is used and its side files are copied
   to the authored output location in the cache when they differ. An invalid result logs a warning and falls
   back to live generation. A missing result generates live.
2. Generator not compiled in (for example a Lua project's generator): `FindUnregistered` looks for a result
   whose generator name, params hash and scene game subset match. Input files are not re-hashed. If none is
   found, loading fails with a message that says to run `nox_cook`.

`NOX_PROCEDURAL_LIVE=1` skips step 1's cached result (`SceneLoader.cpp`). It does not help for step 2.

`LoadDir` checks that the directory name matches generator and key, that `model` exists, and that every side
file exists. `FromJson` validates the structure and recomputes the key.

Writer: `nox_cook` (`tools/cook/main.cpp`; options `--repo`, `--scene`, `--manifest`, `--force`) runs the
registered generators with their output redirected into the result directory and writes `result.json`
atomically. Packaging runs it and packs the result directories into `game.pak`
(`tools/Package.ps1`, around the `nox_cook` call). `nox_cook` links `NoxRacingLegacy`, so it needs the racing
generators compiled in.

Distribution cache: `<LocalAppData>\<user_dir>\cache\generated` (cache overlay, empty at first run). Development
cache: `<root>/generated`.

## Reader migration

Readers that use `AssetIO` (grep for `AssetIO::` under `src/`):

| Area | Files | Entry points |
| --- | --- | --- |
| Startup and configuration | `main.cpp`, `Core.cpp`, `MainWindow.cpp`, `SimulationConfig.h`, `ImGui/RenderingSettingsWindow.cpp` | `ReadText`, `ReadJsonOrNull`, `UserSettings` |
| Shaders | `ShaderLoader.h` (includes resolve relative to the including file) | `ReadText` |
| Images and textures | `Texture.cpp`, `TextureArrayBuilder.cpp`, `UiIconAtlas.cpp`, `WorldMapUI.cpp`, `NightStreetPacks.cpp`, `Skybox.cpp`, `editor/solace/SolaceTheme.cpp` | `LoadImageFile`, `LoadImageFileF`, `ReadAll`, `Exists` |
| Models (glTF) | `Scene.cpp`, `CollisionWorld.cpp`, `assets/AssetIOGltf.cpp` | tinygltf FS callbacks (writes refused) |
| Fonts and audio | `GuiNode.cpp`, `AudioPlayer.h`, `editor/solace/SolaceTheme.cpp` | `OpenRWops` (memory-backed SDL RWops) |
| Scenes and render settings | `SceneLoader.cpp`, `SceneRenderSettings.cpp`, `InteriorAtlasManifest.cpp` | `ReadText`, `ReadJsonOrNull`, `Exists` |
| Racing data | `RoadNetwork.cpp`, `WorldMapUI.cpp`, `NightStreetPacks.cpp`, `RacingHUD.cpp`, `ProceduralStreetKit.cpp` | `ReadJsonOrNull`, `ReadText`, `LoadImageFile`, `Exists`; `SourcePath` for the HUD config save; `GeneratedOutputPath` and `CacheDir` for generated geometry |
| Input | `Input/InputSystem.cpp` | `ReadText` (default `config/input_config.json`) |
| Lua projects | `src/scripting/*`: descriptor, `require` through `ReadText`, `nox.json` (limited to config, data, scenes, script roots and declared road files; 1 MiB), UI fonts and icons, input | `ReadText`, `ReadAll`, `Stat`, `Normalize`, `ResolveRelative` |
| Procedural results | `cook/CookedResults.cpp`, `SceneLoader.cpp` | `Exists`, `ReadText`, `List`, `GeneratedOutputPath` |
| Runtime state and captures | `RuntimeStateManager.cpp`, `PerformanceRecorder.cpp`, `BenchmarkRunner.cpp`, `ImGui/ImGuiWindowManager.cpp` | `UserOutputPath` |

Explicitly physical (not migrated), by design or as a known gap:

- Editor text fields that take an OS path: `ImGui/StateExportWindow.cpp` (`GetStateFileInfo` reads the typed
  path with `std::ifstream`). Loading a state file with an absolute path goes through `SceneLoader::LoadRuntimeState`,
  which uses the path as given. Relative names go to `UserDir`.
- `NOX_TEST_POINTER_SCRIPT=@<file>` (`GameSession.cpp`, `RunPointerScript`) reads a test script from disk.
- Diagnostic output paths given by environment variables: `NOX_VEHICLE_TELEMETRY` and
  `NOX_VEHICLE_POLICY_TRACE` (`DriveMode.cpp`, `fopen "w"`), `NOX_FINAL_DUMP_DIR` (`MainWindow.cpp`),
  `NOX_FRAMEBUFFER_DUMP_*` (`Core.cpp`), and the PNG writers in `BenchmarkRunner.cpp` and `passes/`.
- Writes to `UserDir` through physical paths (`Scene.cpp` load diagnostics, `SceneLoader.cpp` runtime state).
  These are user-data writes, so they are not authoring.

This list comes from grepping `src/` for `std::ifstream`, `std::ofstream`, `fopen` and `fs::exists`. It was not
re-checked call by call, so other physical reads may exist.

## Streams and seeking

`AssetIO::Stream` (`AssetIO.h`) gives independent cursors on one entry:

- `Read(dst, n)` returns the bytes read: 0 at end, -1 on error. It loops until `n` bytes or end of file.
- `Seek(offset)` succeeds for `0..Size()`; past the end it fails. `Tell()`, `Size()` and `Path()` are available.
- After `Shutdown`, open streams report errors (`Kill` in `AssetIO.cpp`); their destructors are safe.

Cost model (`PHYSFS ZIP_seek`, `external/physfs-3.2.0/src/physfs_archiver_zip.c`):

- Stored entries seek directly.
- Deflate entries: a forward seek decodes the skipped bytes. A backward seek restarts decoding from the start of
  the entry. Random access into a Deflate entry therefore costs the whole prefix. Use `ReadAll` or a stored entry.

Packing rules (`packaging/pack_rules.json`, `tools/NoxPackWriter.cs`): `.ogg .mp3 .wav .jpg .jpeg .dds .ktx .ktx2`
are always stored. Everything else is Deflate when it saves at least max(64 B, 1%). Entries are sorted, timestamped
1980-01-01, without extra fields or ZIP64. The writer re-reads and verifies each entry before replacing the target.

`ReadAll` reads the whole entry and returns a shared buffer. Its size comes from PhysicsFS. A short read or
a size mismatch is reported as `truncated entry`.

## Threading

- One global `shared_mutex`. Operations take it shared. `Initialize` and `Shutdown` take it exclusive, so a
  shutdown waits for running operations.
- Each stream has its own mutex. Independent streams do not contend (`ConcurrentIndependentStreams`).
- PhysicsFS guards its own state with an internal mutex (`stateLock` in `physfs.c`).
- `Buffer` is `shared_ptr<const vector<uint8_t>>`. Consumers that keep pointers into it (SDL fonts, RWops,
  ImGui atlases) keep the buffer alive.

## Errors for missing and corrupt packs

- Missing pack: `Initialize` returns false with `pack not found`. Bootstrap then fails (message box, exit 1).
  Test: `MissingPackFailsInitialize`.
- Garbage pack: mounting fails, `Initialize` returns false, and the service stays uninitialized
  (`CorruptAndTruncatedPaksNeverCrash`).
- Truncated pack: mounting may fail. If it succeeds, reads of damaged entries return an error and the process
  does not crash. Tested for three cut points.
- Corrupt compressed data: decoding fails or returns a short entry. Reads report an error. Tested by flipping bytes.
- Missing file: `not found: <path>`. Directory passed to a read: `is a directory`.
- Escapes and absolute paths: rejected by `Normalize` before any mount is touched (`EscapesAndAbsolutesRejectedThroughApi`).

Readers print their own messages. Loaders that fail a read fall back or skip the asset, as before the migration.
This was not re-audited per reader.

## Project selection

Scenes and launch options choose a project, and the project chooses a runtime:

- top-level scene key `"project": "projects/<name>/project.json"` (`Core::ProjectForScene`);
- `--project <descriptor>` on the command line, or `project` in the chosen launch preset (`src/main.cpp`,
  `config/launch_presets.json`). A launch project overrides the scene key.
- `Core::SelectGameModule` (`src/Core.cpp`): no project means the native factory registered by the application.
  Only `Nox_RacingLegacy.exe` registers one (`RegisterNoxRacingGame`, skipped by `--no-game`); `Nox_Engine.exe`
  registers none, so a scene without a project runs no game module there. A project path goes to `GameModules::CreateForProject`,
  which reads the descriptor through AssetIO, checks `format: 1`, and calls the factory registered for its
  `runtime` (`src/GameModule.cpp`; `"lua"` is registered in `main.cpp`).
- If the project has no runnable module, Core logs the reason and runs no game module. It does not fall back to
  the native factory.

Descriptor parsing (`src/scripting/LuaProject.cpp`): `format` 1, `runtime` `"lua"`, `api_version` 1, `entry`
(required), `script_roots` (default `["scripts"]`), `data_roots`, `fonts`, `icons`, `input` (default
`input.json` beside the descriptor), `roads`, `collision` and `authoring_roots`. Project-relative paths resolve
against the descriptor's folder.

Shipped racing: the racing scenes and the racing launch presets (`projects/nox-racing/launch_presets.json`) name
`projects/nox-racing/project.json`, so `Nox_Engine.exe` runs the Lua project for them, packaged or not. The native
game runs only in `Nox_RacingLegacy.exe`, and only on a scene without a project key.

## Verification

Build (CI uses the same presets, `.github/workflows/engine.yml`):

```powershell
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-release --parallel
```

Native tests (run from the repository root; the filter matches substrings of `Suite.Name`):

```powershell
build\vs2022-x64\Release\nox_tests.exe AssetIO
build\vs2022-x64\Release\nox_tests.exe CookedResults
```

Last run on this checkout (2026-10-12, Release build at 17:27): `AssetIO` and `AssetIOReaders` gave
`20 test(s), 0 failed`; `CookedResults` gave `6 test(s), 0 failed`. The Debug configuration and CI were not run
for this document.

Packaged run (copies a package to TEMP, runs it from an unrelated folder, checks mount lines, the absence of the
checkout path in the log, and that the install folder is unchanged):

```powershell
tools\Package.ps1 -SkipBuild
tools\TestPackagedRun.ps1 -Package Package\NOX_Racing -Frames 200
```

Not run for this document (needs a built package). Its header still says the packed path fails until pack reading
exists; the code has that support now.

Packaging fixtures: `tests\PackagingTests.ps1` (no engine needed).

## Known gaps

- Bootstrap `descriptor` and `content_version` are parsed and stored but unused. The shipped racing profile's
  project is therefore not selected from the bootstrap. Either the bootstrap should select the project or the
  file should drop the field.
- `InputManager::SaveConfiguration` writes bindings through `UserSettings::SaveUserText`, but
  `LoadConfiguration` reads only the logical default. No caller of the save path was found, so saved bindings are
  not read back.
- Development user data is flat under `build/user`, not per project.
- The descriptor keys `assets`, `scenes` and `native_extensions` from the early contract are not parsed by
  `LuaProject.cpp`.
- The loose spelling check runs in every build, so case mismatches show up in release development runs too.
- Physical access listed in [Reader migration](#reader-migration) is not fully enumerated.
- Real-content measurements (cold/warm load, peak memory, Deflate seek cost in the renderer) are in
  [pak_vfs_research.md](pak_vfs_research.md) and [packaging.md](packaging.md), not here.
