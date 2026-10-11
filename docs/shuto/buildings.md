# Shuto Japanese architecture delivery

The editable kit and placed district are in `models/shuto/buildings/`. Open
`japanese_architecture_kit.blend` for the procedural kit, `placed_buildings.blend`
for individually named geographic placements, and `source_asian.blend` /
`source_lowpoly.blend` for normalized supplied buildings. Blender 4.3.2 was used.
The new playable scene is `scenes/shuto_buildings.json`; the established road
scene and original supplied files are preserved.

## Retained brief

- Metres, 1:1 road geography, reference date 2002-12-31.
- Stylized NFS 2015 night direction using the existing High rendering settings.
- RTX 3080 / Ryzen 7 5800X, 3440 x 1440, target 165 Hz (6.06 ms per frame).
- Ginza, Shiodome, Hamamatsucho/Shibaura and the Daiba approach. Fuji TV is
  included just beyond the built road box for its skyline silhouette.
- The remaining road-graph-only network has no new placements in this delivery.

## Kit, editing and export

`kit_index.json` records 16 project-authored building prototypes and four
components: AC condenser, water tank, shop sign and window bay. It covers compact
residential buildings, balcony apartments, slabs, apartment towers, offices,
retail corners/frontages, commercial mid-rises, workshops, warehouses and
waterfront facilities. Dentsu and Fuji TV provide recognizable custom silhouettes.
Each prototype has three separate LOD GLBs and a closed box collision proxy.

The main .blend opens with LOD0 arranged in a gallery. Object mesh coordinates
keep the base-center pivot; gallery object locations are for presentation only.
LOD1, LOD2 and collision objects are hidden in the viewport and named explicitly.
Collections and objects carry building IDs, floor pitches and LOD metadata.
LOD0 building and component collections are marked as Blender assets, with
collection instance offsets that remove their gallery translation when placed.
Generated coincident mesh vertices are welded for editing while corner UV seams
are preserved. Edit the Python specifications for
repeatable variations, or edit the meshes and nodes directly in Blender.

Near geometry uses fixed 1.6 x 2.0 m window panes, 14 cm recess returns, sash
frames/mullions, apartment railings/slabs, commercial signs/canopies and roof
equipment. The first nine floors receive geometric panes; upper floors use
coherent facade patches. Floor pitches are 3.3 m residential, 3.6 m commercial
and 5.0 m industrial. Dentsu uses its 48-floor height. Tall rectangular blocks
use floor-aligned setbacks. Mid/far buildings use shared facade materials and
simple silhouettes. No independent X/Y/Z scaling is used to fit source meshes.

The section uses fixed proximity tiers (50 m / 110 m / 350 m from roads) baked into
chunk exports. These are not runtime camera-distance LOD switches. NOX currently
does not automatically switch the kit's LOD files or instance a Blender linked
mesh on the GPU. Select a kit LOD when importing; implement distance selection
before deploying the entire network. Chunking and shared external texture URIs
reduce resources, while individually editable objects remain in the .blend.
There are 233 full-detail placements within 50 m, 377 with lighter window frames,
parallax panes and balcony slabs to 110 m, 1,453 facade-patch buildings to 350 m,
and 3,164 background silhouettes. Upper-storey patches remain in both window tiers.

All GLBs use glTF Y-up, applied transforms and relative image paths to one
shared `textures/` folder. Blender is Z-up: (NOX x, y, z) -> (x, -z, y).
The placed buildings use the existing ground datum y=2.9 m. Chunk size is 500 m.
The kit origin is ground-level base center. Source meshes retain their original
UVs and useful materials; source image copies are limited to 2048 pixels.

## Materials and parallax rooms

The source `textures/shuto/README.md` and `manifest.json` were read before UV
assignment. Original atlas files remain untouched. `material_sources.json`
records exact facade crops with edge padding, mirror-derived repeating surface
tiles, edge-continuity measurements, source hashes, and deterministic sign text.
Facade style UVs map 16 bays / 12 storeys and preserve documented image orientation.
Generated patches remain middle/far artwork; close walls use geometry. Surface
mirror derivatives have matching edge pixels, but their mirrored motifs remain
visible at close range. They are base color, not measured PBR maps.

