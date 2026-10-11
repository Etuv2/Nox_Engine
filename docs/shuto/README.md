# Shuto Expressway environment (NOX)

Status: **milestone 1 of a multi-stage delivery. The network is NOT complete.** The whole
classic-era network exists as a connected, routable 3D road graph. One representative section is
built, drivable and validated: Ginza open cut, Shiodome tunnel, Hamazakibashi stacked junction,
Shibaura, Route 11 and Rainbow Bridge. Everything else is listed under [Gaps](#gaps). The
per-route coverage table is generated: [route_checklist.md](route_checklist.md).

Brief: [../shuto_environment_prompt.md](../shuto_environment_prompt.md).

## Agreed requirements (2026-10-10)

| Topic | Decision |
|---|---|
| Reference era | Classic network at **2002-12-31**: before C2 west / Yamate Tunnel, Shinagawa, K7 and Tokyo Gate Bridge. Sections open then and closed later (Yaesu route, C1 Edobashi/Gofukubashi exits) are in scope. |
| Scale | 1:1 everywhere (no compression). Frame: +X east, +Y up, +Z south, origin Nihonbashi ([network_format.md](network_format.md)). |
| Fidelity / hardware | Existing NOX **High** tier at 3440x1440 on RTX 3080 / Ryzen 5800X (the NFS 2015 night look). The city is stylised and selectively detailed. |
| Activities | **Free roam + parking-area meets first**; sprints use the existing event framework. |
| Delivery | Milestone first: manifest + whole-network graph + engine support + Blender pipeline + one validated section, with gaps documented. |
| Multiplayer | Not requested. |

## Architecture

One authoritative dataset drives every output:

```
OSM (ODbL) + route manifest
  -> tools/shuto/build_network.py   data/shuto/network.json (carriageways, nodes, facilities, ids)
  -> tools/shuto/refine_heights.py  global height solve (grades, merges, clearances)
  -> tools/shuto/blender/build_roads.py   render + collision GLB chunks, lamps (per section bbox)
  -> tools/shuto/make_facade_units.py + scenery_plan.py + blender/build_scenery.py   city, water, trees (OSM footprints + kit; facade units from textures/shuto/derived/facade_units)
  -> tools/shuto/compose_signs.py   navigation signs from the graph (Noto Sans JP, sign blanks)
  -> tools/shuto/make_activities.py events + parking-area places from the graph
  -> tools/shuto/build_scene.py     scenes/shuto_<section>.json
  -> validation: check_collision.py, check_scenery.py, traversal.py (NOX autopilot), checklist.py
```

Engine changes are generic. Game data feeds them and there are no Shuto-specific code paths:

- **`CollisionWorld`** (`src/CollisionWorld.*`): static triangle collision from GLB files, with a
  BVH per mesh. Triangles are classified as ground, wall or ceiling. It provides raycast,
  ground-below and sphere resolve, and supports per-mesh enable for chunk activation.
- **`DriveMode` surface mode** (when `game.collision` is set): the car has a real height.
  - Per-wheel ground raycasts start just above the current deck, so stacked decks resolve correctly.
  - The body follows grade and bank, and slopes add gravity along and across the road.
  - The car can go airborne and land.
  - Walls are collision wall triangles tested at body height.
  - A camera ray keeps the chase camera out of walls and tunnel ceilings.
  - Recovery checks the lane for ground, otherwise uses the last safe pose.
  - Scenes without collision behave exactly as before. The NFS sprint finishes identically.
- **`RoadNetwork`**:
  - 3D nodes, one-way edges, lanes and lane width, carriageway ids.
  - `traffic_side` (Japan: left) and `band_walls: false`.
  - File-backed networks with a uniform-grid index, holding 79k nodes.
  - Height-aware closest-point queries, directed routing and lane placement.
- **`GameSession`**:
  - Positions may be `[x, y, z]`. Gates, triggers, respawns, route targets and HUD markers use height (`height_tolerance`).
  - Parking-area **places**: a minimap marker, an arrival message, and rest (refills N2O).
  - Traversal hooks: `NOX_AUTOPILOT_EVENT`, `NOX_EXIT_ON_FINISH`, `NOX_AUTOPILOT_MAXKMH`, plus stuck and fall reporting.
- **`RacingHUD`**: the minimap culls distant edges, dims roads on other levels, and has a parking marker.
- **Engine bug fixed:** `LightNode::SetLight` reset every punctual light to the world origin. Car
  headlights, district streetlights and all scene spot and point lights now light where they are
  authored. Note: `scenes/nfs_sprint.json` still stores zero positions for the car lights, which
  were saved while the bug was active. `build_scene.py` overrides them for Shuto scenes.

## Reproduce

```
python -I tools/shuto/build_network.py              # needs network access (Overpass), caches data/shuto/raw/
python -I tools/shuto/refine_heights.py
"<blender>" -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/build_roads.py -- --network data/shuto/network.json --out models/shuto/rep_section --bbox=-2600,1500,0,6000
python -I tools/shuto/scenery_plan.py ... ; blender ... build_scenery.py     (see scenery.md)
python -I tools/shuto/compose_signs.py --bbox=-2600,1500,0,6000 --name rep_section
python -I tools/shuto/make_activities.py --section rep_section
python -I tools/shuto/build_scene.py --section rep_section
python -I tools/shuto/check_collision.py --collision-dir models/shuto/rep_section/collision --network data/shuto/network.json --bbox=-2600,1500,0,6000
python -I tools/shuto/check_scenery.py ...
python -I tools/shuto/traversal.py make --section rep_section && python -I tools/shuto/traversal.py run --section rep_section
python -I tools/shuto/checklist.py
Nox_Engine.exe --game --scene scenes/shuto_rep_section.json --quality high
```

Debug helpers: `tools/shuto/debug_spot.py <section> x z [r] [--log run.log]` (collision plot +
centrelines + nearby carriageways), `tools/shuto/plot_collision.py`.

Editable sources (git-ignored, under `models/`):
- `models/shuto/src/rep_section.blend`, `rep_section_scenery.blend`, `buildings_kit.blend`, `test_site.blend`
- originals in `models/shuto/src/originals/`

Outputs:
- `models/shuto/rep_section/{render,collision,scenery,scenery_collision}/<cx>_<cz>*.glb`
- `signs.glb`, `lamps.json`, `chunks.json`, `scenery.json`

## Data and licences

Full record: [assets_and_licenses.md](assets_and_licenses.md).

- **Road geometry, buildings and water:** OpenStreetMap, ODbL 1.0, "(c) OpenStreetMap contributors". This is a derived database, so share-alike applies to it.
- **Route manifest:** opening and closure dates come from Wikipedia-derived pages. shutoko.jp returned HTTP 429, so they are not verified against official dated maps ([route_manifest.md](route_manifest.md)).
- **Building GLBs:**
  - `asian_themed_low_poly_night_city_buildings` (99.Miles) and `low-poly_city_buildings` (smooth998) are CC-BY-4.0, and attribution is required.
  - **`japanese_building__cc0` is "Sketchfab Standard" in its metadata, not CC0.** It is kept out of all shipped scenery until the licence is verified.
- **Texture kit:** `textures/shuto` is AI-generated base colour and artwork, used as delivered. Atlas cells are extracted with 8 px padding and fringe dilation (`tools/shuto/extract_patches.py`). Seamless derivatives exist for the tunnel, galvanised steel and corrugated metal. Billboard art is used for scenery only.
- **Fonts:** Noto Sans JP Bold and Medium, SIL OFL 1.1 (`fonts/noto/`).

## Validation (representative section, 2026-10-11)

**Round trip** (Blender to NOX, `scenes/shuto_roundtrip.json`) is verified for:
- axes (+X east, text not mirrored);
- 1:1 scale;
- texture tiling via UVs (NOX has no `KHR_texture_transform`);
- normal map and emissive.

**Offline collision check** (`check_collision.py --bbox=-2600,1500,0,6000`):
- 22,192 lane-centre rays hit their own deck, with 0 missing and 6 height mismatches.
- 81 "shadowed" samples (an overlapping deck within 1.5 m above, in merge tapers).
- 1 lane-wall spot (`11.IN.001` at the Shibaura loop, a data overlap).
- 9 short barrier gaps of 2-5 m where gore paving starts and ends.
- 2 of 549 lamps not resolved to a support.
- 606 render z-fight candidate pairs: same-level overlapping decks, lifted 1.5 cm render-only.
- Deck seams are below 1 mm.

**Scenery check** (`check_scenery.py`): 0 wall triangles inside any carriageway band.

**In-engine scripted traversal** (`traversal.py run`, full scene with scenery, final build):
**22 of 22 events pass.** They cover 49 of 50 carriageways in the box, about 58 km, both
directions, every junction connector and ramp. All checkpoints are hit, with 0 stuck, fall or
recovery incidents (`data/shuto/traversal_rep_section_results.json`). Coverage runs use a 100 km/h
autopilot cap (`NOX_AUTOPILOT_MAXKMH`). `C1.CCW.016` is not covered because too little of it lies
inside the box.

**Main sprint** (Ginza to Rainbow Bridge, 4.9 km, full speed): finishes in 151.7 s with 0 incidents.
Height goes from -13 m (tunnel) to 52 m (bridge deck), with stacked checkpoints.

**NFS street regression:** `nfs_sprint` autopilot finishes with 0 incidents. Editor smoke test:
all PASS. `tools/CheckNoRawGL.ps1`: pass.

**Performance** (3440x1440, High, RTX 3080 / 5800X, autopilot drive, summed pass GPU time):
- Median 5.43 ms, p95 6.87 ms; the 165 Hz budget is 6.06 ms.
- Largest passes: G-buffer 1.81 ms, lighting 0.83, SSR 0.51, TAA 0.48, bloom 0.46.
- 549 sodium spot lights, clustered.
- Note: `--window borderless` snaps to the native display size, so measure at full size with
  `--window windowed --width 3440 --height 1440`.

**World map:** `WorldMapUI::Draw` CPU averages 0.18-0.25 ms (max 1.1 ms) at 720p-3440.

**Load and memory:** load plus 300 frames takes about 27 s; peak working set is about 2.7 GB.
- **Scene content:** roads 0.69M render triangles and 224k collision triangles; scenery 1.05M render triangles and 55k collision triangles. Scenery textures are external, so each is stored and uploaded once (engine-wide shared-image cache in `Scene.cpp`).
- **Precision:** the section lies within 6 km of the origin. Far radials (up to about 35 km) will need origin rebasing.

Captures:
- `captures/shuto/roads_v3/` (merge, diverge, junction)
- `captures/shuto/city_v6/` (street, facade 40 m, expressway, aerial)
- `captures/worldmap/` (1280, 1920, 3440, zoom)
- `captures/ui_icons/final/` (HUD 720/1920/3440, results, editor)
- `captures/ui_icons/` (pause menu, fallback)
- `captures/shuto/lightfix/` (punctual light bug before and after)
- `captures/shuto/roundtrip/`

## Activities

Implemented:
- free roam over the whole section;
- **Shibaura PA place**: arrival message, rest, minimap marker;
- one point-to-point sprint (Ginza to Rainbow Bridge), generated from waypoint positions resolved on the graph, with stable 3D checkpoints, timing, results, retry, return to free roam and saved best time (`config/speedwall.json`).

Traversal events are validation-only (`scenes/_shuto_traversal_rep_section.json`).

UI:
- **World map** (M or pad View; `src/WorldMapUI.*`, baked base `tools/shuto/make_worldmap.py`). It shows glowing expressways with route shields, Tokyo Bay and rivers, district labels, the player, event and parking markers, and a cyan event route preview. Controls: pan, zoom, recentre, and select an event. Enter sets a GPS waypoint, which routes on the minimap.
- **UI icon pack** (`textures/ui_icons`, 128 px atlas via `src/UiIconAtlas.*`, configured in `config/hud.json`). Icons appear on HUD gauges and warnings, minimap and world markers, the pause and results menus, prompts, and the editor toolbar. The previous glyphs and text remain as fallbacks.

Not implemented yet:
- loop time trials;
- checkpoint challenges with time extension;
- speed zones;
- traffic challenges (no AI traffic yet);
- delivery and navigation runs;
- drift zones;
- meet/photo mode beyond the place prompt.

## Gaps

Precise, as of this milestone:

1. **Coverage.** Only the representative section (3.3 % of 784 km of carriageway) has geometry,
   collision, scenery and signs. All other routes exist only as the graph. The same pipeline must
   be run section by section. Route-level numbers are in [route_checklist.md](route_checklist.md).
2. **Data.**
   - Yaesu (Y) and the open S2 section are absent from the OSM extract.
   - Route B's 168 km is inflated by connector extensions and needs review.
   - 11 C1/C2 direction-label conflicts remain.
   - Ramps are sometimes unnamed.
   - Lane counts come from OSM tags.
   - There are no lane-level connections.
   - Bank is from curvature only.
   - `refine_heights.py` guarantees 0 grade violations network-wide. 127 crossing pairs remain below 7 m deck-to-deck and 208 merge samples differ by more than 0.25 m, almost all outside the section.
   - Elevation is inferred from OSM layers plus pins, not surveyed. There is no DEM; ground is flat at TP+3 m.
3. **Geometry.**
   - 1 lane-wall spot (`11.IN.001`, Shibaura loop data overlap).
   - 606 render z-fight candidates between same-level overlapping decks (render-only 1.5 cm lift).
   - 2 lamps whose support the checker cannot resolve.
   - PA lot roads are not connected to the graph and not generated; Shibaura PA is a place marker.
   - There are no toll booths, landmark bridge superstructures (Rainbow Bridge towers and cables) or noise screens (optional flag).
   - Gore paving fills gaps under 1.5 m. Wider gores show a barrier nose without a crash cushion.
4. **Signs.**
   - 27 panels are composed deterministically, with 4 branches lacking a verified destination name.
   - English text is used only where OSM has `name:en`.
   - Pre-2017 route shields are approximated.
   - Distances come from the graph.
5. **Traffic.** Not implemented.
6. **Streaming / LOD.**
   - The whole section loads at once (about 27 s, 2.7 GB).
   - No chunk streaming, collision activation by distance or hardware instancing.
   - The p95 frame time (6.87 ms) exceeds the 165 Hz budget.
7. **Lighting and weather.**
   - No lamp shadows. Sign illumination is a flat emissive.
   - No Shuto-specific weather or time of day.
8. **Scenery** (see [scenery.md](scenery.md)).
   - 12 generated facade styles (wall materials cropped from the supplied patches, window_rooms in lit panes), district zoning, near-range shopfronts, balconies and rooftop detail, OSM city streets with sidewalks, street trees and lamps.
   - Open:
     - most OSM buildings lack heights;
     - water edges are stair-stepped;
     - 37 kit landmarks remain after the aspect filter;
     - lit panes are emissive, not engine interior mapping (that needs a generic engine change);
     - street-level driving is not validated.
9. **Large world.** No origin rebasing yet. It is needed beyond about 10 km from Nihonbashi.
10. **Verification.**
    - Route dates are not checked against official Shutoko maps.
    - No driving footage was used.
    - No manual driving; only scripted traversal and captures.
