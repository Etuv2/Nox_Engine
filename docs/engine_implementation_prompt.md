# Agent implementation prompt

Implement the NOX research end to end as production engine functionality. Use coordinated
agents to deliver code, tests, working distributions, measurements, and updated documentation.
Complete the implementation rather than producing another research report or stopping at a
prototype. Work in this repository and preserve unrelated/uncommitted work.

## Read first and establish the baseline

Inspect repository structure, AGENTS.md/AGENT.md, .agents, applicable skills, and conventions.
Read these current contracts and proposals before assigning work. The linked Markdown pages
are under docs/; other paths below are relative to the repository root:

- [Engine/game boundary](engine_boundary.md).
- [Pack/VFS research](pak_vfs_research.md) and its recorded compression results.
- [Lua research](lua_scripting_research.md), especially the NFS car migration section and probe limitations.
- [Building](building.md), [packaging](packaging.md), and [maintenance](repository_maintenance.md).
- [NFS gameplay/vehicle rework](nfs2015/game.md) and the relevant Shuto scene/content guides.
- CMakeLists.txt, CMakePresets.json, tools/Package.ps1, tools/AssetPackaging.ps1,
  packaging profiles, and existing validation tools.

Housekeeping and selected loose-asset packaging already exist; VFS and Lua currently do not.
Verify that state instead of assuming the research prototypes are integrated. Preserve source
asset reuse, out-of-source builds, manifest-owned cleanup, and existing build/run commands.
Measure a fresh storage/package/load/vehicle baseline. Record pre-existing failures separately.

The following directions are already selected: read-only shipping packs, loose editor assets,
project-defined Lua gameplay, and reloads that preserve declared state on success and retain
working code on failure. Resolve routine choices independently. Ask only about consequential
requirements that remain unclear after inspection; do not re-ask these agreed directions.

## Agent organization

Act as the integration lead and explicitly spawn agents. Use the available concurrency limit;
with four slots, run the lead plus three workers. Reuse workers as phases finish. Give each
worker a bounded task, owned files/modules, acceptance criteria, and the message: "You are
not alone in the codebase. Do not revert others' edits; accommodate concurrent changes."

| Owner | Initial assignment | Later assignment |
| --- | --- | --- |
| Lead | Shared API/descriptor contracts; bootstrap/Core/GameModule integration; root CMake/native VS project changes; CI; integrated builds and acceptance. | Integration, source partitioning, regression review, measurements and final delivery. |
| Asset I/O agent | VFS/stream implementation and native loader adapters, with exclusive ownership of assigned loader files. | Extract reusable native vehicle calculations and rig/service operations after loader integration. |
| Content agent | Dependency graph, cook/cache and pack writer; Package/AssetPackaging scripts, profiles and packaging fixtures. | NFS project Lua/data migration and gameplay parity fixtures after the pack pipeline is integrated. |
| Scripting agent | Lua dependency/runtime/bindings, ECS handles and project storage; assigned ComponentManager/scene-lifecycle files. | Transactional reload, play-world restoration, generic editor diagnostics and vehicle bindings. |

Agree interfaces before parallel edits. Keep one writer for shared files such as Core.cpp,
main.cpp, CMakeLists.txt and GameModule.h; workers request lead integration there. Reassign
ownership explicitly at phase transitions. Avoid concurrent builds/package writes to shared
outputs; serialize supported builds or use isolated build directories for bounded tests.
Have workers review another subsystem after implementation, checking connected code and
failure paths. Maintain a compact completed/pending list and continue through all phases.

## 1. Asset I/O and shipping packs

Implement one logical asset service: Exists, ReadAll/ReadText, OpenRead, Stat, List, and relative
resolution. Streams provide 64-bit Read/Seek/Tell/Size; buffers have explicit consumer lifetimes.
Normalize separators and relative segments, reject root escapes/absolute shipped references,
preserve spelling, and detect case collisions. Keep explicit editor OS-file imports separate.

Use pinned PhysicsFS with ZIP support for initial engine.pak/game.pak files: standard ZIP
content, adaptive Deflate/stored entries, disjoint engine/game asset ownership, fixed mount
order, and no extraction on launch. Use stored entries or owned buffers where compressed
seeking would repeatedly decode data. Disable unused archivers/tools; measure dependency cost.
Do not implement a custom Zstd format unless real seek/load measurements justify a follow-up.

Initialize asset I/O before reading launch presets/configuration, using a small physical,
executable-relative project/bootstrap descriptor. EXE/DLL/bootstrap remain physical. Development
reads loose project assets; shipping reads explicit immutable packs. Confine generated-cache
overrides to generated/. Read settings defaults from assets and apply per-project user overrides.
Saves, logs, settings, snapshots and cache writes go to writable user/cache locations; editor
authoring writes target explicit source paths. Do not mount user settings as unrestricted assets.