Concrete/roof materials use roughness around 0.8; steel uses metallic 1 and
roughness 0.38; sash frames use metallic 0.7; glazing uses roughness 0.14-0.18.
Walls are non-emissive. Mid/far facade emission uses the existing authored
window-only masks. Close-window room lighting is selected by stable pane seeds,
with apartment/office/shop style and lighting metadata. Plain industrial glass
is non-emissive. No new image generation was needed.

The three fictional signs have exact Japanese and English lettering rendered
with Pillow and licensed Noto Sans JP / Barlow Condensed fonts. Font licenses
are in `fonts/noto/OFL-NotoSansCJK.txt` and `fonts/licenses/Barlow-OFL.txt`.
Sign UV U increases toward screen-right when viewed from the outward normal.
The validator checks this using the exported tangent basis; the text is not
mirrored on outward-facing signs.

`src/SceneLoader.cpp` now loads an imported scene's explicit
`"window_room_root": "textures/window_rooms"`. The existing
`NightStreetPacks::BuildInteriorAtlas` extracts six rooms with five faces each
using `atlas_manifest.json` into a 30-layer texture array with mipmaps. The
existing renderer ray-box mapping samples those faces. Furniture remains
painted on room surfaces and has no independent geometry/depth.

Each close pane has `material.extras.nox.interior`, pane half-size [0.8, 1.0],
depth 4.5 m (6 m shop), style, seed, lighting and blinds. Its UVs occupy one
4x4 grid tile with inset 0.004 and extent 0.242. Export Blender tangents unchanged:
NOX expects the derived bitangent to point up in room/image space. A wrong
tangent sign causes per-pixel room seed changes. The validator checks pane-center
stability, UVs and tangent handedness. Room materials appear as simple emissive
glass in Blender; actual room appearance is validated in the NOX captures.
The same room fields are stored as material custom properties in the .blend
files so edited exports retain the NOX integration.

GLB vertex records are indexed without changing positions, materials or UV/normal
seams. `vertex_optimization.json` records the measured vertex and file-size
reduction. Closed footprint proxies use explicit floor-down and roof-up normals;
box proxy normals are checked against their bounds. Original custom source
shading normals are retained and checked for finite unit length.

## Placement and historical accuracy

`data/shuto/buildings/placements.json` has stable original OSM IDs, SHA256-based
variation seeds, chunk ownership, original footprints, heights, detail tier,
date-tag status and exclusions. Multipart OSM relations have deterministic
geometry-hash suffixes, so each physical part has a unique ID; shared source-ID
seeds keep parts of one building coherent. The export index records the realized geometry.
Updating the road map requires rerunning preparation and clearance validation.
The planner reserves lane/shoulder widths plus a 4 m clearance band and 1.5 m
building-detail slack. Balconies and canopies project at most 1.1 m.

Known post-2002 towers, including Toranomon Hills, World City Towers, Shibaura
Island towers, Shiodome City Center and the Nippon Television Tower, are excluded.
Their 2002 construction sites are not reconstructed. Undated current OSM
footprints and inferred heights remain historical approximations, explicitly
marked in the data. District density and use follow the cached geography; this
is not a surveyed historical building-by-building reconstruction. Court yards
and elevated site terrain are not separately surveyed. Collision proxies are
conservative ground-to-roof footprint prisms, not traversable building interiors.

