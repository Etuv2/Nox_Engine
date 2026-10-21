# Building and validation

## Supported build

Use Windows x64, Visual Studio 2022 with v143/C++20, and CMake 3.21 or newer.
The required prebuilt import libraries and their matching DLL versions are listed in the
[root README](../README.md#building-with-cmake). Asset downloads are documented by each game's
asset guide; a successful compile does not imply that every optional scene has its assets.

```powershell
tools\build.cmd
tools\build.cmd -Configuration Debug
tools\build.cmd -Reconfigure
```

`tools/Build.ps1` owns configuring and building through `CMakePresets.json`. Direct CMake
commands are equivalent:

```powershell
cmake --preset vs2022-x64
cmake --build --preset vs2022-x64-release --parallel
```

All CMake intermediates and binaries live under `build/vs2022-x64/`. Builds copy runtime DLLs
only. Both Debug and Release binaries find the repository's asset tree through `ProjectBootstrap`;
changing a shader, scene or texture does not require recopying it beside the executable.
An ordinary build is a development build. Use [packaging](packaging.md) to create a standalone
distribution. In-source CMake builds are rejected.

## Targets

| Target | Kind | Contents |
| --- | --- | --- |
| `NoxEngineCore` | static library | `src/**/*.cpp` except `main.cpp`, the procedural and legacy-racing lists and `src/legacy/`, plus `include/IMGUI/*.cpp` |
| `NoxProcedural` | static library | `ProceduralStreetKit`, `NightStreetPacks` and the generator registration in `src/procedural/` (`NOX_PROCEDURAL_SOURCES`). Linked by `nox_cook` and the legacy racing executable only |
| `NoxRacingLegacy` | static library | the compiled racing game in `NOX_RACING_LEGACY_SOURCES` (`GameSession`, `DriveMode`, `RacingHUD`, ...). Only `Nox_RacingLegacy.exe` links it. Option `NOX_BUILD_RACING_LEGACY` |
| `Nox_Engine` | executable | `src/main.cpp`, linked with `NoxEngineCore` only (whole-archive on MSVC). The generic engine: no racing code, no procedural generators |
| `Nox_RacingLegacy` | executable | `src/legacy/main_racing_legacy.cpp`: the same command line as `Nox_Engine.exe`, plus the native racing game registered before the window opens. Option `NOX_BUILD_RACING_LEGACY` (default ON) |
| `nox_cook` | executable | `tools/cook/main.cpp`, headless procedural cook; linked with `NoxProcedural` (see [Cooked procedural results](#cooked-procedural-results)) |
| `nox_tests` | executable | headless tests in `tests/native/*.cpp`, linked with `NoxEngineCore` |

`nox_physfs`, `nox_lua` and `nox_settings` are internal targets: the pinned third-party
libraries (see [Dependencies](#dependencies)) and the shared compile options. Build one target with
`cmake --build --preset vs2022-x64-release --target nox_tests --parallel`.

Options (`cmake -D<option>=<ON|OFF> --preset vs2022-x64` reconfigures):

- `NOX_BUILD_RACING_LEGACY` (default ON): build `NoxRacingLegacy` and `Nox_RacingLegacy.exe`. OFF builds the
  generic engine, `nox_cook` and the tests only.
- `NOX_BUILD_TESTS` (default ON): build `nox_tests`.

Release sizes at the time of the split: `Nox_Engine.exe` 5.48 MB (was 6.03 MB with the racing game linked in),
`Nox_RacingLegacy.exe` 6.03 MB, `nox_cook.exe` 1.0 MB.

## Generic engine and the legacy racing executable

`Nox_Engine.exe` is the engine. It contains no compiled racing game: the racing content runs as project
data and Lua (`projects/nox-racing`), and its procedural streets come from cooked results. Without a project
or a registered factory it runs scenes as a viewer/editor.

`Nox_RacingLegacy.exe` keeps the native `GameSession` for parity comparisons with the Lua project. It takes the
same command line, and `--no-game` makes it behave like the generic engine. The native game starts only on a scene
without a `"project"` key; on a scene with the key the Lua project runs instead. `tools\VehicleParity.ps1` writes
such a copy of each racing scene to `generated/vehicle_parity/`. Its parity run:

```powershell
$env:NOX_DRIVE_AUTOPILOT = "1"; $env:NOX_FIXED_DT = "0.0166666667"
build\vs2022-x64\Release\Nox_RacingLegacy.exe --game --scene generated/vehicle_parity/nfs_sprint.native.json
# [Race] Checkpoint 6/6 at 47.3328 s
# [Race] Finished in 47.3328 s (new record)
```

The legacy executable is CMake-only: `Nox_Engine.vcxproj` builds the generic engine, and the racing files are not
in it.

## Cooked procedural results

The procedural street generators (`street_kit`, `district`) are not compiled into `Nox_Engine.exe`. `nox_cook`
runs them headless and writes each result to `generated/cooked/<generator>-<key>/` with a `result.json`. The key
covers the generator version, the params, the scene's declared `game` keys and the input files. Generate the
results for the scenes that have procedural nodes, from the repository root:

```powershell
build\vs2022-x64\Release\nox_cook.exe --scene scenes/nfs_sprint.json --scene scenes/nfs_night_street.json
```

`--repo <dir>` names the repository explicitly. The cook keeps valid results with the same key and rebuilds the
others. `--force` rebuilds all of them, and `--manifest <file.json>` lists the result directories. Re-run it after
changing a generator, its inputs, or a scene's `game` data that a generator declares. Packaging runs it for the
profile's scenes (`tools\package.cmd`; `-SkipCook` skips it) and packs the results into `game.pak`.

When a scene needs a result that is missing, `Nox_Engine.exe` logs an `ERROR` naming the generator, the node and
the `nox_cook` hint, skips that node and loads the rest of the scene. `Nox_RacingLegacy.exe` generates the geometry
live instead, unless `NOX_PROCEDURAL_LIVE=1` is set (cooked results are then ignored).

## Dependencies

Lua 5.5.1 and PhysicsFS 3.2.0 are vendored under `external/` at pinned archive hashes and are
compiled from source by CMake and by the legacy project. Lua is compiled as C++ so that errors unwind
with C++ exceptions. Upstream versions, hashes, licenses and the patch policy (none) are in
[external/README.md](../external/README.md). The other libraries (SDL2, GLEW, Assimp) are prebuilt
import libraries under `lib/` with their DLLs in the repository root.

## Tests

`nox_tests` covers asset I/O, scripting, ECS handles and the vehicle service. It needs no window or
GPU, and it reads repository assets, so run it from the repository root:

```powershell
build\vs2022-x64\Release\nox_tests.exe
ctest --test-dir build\vs2022-x64 -C Release --output-on-failure
```

`ctest` runs the same executable as one test (`nox_tests`). Use `-C Debug` for the Debug build. If
`ctest` is not on `PATH`, use the copy bundled with Visual Studio (`CMake\bin` under
`Common7\IDE\CommonExtensions\Microsoft`). Configure with `-DNOX_BUILD_TESTS=OFF` to skip the test target.

## Debug builds

`tools\build.cmd -Configuration Debug` (or `cmake --build --preset vs2022-x64-debug --parallel`)
writes to `build/vs2022-x64/Debug/`, separate from Release. Debug and Release use the same asset
tree and the same runtime DLLs. To run a Debug engine for a fixed number of frames:

```powershell
$env:NOX_EXIT_AFTER_FRAMES = 60
build\vs2022-x64\Debug\Nox_Engine.exe --no-game --game --scene scenes/cornell_box.json --window windowed --width 960 --height 540
```

`BuildLocked.ps1` only wraps Release builds. Debug builds on a shared machine should take the same
machine-wide mutex (`Global\NoxBuild`) so they do not contend with Release builds in MSBuild.

## Scene save/load round trip

`NOX_SAVE_ROUNDTRIP=<out.json>` makes the engine write the loaded scene to `<out.json>` and exit. The write happens
at the top of the first `Core::Update`, before the game module and the physics step run, so the file holds the
authored state (a game module that poses cars or other nodes on its first update does not change it). The source
scene file is not changed: the copy goes through the scene serializer, not the authoring target. It is the
engine-mode check for save/load. Run it on a private copy of a scene, then run it again on the output:

```powershell
Copy-Item scenes\car.json scenes\_x_car.json
$env:NOX_FIRST_SCENE = "scenes/_x_car.json"; $env:NOX_SAVE_ROUNDTRIP = "C:\tmp\car_A.json"
.\build\vs2022-x64\Release\Nox_Engine.exe              # load original -> A
Copy-Item C:\tmp\car_A.json scenes\_x_carA.json
$env:NOX_FIRST_SCENE = "scenes/_x_carA.json"; $env:NOX_SAVE_ROUNDTRIP = "C:\tmp\car_B.json"
.\build\vs2022-x64\Release\Nox_Engine.exe              # load A -> B
fc /b C:\tmp\car_A.json C:\tmp\car_B.json              # must report no differences
Remove-Item Env:NOX_FIRST_SCENE, Env:NOX_SAVE_ROUNDTRIP
Remove-Item scenes\_x_*.json
```

With the variable set, the engine exits after its first update, so unset it after use. The headless equivalent is
`SceneRoundTrip.*` in `tests/native/test_scene_roundtrip.cpp`.

## Legacy Visual Studio project

`Nox_Engine.sln` and `Nox_Engine.vcxproj` build the generic `Nox_Engine.exe` without CMake, for
Release|x64 and Debug|x64 (MSBuild, v143, C++20). Output goes to `build/msbuild/x64/<Configuration>/`
and intermediates to `build/msbuild/obj/`. The project compiles the engine sources only (`NoxEngineCore`
plus `src/main.cpp`): no procedural generators and no racing game. The pinned Lua and PhysicsFS sources
are per-file settings (Lua as C++, PhysicsFS with the ZIP and directory archivers only, third-party
warnings off). A post-build step copies the runtime DLLs beside the exe, as CMake does. The Win32
configurations were removed because the build and its libraries are x64 only. The legacy racing
executable has no Visual Studio project; build it with CMake.

```powershell
msbuild Nox_Engine.vcxproj /p:Configuration=Release /p:Platform=x64
```

CMake is the supported, validated build; the project is a second entry point that must stay in
step with it. Source lists are maintained by hand, so after adding, moving or removing a source run:

```powershell
python tools\CheckVcxprojSources.py
```

The check derives the expected sources from `CMakeLists.txt` and fails on any difference in
`ClCompile` or `ClInclude` items, or in their `.filters` entries. New files are registered with
`python tools\add_to_vcxproj.py src\New.cpp src\New.h`. Racing sources are in the "Racing (legacy)" filter.

## Launching

```powershell
tools\run.cmd -Preset editor -Source build
tools\run.cmd -Preset game -Source build
tools\run.cmd -Preset game -Source package
```

`Run.ps1` defaults to the newest build or `Package/NOX_Racing` executable. Explicit `-Source`
avoids launching an older package accidentally. `-Legacy` launches `build\vs2022-x64\<Config>\Nox_RacingLegacy.exe`
with the same arguments (build output only; the package has no legacy exe):

```powershell
tools\run.cmd -Legacy -Scene generated/vehicle_parity/nfs_sprint.native.json -Mode game
``` For a custom package, use its generated
`Play.cmd` or `Editor.cmd`. For the engine-only profile, both launchers add `--no-game`.

## Checks

```powershell
powershell -NoProfile -File tests/PackagingTests.ps1
tools\CheckNoRawGL.ps1
tools\CheckCompile.ps1 src\SomeChangedFile.cpp
tools\CheckGenericExe.ps1
python tools\CheckVcxprojSources.py
python tools\CheckCleanExit.py
```

`CheckCleanExit.py` runs the Release exe for 60 frames in `--game`, `--preset editor` and editor with surfel GI, and
fails on a non-zero exit code or any `[CrashHandler]` line (static-teardown crashes only show up there). Use
`--repeat N` for intermittent exit crashes. `tools/nox_mcp/test_e2e.py` also checks `engine.quit` exit code 0, the
absence of `[CrashHandler]`, and that five A-B-A scene round trips leave `gpu_objects` unchanged.

`CheckGenericExe.ps1` fails when `Nox_Engine.exe` contains racing identifiers (`GameSession`, `RacingHUD`,
`NoxRacing`, ...). It does not build anything.

`CheckVcxprojSources.py` needs only Python 3 and compares the legacy project with `CMakeLists.txt`.

The packaging fixture suite needs only Windows PowerShell and .NET, not a compiled engine or
downloaded assets. It covers dependency closure, GLB JSON parsing, unsafe paths, raw-file
exclusion, stale package content, saves, divergent build copies and junction preservation.
`.github/workflows/packaging.yml` runs it on Windows when packaging/build tooling changes.

Other `scripts/validate_*.py` and `scripts/verify_*.ps1` check specific rendering contracts.
Some need local scenes, an OpenGL 4.6 GPU, or Python packages. Choose checks for the subsystem
being changed; they are not all interchangeable build checks. Shuto content checks live under
`tools/shuto/` and are documented by `docs/shuto/`.

The legacy `scripts/validate_scene_loading_static.py` currently expects `scenes/shibuya.json`,
which is absent from this checkout. Its full default run cannot pass until that fixture is
restored or its separately reviewed scene list is updated.

`BuildLocked.ps1` is the mutex wrapper for concurrent local build users. `-Snapshot <name>`
copies just the executable for long validation runs, sharing the same source assets.
