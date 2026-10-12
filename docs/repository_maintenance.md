# Repository storage and layout

## Ownership

| Path | Role | Included by ordinary builds? |
| --- | --- | --- |
| `src/`, `include/`, `lib/` | Engine source and bundled dependencies | Compile/link input |
| `shaders/`, `config/`, asset roots, `scenes/` | Shared development runtime content | Read in place |
| `models/**/src`, `data/**/raw`, asset provenance | Authoring/import source | No |
| `packaging/` | Per-distribution dependency profiles | Explicit package input |
| `tools/` | Build, launch, packaging and content-pipeline commands | No |
| `scripts/` | Existing renderer/static validation utilities | No |
| `tests/` | Fixture/unit checks | No |
| `docs/` | Current contracts, workflows and research | No |
| `build/` | CMake/MSBuild binaries and compiler intermediates | Generated, ignored |
| `Package/` | Standalone distributions | Generated, ignored |
| `captures/`, `validation_output/`, `EvaluationCaptures/` | Run evidence | No |
| `logs/`, `performance_data/`, `snapshots/`, `output/` | Local diagnostics/saves/exports | No |

Runtime asset roots stay stable because scenes, glTF URIs, generators and editor browsers use
their relative paths. Changing those roots requires an asset migration, not a folder-only rename.
Avoid new root output directories: compiler products go to `build/`, distributions to `Package/`,
and run artifacts to the existing ignored directories. Curated evidence already tracked in Git
is retained; new long-term evidence should be deliberately selected and linked from docs.

## Measure before cleaning

```powershell
tools\MeasureRepository.ps1
tools\Clean.ps1
tools\Clean.ps1 -IdeCache
tools\Clean.ps1 -Apply -IdeCache
```

`Clean.ps1` defaults to reporting. It hashes each candidate build asset against its source and
removes only byte-identical copies from known output trees. Missing/divergent originals are
preserved. Parity-test junctions are preserved and should not be counted as duplicate bytes.
`-IdeCache` requires Visual Studio/compiler processes to be closed; it clears `.vs` and known
legacy compiler products. The current CMake build, authored content, captures, logs and snapshots
are preserved. `-WhatIf` previews deletions even with `-Apply`.

Generated CMake metadata, validation results and performance CSVs have been removed from Git
tracking while staying on disk. Ignore rules now allow source Python scripts and `.github`
workflows, and exclude Python bytecode/virtual environments and local run output consistently.
Ignore rules cannot remove already tracked files by themselves.

## Initial findings (2026-10-12)

The physical baseline was about 24.3 GiB. An initial recursive scan reported 28.3 GiB because it
counted eight parity-test junction targets a second time. Major contributors were captures
(8.5 GiB), authored models (3.4 GiB), Visual Studio caches (3.2 GiB), the CMake build (3.0 GiB),
validation output (1.4 GiB), and old build trees (2.2 GiB combined). Exact duplicate tracked
images were a small part of the total; broad source rewrites would not address the main cause.

Full asset folders were copied after Release builds despite the runtime using source assets.
That step is removed. Legacy MSBuild output now stays under `build/msbuild/`. The cleanup command
and explicit dependency packaging prevent the same copies from accumulating again.

The applied cleanup removed 5,061 verified duplicate/cache files (7.17 GiB), followed by
20 obsolete compiler products (0.29 GiB). Divergent/unmatched files and run evidence were
preserved. Packing Git objects saved another approximately 67 MiB.
The resulting managed Release distributions are about 182.6 MiB for NOX Racing and 34.0 MiB
for the engine-only profile, including DLLs, available VC runtime files and attribution.

The source models, research PDFs, captures and raw datasets remain. Do not delete alternate
exports or similar textures based only on their names; they can have different licenses,
materials or uses. Review large asset histories and deliberate evidence retention separately.

## Git storage

For a local checkout, `git gc --prune=never` packs loose objects without intentionally pruning
unreachable history. It does not remove large assets from history or change commit IDs.
Ignoring/untracking a generated file reduces future snapshots, not existing history.

For new large authored binary assets, consider a separately versioned asset library or Git LFS
with a documented fetch/bootstrap step. Moving existing history into LFS is a coordinated
repository migration and is not part of routine cleanup. Existing ignored model/HDR/CC0 downloads
still need their documented asset setup on a fresh clone.