Dentsu's curved footprint and 48-storey profile are based on the cached OSM
footprint and its 2002 completion: [Dentsu history](https://www.group.dentsu.com/en/about-us/history/2001-1.html),
[building data](https://www.ibecs.or.jp/jsbd/X/index.htm).
Fuji TV was rebuilt from the supplied waterfront reference after visual review.
Its narrow taller office wing, wider lower media wing, six glazed bridges,
four-shaft mast clusters, supported offset sphere, expressed aluminum facade
grid, atrium and stepped lower studios are modeled in `fuji_tv.py`. The sphere
has radial smooth normals and a separate observation-window band. LOD0/1/2
retain its major proportions while reducing facade, mast and sphere detail.
The estimated 304 x 104 m campus envelope includes the extended lower complex
beyond the cached main-building OSM ring; a separate conservative collision
rectangle records that extent. This width/depth is a photo-based estimate, not
a survey. The campus anchor is offset 16 m toward the waterfront from the
main-footprint OSM center to keep its entire envelope clear of the adjacent
planned Bayshore carriageways. The modeled height is 123.45 m and sphere diameter is 32 m.
See [Fuji TV building](https://www.fujitv.com/about/about-our-building/).
Both are stylized silhouette approximations, not exact architectural replicas.
Fuji TV's wall and window panels share edges instead of placing glass overlays
over solid wall faces; this avoids depth-buffer fighting at skyline distances.
The skyline capture extends beyond the built ground section and shows the existing
HDR environment behind the campus. Its surrounding streets and waterfront terrain
are not reconstructed in this delivery.

## Source provenance and redistribution

The original attachments already existed at `models/shuto/src/originals/`.
Their bytes matched the attachment paths. `source_audit.json` records SHA256,
embedded license declarations, transforms, dimensions, mesh organization,
topology, UV ranges, material nodes and image data before adaptation. Original
inspection renders are in `captures/shuto/buildings/sources/`.

- Asian Themed Low Poly Night City Buildings, 99.Miles, CC BY 4.0:
  [source](https://sketchfab.com/3d-models/asian-themed-low-poly-night-city-buildings-9f0343aff4814b758dc6e905aba5b5e0).
  Nineteen buildings retained; seven flare meshes excluded. Uniform scale 1.
- Low-poly City Buildings, smooth998, CC BY 4.0:
  [source](https://sketchfab.com/3d-models/low-poly-city-buildings-e0209ac5bb684d2d85e5ade96c92d2ff).
  Twenty-one buildings retained; ground and degenerate faces removed, including
  faces collapsing during float-coordinate recentering.
  Uniform scale 160 restores plausible floors to the very small original units.
  Western crowns are generic reference/background architecture, not Tokyo landmarks.
- Japanese Building | CC0, Lit For 20:
  [source](https://sketchfab.com/3d-models/japanese-building-cc0-4bde4dd51a3f4cacabc22324a7d0c62c).
  The embedded declaration is Sketchfab Standard, not CC0. Its 18 mesh parts
  are normalized into one editable local template, scale 1. Parent transforms
  are captured before detaching so the resulting footprint is about 12 x 16 m.
  The credit card is excluded. `source_japanese.blend` and `restricted_local/`
  are supplied locally, excluded from the redistributable kit archive and
  geographic scene. Do not redistribute the standalone derivative under CC0.

Source scales are uniform visual floor-pitch calibrations, not surveyed source
units. CC BY derivative changes include base pivots, names, cleanup, LODs,
collision proxies, material conversion and image resizing. Retain these credits,
source links and [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
The original source pages were not accessible during the current audit; embedded
metadata and the repository's previous page audit are retained as evidence.
See [Sketchfab terms](https://sketchfab.com/licenses) for Standard restrictions.
OpenStreetMap-derived placement data requires contributor attribution and ODbL.

## Reproduce

Run from the repository root. Python needs numpy and Pillow. Blender must have
the bundled glTF importer/exporter. Preserve the source originals and texture pack.

```powershell
$blender = 'C:\Program Files (x86)\Steam\steamapps\common\Blender\blender.exe'
& $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/audit_building_sources.py
python tools/shuto/prepare_buildings.py
& $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/build_architecture.py -- --mode probe
python tools/shuto/validate_buildings.py --probe
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Build.ps1 -Configuration Release
# Run scenes/shuto_building_probe.json in NOX before bulk export.
& $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/adapt_building_sources.py
& $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/build_architecture.py -- --mode kit
& $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/build_architecture.py -- --mode section
& $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/build_architecture.py -- --mode context
& $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/finish_building_blends.py
# Only needed to migrate files produced before the sign UV correction:
# & $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/fix_building_signs.py
python tools/shuto/compose_building_scene.py
python tools/shuto/optimize_building_glbs.py
python tools/shuto/validate_buildings.py
python tools/shuto/check_scenery.py --col-dir models/shuto/buildings/collision
powershell -NoProfile -ExecutionPolicy Bypass -File tools/shuto/benchmark_buildings.ps1 -Mode capture
$env:NOX_BENCHMARK_REPEAT = '3'
powershell -NoProfile -ExecutionPolicy Bypass -File tools/shuto/benchmark_buildings.ps1 -Mode timing
python tools/shuto/compose_building_baseline.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/shuto/benchmark_buildings.ps1 -Mode timing -Scene data/shuto/buildings/road_baseline_scene.json -Output captures/shuto/buildings/road_baseline -LogPrefix building_baseline
Remove-Item Env:NOX_BENCHMARK_REPEAT
python tools/shuto/compose_building_kit_check.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/shuto/benchmark_buildings.ps1 -Mode capture -Scene data/shuto/buildings/asset_import_scene.json -Output captures/shuto/buildings/kit_import -Width 1600 -Height 1000 -LogPrefix building_kit
python tools/shuto/report_buildings.py
python tools/shuto/validate_building_text.py
python tools/shuto/package_buildings.py
```

For an edited kit collection, use the following instead of the procedural
generator. It exports the collection with its gallery offset removed, preserves
material custom properties and writes relative shared image URIs. Use LOD1/2
collection names to export the other levels. It does not overwrite the .blend.

```powershell
& $blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/export_edited_building.py -- --blend models/shuto/buildings/japanese_architecture_kit.blend --collection residential_compact_LOD0 --output models/shuto/buildings/kit/residential_compact_edited.glb
python tools/shuto/validate_buildings.py
```

The benchmark helper indexes new exports and validates them before launching
NOX. Use `-SkipOptimization` to benchmark already prepared assets without that
preparation step. Timing runs above use three repetitions and capture runs use
one. The road-only comparison keeps the same roads, lamps, rendering settings
and camera shots while removing building/context geometry and its collision.

The context export retains existing water, local streets, foliage and container
props while removing old building geometry, so the new scene avoids duplicate
facades. Container roofs and collision are retained in dedicated context exports.
The new scene keeps the existing roads, player, road graph, lamps and gameplay.
All generated scripts, metadata and documentation are UTF-8 without BOM.

## Validation evidence and limits

- Release engine build: `logs/buildings_engine_build.log`.
- Original asset audit: `data/shuto/buildings/source_audit.json`.
- Probe geometry/pane check: `probe_validation.json` (2,362 triangles, 84 panes).
- Actual NOX probe: `captures/shuto/buildings/probe_nox/`, four views including
  two close room angles; six rooms / 30 faces confirmed by engine log.
- Bulk GLB geometry/resources: `data/shuto/buildings/export_validation.json`.
- Footprint collision clearances: `logs/building_clearances.log`.
- Actual district captures and timings: `captures/shuto/buildings/section_capture/`
  and `section_timing/`; consolidated results in `import_results.json`.

The full composed section is imported and representative views are inspected.
Every independent kit/source LOD and proxy is also imported in the dedicated
kit-check scene, including the local restricted Japanese template. Five selected
standalone views are visually inspected; import success alone does not establish
every asset's appearance. The edited-collection exporter is checked with the
compact residential prototype.
Four static camera shots do not prove a sustained 165 Hz full-course drive or
replace a full traversal regression. Performance results distinguish CPU frame
time from GPU pass time and must not be described as a 165 Hz certification.
Mid/far facade tint variants are consolidated into 12 shared runtime facade
styles to reduce material groups. The editable kit retains its palette, while
near geometry retains separate concrete surface assignments. `material_optimization.json`
records the final group count. `geometry_statistics.json` records triangles,
proximity tiers, panes, file sizes and the 1.3 million city scenery triangle budget.
The 165 Hz target is not met in the measured representative section; consult
`import_results.json` for final wall-time and GPU-time results. The full-detail
kit is retained; the placed section uses the lighter 50-110 m parallax tier.

The final three-repeat High run at 3440 x 1440 on RTX 3080 / Ryzen 5800X measured
11.25 ms median frame time, 13.60 ms p95 and 6.45 ms median GPU pass time across
2,172 recorded frames. The road-only comparison measured 8.85 ms median and
10.97 ms p95, so it also exceeds the 6.06 ms target before the city is added.
These fixed-camera runs do not identify every engine bottleneck. Reaching 165 Hz
requires further engine/rendering work and a measured full-course drive.

The composed city has 1,034,820 building triangles plus 114,640 retained context
triangles, 49 building chunks, 64 context chunks and 54 new collision files.
The delivery includes 5,227 individually editable placements, 16 new building
prototypes, four components and 41 adapted source buildings (40 redistributable).
There are 401 validated GLBs across all LODs, probes and context; 397 are included
in the archive after excluding the four Standard-licensed standalone files.