Migrate every runtime reader: config/scenes/menus, shader includes, render and collision glTF,
STB images/HDR, SDL images/fonts/audio, ImGui fonts, game JSON/maps, browser enumeration, and
procedural output. Share TinyGLTF FsCallbacks between loaders and handle external buffers/images
relative to the virtual model path. Preserve decoder orientation/channel/color behavior and
font/SDL buffer ownership. A package that still needs loose models/fonts is incomplete.

## 2. Dependencies, cooking, packaging and layout

Extend the existing profile/manifest pipeline rather than adding a competing workflow. Replace
generic JSON string scanning with schema-aware/registered dependencies where appropriate. Keep
scene/glTF sidecars, shader includes, generator inputs, Lua modules and script-selected assets
in the graph. Declare script roots and dynamic asset dependencies explicitly; do not infer
computed require()/asset paths with regex. Exclude other projects, tests, authoring/raw assets,
research, tools and captures. Preserve existing runtime paths through explicit project mappings.

Validate the suspected unused config environmentPath/HDR dependency with isolated loading and
scene-save tests before pruning it. Preserve models genuinely referenced by other selected
scenes. Raw authoring files stay out; selected runtime models/data remain necessary content.

Cache compatible cooked/compressed results by source and transitive hashes, settings, platform
and schema/tool version, reusable across Debug/Release and projects. Write packs directly from
selected entries without first staging another complete loose tree. Sort entries, fix archive
timestamps, validate manifests, then atomically replace output. Unchanged inputs perform no
cooking or pack rewrite. Changed builds must not grow endlessly appended archives.

Cache complete procedural results, including geometry/layout, child props/lights, probes,
decals/flares and render extras, not only a generated glTF. Keep generation fallback until the
complete versioned contract works. Begin with supported runtime asset formats; add native
mesh/collision or texture cooking only with matching runtime readers and quality/performance
evidence. Do not enable unsupported Draco/meshopt/KTX2/Basis formats. Keep commands/docs/workflows
coherent, outputs ignored under existing build/Package locations, and source assets read in place.

## 3. Generic Lua gameplay and safe ECS

Build a pinned, matched Lua source dependency with the engine toolchain. Verify the production
version against upstream and the research; the attached 5.4.2 DLL is a compatibility fixture,
not the production default. Prefer static linking and a small C API wrapper initially. Record
version/hash/license and test Debug/Release; avoid duplicate vendor folders per game.

Implement one owned VM per active project/play world through the existing IGameModule boundary.
Select project entry/modules from content, independently of RegisterNoxRacingGame. Load buffers
through asset I/O and a Lua-only module searcher in loose and packed modes. Expose selected
libraries/services; remove default filesystem/native-library loading and base dofile/loadfile
escape paths. Restrict/omit dynamic load. No raw ECS pointers; Lua stays on its owned gameplay/
editor thread, not audio callbacks or render backend threads.

Support ordered project systems, optional entity behaviors, declared properties, and Lua-owned
component schemas such as Health/Quest/Inventory without native enum/pool additions. Resolve
stable authored references to opaque world/epoch + entity ID + incarnation handles. Add native
incarnation tracking without rewriting the whole ECS. Validate every operation after deletion,
ID reuse, Clear and scene replacement. Read values by copy; use safe host operations for native
writes, preserving hierarchy, transform/physics dirtying and temporal history. Snapshot queries
and defer structural commands; validate targets again when applying them.

Protect lifecycle calls with tracebacks and balanced stacks. Handle non-string errors, native
exceptions, Lua longjmp/C++ destructor hazards, bootstrap/OOM paths, instruction budgets and
allocator limits. Fault a failing system and discard its native commands without claiming
arbitrary Lua table mutations were rolled back. Honor Play/Pause/input focus and deterministic
system/event ordering. Physics callbacks must run at actual Step/SingleStep boundaries.

Implement a disposable play-world/service restoration strategy before unrestricted spawning,
destruction or native mutation. Current Stop only restores selected transforms/game JSON.
Stop must restore authored entities/components/physics and release runtime-owned resources;
successful code reload must preserve the running world. Keep these lifecycle actions distinct.

## 4. Transactional state-preserving reload

At a safe boundary, take a bounded, versioned snapshot of declared gameplay/component/camera
state. Preserve integer/finite numeric types and stable references; reject unsupported functions,
userdata, cycles/metatables and oversized data. Load a consistent hashed module set into a fresh
candidate VM and migrate a copy of the snapshot. No shared mutable tables or host side effects
during staging: block controls, spawning, subscriptions, audio, writes and native service creation.

