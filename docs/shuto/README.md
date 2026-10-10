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
  -> tools/shuto/scenery_plan.py + blender/build_scenery.py   city, water, trees (OSM footprints + kit)
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

## Validation (representative section, 2026-10-10)

**Round trip** (Blender to NOX, `scenes/shuto_roundtrip.json`) is verified for:
- axes (+X east, text not mirrored);
- 1:1 scale;
- texture tiling via UVs (NOX has no `KHR_texture_transform`);
- normal map and emissive.

**Offline collision check** (`check_collision.py`, section bbox):
- 21,779 lane-centre rays hit their own deck, with 0 missing.
- Deck seams are below 1 mm.
- Barrier gaps over 0.3 m: 0.
- Remaining:
  - 6 height mismatches;
  - 430 "shadowed" samples (an overlapping deck within 1.5 m above, in merge tapers beyond the 25 m zone);
  - 5 lane-wall spots (Gaps 3).

**Scenery check** (`check_scenery.py`): 0 wall triangles inside any carriageway band.

**In-engine scripted traversal** (`traversal.py`): 22 events covering 49 of 50 carriageways in
the box, about 58 km, both directions, every junction connector and ramp. Results:
**20 of 22 events pass**, about 57 km with all checkpoints and
no stuck, fall or recovery incidents (`data/shuto/traversal_rep_section_results.json`). Both failures stop at
lane-wall spots near street level: event 11 (`1-Haneda.OUT.007` street-level on-ramp start) and event 18
(`C1.CW.012`/`014`, at the cut/at-grade transition near (-1093, 2375)). `C1.CCW.016` is not covered
(it has too little length inside the box). Coverage runs use a 100 km/h autopilot cap
(`NOX_AUTOPILOT_MAXKMH`) so that the validation driver takes the routed branch at diverges.

**Main sprint** (Ginza to Rainbow Bridge, 4.9 km, full speed, final build): finishes in 166.9 s.
Height goes from -13 m (tunnel) to 52 m (bridge deck), with stacked checkpoints. It logs **3
stuck incidents** at the Shiodome tunnel exit (`C1.CW.018`, s = 350-440, around (-1100, 5, 2390)).
There the car ends up beside the median barrier next to the opposite carriageway, and the
autopilot skip-ahead recovers it. In this stretch the data marks `tunnel` while the solved profile
already climbs above street level. On the previous height solve the sprint ran with 0 incidents.
The traversal events over the same carriageway (9 and 19) pass at 100 km/h. Open: Gaps 3.

**Performance** (3440x1440 borderless, High, RTX 3080 / 5800X, autopilot drive):
- GPU frame median 4.99 ms, p95 6.53 ms; the 165 Hz budget is 6.06 ms.
- Largest passes: G-buffer 1.6 ms, lighting 0.68, SSR 0.51, TAA 0.48.
- 474 sodium spot lights, clustered.

**Load and memory:** scene load about 13 s; peak working set about 2.0 GB.

- **Scene content:** 110 GLBs and about 1.2M render triangles (roads 0.67M, scenery 0.54M). Collision: 221k road and 77k scenery triangles.
- **Precision:** the section lies within 6 km of the origin, so there is no measurable float jitter. Far radials (up to about 35 km) will need origin rebasing (Gaps 9).

Captures:
- `captures/shuto/rep_v3/` (drive)
- `captures/shuto/rep_v2/aerial2.png` (skyline)
- `captures/shuto/lightfix/` (light bug before and after)
- `captures/shuto/roundtrip/`

## Activities

Implemented:
- free roam over the whole section;
- **Shibaura PA place**: arrival message, rest, minimap marker;
- one point-to-point sprint (Ginza to Rainbow Bridge), generated from waypoint positions resolved on the graph, with stable 3D checkpoints, timing, results, retry, return to free roam and saved best time (`config/speedwall.json`).

Traversal events are validation-only (`scenes/_shuto_traversal_rep_section.json`).

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
   - There are no lane-level connections; merges are carriageway-level.
   - Bank is from curvature only.
   - `refine_heights.py` now guarantees 0 grade violations network-wide (6 % main, 8 % ramps). 127 crossing pairs remain below 7 m deck-to-deck, and 208 overlapping merge samples differ by more than 0.25 m (see `refine_report.json`). Vertical-curve abruptness is smoothed but not proven within 2 % per 10 m.
   - Elevation is inferred from OSM layers plus pins (Rainbow Bridge 52 m, Bay Bridge 55 m), not surveyed.
   - Tsurumi Tsubasa height is unverified.
   - There is no terrain DEM; ground is flat at TP+3 m.
3. **Geometry.**
   - Shiodome tunnel exit (`C1.CW.018`): structure ranges say tunnel while the refined profile is above ground, and the median clipping beside `C1.CCW.024` lets a fast car end up beside the barrier (see Validation).
   - 5 lane-wall spots remain in the section (`check_collision.py`): at street-level ramp starts and cut/at-grade transitions (`1-Haneda.OUT.007`, `C1.CW.015`, `C1.CW.018`) and at the Shibaura loop (`1-Haneda.IN.001`, `11.IN.001`). Two of them stop the traversal driver.
   - PA lot roads are not connected to the graph, so they are not generated. Shibaura PA is a place marker, not a drivable lot yet.
   - Structure detail is modular and simple: decks, parapets, piers, joints, open cuts, covered tunnels and lamps. There are no tunnel portals, toll booths, noise screens (optional flag), gantry variety or landmark bridge superstructures (Rainbow Bridge towers and cables are not modelled).
4. **Signs.**
   - 27 panels are composed deterministically in the section, with 4 branches lacking a verified destination name.
   - English text is used only where OSM has `name:en`.
   - The era-appropriate (pre-2017) route marker shield shapes are an approximation.
   - Distances come from the graph, not from real sign inventories.
5. **Traffic.** Not implemented: no AI traffic, lane following or density control.
6. **Streaming / LOD.**
   - The whole section loads at once (about 13 s).
   - There is no chunk streaming, collision activation by distance, `_lod1`, or hardware instancing.
   - The p95 frame time exceeds the 165 Hz budget.
7. **Lighting and weather.**
   - Sodium lamps are thinned to about 470 lights. There are no lamp shadows, sign illumination is only a flat emissive, and lamp heads are simple.
   - No weather or time-of-day behaviour is specific to Shuto (the engine's rain and wetness are available but untuned here).
8. **Scenery.**
   - 89 % of OSM buildings lack heights, so the skyline is coarse.
   - Water edges are stair-stepped at 4 m.
   - Window lighting is block-based.
   - No street-level road network is built besides ramp ends. Street driving is not supported.
9. **Large world.** No origin rebasing yet. It is needed beyond about 10 km from Nihonbashi (Yokohama, Saitama).
10. **Verification.**
    - Route dates are not checked against official dated Shutoko maps.
    - Driving footage was not used.
    - Manual driving was not done in this milestone; only scripted traversal and captures.
