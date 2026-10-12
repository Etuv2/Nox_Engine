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

The checked-in `.sln`/`.vcxproj` are a legacy entry point. Their output/intermediate directories
now live under `build/msbuild/`; CMake is the supported, validated build. The legacy project's
source list and old dependency settings are not kept equivalent to CMake automatically.

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
```

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