Validate exports, API/schema versions, dependencies and migrations. Failure closes the candidate
and retains working code/state/world. Success swaps callbacks/state/subscriptions atomically at
a frame boundary, releasing old registry/native references before closing the old VM. Do not
rerun initializers that reset gameplay. Explain that upvalues/coroutine stacks are not migrated;
recreate tasks from declared data. Add an editor Reload action and actionable diagnostics first,
then a debounced loose-file watcher. Shipping packs remain read-only with reload disabled by default.

## 5. NFS car and gameplay migration

Follow the research's explicit ownership matrix. A Lua wrapper around existing DriveMode is
scaffolding, not completion. Extract generic native tire/load/integration, suspension, contact,
collision, rig posing and mechanical calculations behind checked per-vehicle services. Keep
supported drivetrain/geometry limits explicit rather than promising arbitrary vehicle support.

Move NFS policies and their game-specific formulas to project Lua: nitrous rules, drift entry/
recovery/assist/velocity swing, reverse controls, shift/kickdown/lockout, burnout/countdown/rev
behavior, input/reset/refuel/recovery, camera modes/framing/FOV/shake, HUD, effect/audio decisions,
race/checkpoint/menu/progression and autopilot policies. Native backends own particle integration,
DSP and rendering. No hidden NFS policy may continue running underneath its Lua replacement.

Move R33 paths/rig references, geometry, fallback dimensions, collision circles, handling/gear/
torque/camera presets into reusable project definitions with scene overrides. Support independent
car instances. Publish generic telemetry and validated bulk controls rather than game-named APIs.
Retire hardcoded game bootstrap/editor/HUD dependencies from the generic engine; explicitly
partition CMake and legacy VS sources. Preserve procedural/interior-atlas scene capabilities
through project declarations/reusable services or cooked content when retiring NoxRacingGame.

Preserve the car's own 240 Hz loop, dt clamping and existing pre/post-integration policy order;
PhysicsEngine currently does not step this car. Sample/latch input appropriately, batch policy
calls, and update camera/HUD/emission at their existing frame rate. Native particle pools tick
once per frame; audio consumes gameplay-thread parameters without invoking Lua. Consolidate
schedulers only after independent parity evidence.

Migrate boost/cooldowns, drift side/blend/timers, gear/direction/shift timers, assist filters,
race progress and camera smoothing on reload. Keep native pose/velocity/contact/suspension/
wheel/accumulator state alive. Validate profile changes; incompatible topology changes require
an explicit Play restart, not a silent reset. Each field has one authoritative owner. Extract
the baseline first, then port nitrous, gearbox/reverse, drift/assists and presentation in small
parity-tested steps, continuing until the full migration is complete.

## Validation and definition of done

Run supported Windows x64 Debug/Release builds, packaging fixtures, CheckNoRawGL and appropriate
existing engine/editor/game/scene tests. Add focused headless tests for risky new contracts and
CI coverage; existing research probes do not prove real integration. Test:

- Loose/packed parity for every reader, relative glTF URIs/includes, enumeration and lifetimes;
  missing/corrupt/truncated packs, short reads, seeks and independent concurrent streams.
- Engine and racing distributions from an unrelated working directory with the checkout and
  loose assets unavailable, no extraction, and immutable installation contents unchanged.
- Reproducible packs, no-op second cook/build, compatible cache reuse, dependency removal and
  preserved unmanaged/divergent files. Lua/profile edits change behavior with unchanged engine hash.
- ECS stale handles/query mutation, missing components, Play/Stop restoration, focus/Pause/
  SingleStep, actual zero/multiple substeps, and multi-instance independence.
- Repeated successful/failed mid-drive reloads, syntax/schema/migration errors, budgets/OOM,
  side-effect rejection, state preservation and cleanup of old subscriptions/VM references.
- Native-versus-Lua vehicle baselines: launch, burnout, reverse, shifting/kickdown/braking,
  nitrous depletion/recharge, brake-tap/handbrake drift, recovery, collision and stacked roads.
  Establish trajectory/timing tolerances from evidence; test multiple frame rates and cars.
- Generic non-racing engine operation without NFS scripts/assets or compiled gameplay dependency.

Measure managed content/EXE/DLL/pack bytes, installed allocation/file count, duplication, clean/
incremental packaging time, cold/warm loading, peak RAM, Lua/GC/call cost, vehicle frame time and
reload peak memory. Report comparable before/after values; do not promise gains from codec or
mock-world results alone. Where hardware/assets block a check, state the exact missing condition
and remaining validation, and complete all other work without claiming the blocked check passed.

Update existing documentation to describe the implemented behavior, commands, contracts,
dependencies and remaining limitations. Save all text as UTF-8 without BOM/ASCII punctuation
and verify encoding. Preserve authored assets, captures, logs, snapshots and unrelated edits;
do not rewrite history or perform broad destructive cleanup. Deliver a concise completion report
with implemented scope, agent contributions, test evidence, measured results and genuine gaps.
