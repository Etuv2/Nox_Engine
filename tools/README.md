# Project commands

The current workflows are [building](../docs/building.md), [packaging](../docs/packaging.md), and
[maintenance](../docs/repository_maintenance.md). Run commands from the repository root.

| Command | Purpose |
| --- | --- |
| `build.cmd` / `Build.ps1` | Configure/build the supported CMake preset |
| `BuildLocked.ps1` | Serialize shared-checkout builds; optional executable snapshot |
| `run.cmd` / `Run.ps1` | Launch a build/package with a named preset |
| `package.cmd` / `Package.ps1` | Plan/stage a standalone distribution from a profile |
| `MeasureRepository.ps1` | Read-only storage report |
| `Clean.ps1` | Preview or apply verified duplicate/cache cleanup |
| `CheckCompile.ps1` | MSVC syntax check of selected source files |
| `CheckNoRawGL.ps1` | Check the rendering API boundary |
| `ui_icons.py` | Export/validate icon sources and atlases |
| `shuto/` | Shuto content generators and validation; see `docs/shuto/` |

`Common.ps1` and `AssetPackaging.ps1` are helpers, not entry points. The editor's Build, Run and
Package actions call the same scripts. Existing rendering checks remain in `scripts/`; the
packaging fixture suite lives in `tests/PackagingTests.ps1`.
