# Standalone packaging

## Current workflow

Development builds use one source asset tree. Distribution is an explicit, separate step:

```powershell
tools\package.cmd -Plan
tools\package.cmd
tools\package.cmd -SkipBuild
tools\package.cmd -Profile engine -SkipBuild
tools\package.cmd -GameScene scenes/my_game.json -Output Package/MyGame -Plan
```

The default profile is `packaging/nox-racing.json`, outputting to `Package/NOX_Racing`.
`packaging/engine.json` selects a small Cornell validation scene and disables the game module.
`-Profile` also accepts a profile JSON path. `-GameScene` replaces the scene list with exactly
one chosen scene; use a dedicated profile for a multi-scene game.

A profile contains `name`, `game`, `scenes`, `include` and `licenses`. `scenes[0]` is the launch
scene. Include only dependencies constructed at runtime that cannot be discovered from data:
engine shaders and editor fonts, or a procedural generator's texture/prop sets. Profiles keep
those exceptions reviewable without baking game-specific dependencies into build logic.
For a Shuto package, create a profile from its selected scene and review its content license
guide. The racing profile's procedural CC0 sets should not be inherited blindly by other games.

The packager:

- Recursively follows scene/config/asset JSON references and glTF/GLB external images/buffers.
- Preserves relative paths, including model-local paths and URI percent escapes.
- Allows only runtime roots/types, filters source/raw directories, and excludes Blender files,
  backup files, PDFs, prompts, compiler output and unrelated scene libraries.
- Ships icon atlases and a reduced runtime icon manifest instead of source PNGs and all exports.
- Changes packaged scene menus and launch presets to the selected scenes. Source configs stay intact.
- Includes required DLLs, available VC runtime files, and profile license/attribution documents.
- Writes `package-manifest.json` with sorted paths, SHA-256 hashes, sizes and total managed bytes.

Missing required assets fail before output is modified. Absolute/out-of-tree dependencies and
junctions/symlinks are rejected. In-repository output must be a child of `Package/`.
OBJ/MTL sidecars are supported without MTL map options. FBX's texture references are opaque to
this tool; explicitly include its texture directory. Runtime-created `generated/` outputs are
regenerated on launch rather than bundled merely because they exist locally.

Reruns remove unchanged files owned by the previous manifest when dependencies disappear.
Unmanaged files and edited stale files are preserved. Current selected configs are refreshed;
back up local package configuration customizations before repackaging. A package made by the
older tool has no manifest. Its obsolete source-identical asset copies can be removed after
hash verification; unmatched files are preserved. Use a fresh output when an exact minimal
package is needed without any legacy or runtime-created files.

## What loose packaging does and does not solve

Each distribution still needs its own selected runtime models/textures/data. Source `.blend`
projects, raw downloads, alternative models, research and captures stay out. Runtime `.glb`
models and JSON stay loose at this stage because the existing loaders consume filesystem paths.
The manifest makes their contribution measurable and provides a dependency list for a future cook.
Filtering is not model conversion, mesh simplification, texture compression, or asset encryption.

## Next step: cooking and content packs

Proceed in this order so storage gains can be measured against the current manifest:

1. **Cook selected assets once.** Cache outputs by source hash, importer settings and cook version
   under an ignored build cache. Reuse outputs across build configurations. Convert authoring
   models to a supported runtime representation and validate materials, animation and collision.
2. **Optimize the largest runtime payloads.** Measure mesh/index data, embedded textures, HDRIs
   and audio separately. Resize/compress textures according to their actual use and quality target.
   Do not enable Draco, meshopt or KTX2/Basis formats until the runtime's corresponding decoders
   and texture upload path are implemented and tested.
3. **Add content packs behind an asset I/O layer.** Model import, images, shaders, fonts, JSON and
   audio currently use separate file readers. A single pack file needs adapters for all of them,
   plus relative dependency resolution and loose-file overrides for the editor. ZIP-extracting
   everything on every launch would recreate the disk duplication this work removes.
4. **Separate engine and game content.** Mount a shared engine pack and one selected game pack.
   Deduplicate immutable payloads by content hash; keep saves, generated files, logs and editor
   writes outside read-only packs. Use a versioned index and explicit compatibility checks.
5. **Measure download size and installed size independently.** A compressed distribution archive
   can reduce transfer size now, but it does not reduce the extracted runtime footprint. Already
   compressed PNG/JPEG/audio data generally has less scope for lossless archive compression than
   text or uncompressed buffers. Benchmark representative manifests before choosing a codec.

No pack/cooking format is introduced in this pass. The reviewed profile and dependency manifest
are the input contract for that next stage.

See the [pack/VFS research](pak_vfs_research.md) for the measured archive comparison and the
proposed runtime integration. It is research only; the current engine still needs loose assets.
