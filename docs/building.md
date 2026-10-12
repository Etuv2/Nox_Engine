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
only. Both Debug and Release binaries find the repository's asset tree using `main.cpp`;
changing a shader, scene or texture does not require recopying it beside the executable.
An ordinary build is a development build. Use [packaging](packaging.md) to create a standalone
distribution. In-source CMake builds are rejected.

## Targets

| Target | Kind | Contents |
| --- | --- | --- |
| `NoxEngineCore` | static library | `src/**/*.cpp` except `main.cpp` and the racing list, plus `include/IMGUI/*.cpp` |
| `NoxRacingLegacy` | static library | the racing sources in `NOX_RACING_LEGACY_SOURCES` (`CMakeLists.txt`); they migrate to project data and Lua over time |
| `Nox_Engine` | executable | `src/main.cpp`, linked with both libraries (whole-archive on MSVC so registries are kept) |
| `nox_tests` | executable | headless tests in `tests/native/*.cpp`, linked with `NoxEngineCore` |
| `nox_cook` | executable | `tools/cook/main.cpp`, headless procedural cook; linked like `Nox_Engine` |

`nox_physfs`, `nox_lua` and `nox_settings` are internal targets: the pinned third-party
libraries (see [Dependencies](#dependencies)) and the shared compile options. Build one target with
`cmake --build --preset vs2022-x64-release --target nox_tests --parallel`.

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

## Legacy Visual Studio project

`Nox_Engine.sln` and `Nox_Engine.vcxproj` build the same `Nox_Engine.exe` without CMake, for
Release|x64 and Debug|x64 (MSBuild, v143, C++20). Output goes to `build/msbuild/x64/<Configuration>/`
and intermediates to `build/msbuild/obj/`. The project compiles the CMake source set, with the pinned
Lua and PhysicsFS sources as per-file settings (Lua as C++, PhysicsFS with the ZIP and directory
archivers only, third-party warnings off). A post-build step copies the runtime DLLs beside the exe,
as CMake does. The Win32 configurations were removed because the build and its libraries are x64 only.

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
avoids launching an older package accidentally. For a custom package, use its generated
`Play.cmd` or `Editor.cmd`. For the engine-only profile, both launchers add `--no-game`.

## Checks

```powershell
powershell -NoProfile -File tests/PackagingTests.ps1
tools\CheckNoRawGL.ps1
tools\CheckCompile.ps1 src\SomeChangedFile.cpp
python tools\CheckVcxprojSources.py
```

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
