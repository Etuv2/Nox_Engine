# Documentation index

Start with [building and validation](building.md), [packaging](packaging.md), and
[repository maintenance](repository_maintenance.md). The root README describes engine features
and usage; these pages own the development and distribution workflows.

The [asset pack/VFS research and measured proposal](pak_vfs_research.md) covers the next
engine storage stage, including loader integration, compression results and cook-cache design.

The [Lua gameplay scripting research](lua_scripting_research.md) covers project-defined systems,
safe ECS access, state-preserving reloads, packaging, and a standalone compatibility probe.

The [agent implementation prompt](engine_implementation_prompt.md) consolidates the research
into coordinated implementation work with ownership and acceptance criteria.

## Engine contracts

- [Engine/game boundary](engine_boundary.md)
- [Rendering hardware interface](rhi.md)
- [Principled shading contract](principled_shading_contract.md)
- [Surfel GI implementation](surfel_gi_gibs_implementation_spec.md)
- [SSGI visibility bitmask](ssgi_v2_visibility_bitmask_spec.md)
- [Physics completion notes](physics_completion_notes.md)
- [Transmission audit](transmission_completion_audit.md)

## Game content and asset pipelines

- [Night Street](nfs2015/README.md): game, assets, settings, evidence and benchmarks
- [Shuto Expressway](shuto/README.md): network, environment, buildings, licenses and route checks
- [Shuto environment brief](shuto_environment_prompt.md)
- Asset-specific provenance and processing instructions stay with the assets:
  [fonts](../fonts/README.md), [UI icons](../textures/ui_icons/README.md),
  [road decals](../textures/road_decals/README.md), [room atlases](../textures/window_rooms/README.md),
  [lens flares](../textures/lens_flares/README.md), [Shuto textures](../textures/shuto/README.md).

## Research and historical evidence

The PDF papers in this directory are research references, not runtime dependencies. They stay
out of builds and packages. `docs/superpowers/` contains local historical plans, not the current
build instructions. Existing curated captures and snapshots remain available; new run output
belongs in ignored output directories, as described in the maintenance guide.
