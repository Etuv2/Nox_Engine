# Shuto road dataset format (authoritative)

`data/shuto/network.json` is the single source of truth for the Shuto Expressway map.
Blender geometry (decks, barriers, tunnels, collision) and NOX gameplay data (driving
graph, routing, minimap, respawns, checkpoints, signs) are all generated from it.
Build it with `tools/shuto/build_network.py`; never hand-edit generated outputs.

## World frame

- Units: metres. NOX frame: +X = east, +Y = up, +Z = south (right-handed, Y up).
  A car with heading 0 faces +Z; heading increases toward +X (the car's left).
- Projection: local transverse Mercator centred on the origin below (WGS84).
- Origin: Edobashi JCT, Nihonbashi (lat 35.6843, lon 139.7745), y = 0 at
  Tokyo Peil (TP) mean sea level. Ground in the bay area is approximately TP +2..+5 m.
  `ground_y` gives the assumed street level until a DEM is integrated.
- Scale is 1:1 everywhere. Compression of radials is not applied, and `scale` stays 1.0.

## Top level

```json
{
  "format": "nox-shuto-network",
  "version": 1,
  "reference_date": "2002-12-31",
  "origin": {"lat": 35.6843, "lon": 139.7745, "name": "Edobashi JCT"},
  "ground_y": 3.0,
  "sources": [{"id": "osm", "license": "ODbL-1.0", "attribution": "(c) OpenStreetMap contributors", "extract": "...", "date": "..."}],
  "nodes": [...],
  "carriageways": [...],
  "facilities": [...],
  "excluded": [...]
}
```

## nodes

Topological points where carriageways start, end, merge or diverge.

`{"id": "N000123", "pos": [x, y, z], "kind": "merge|diverge|end|boundary|split"}`

- `boundary` nodes are where the network meets another operator's road (NEXCO and
  others) or the street network at a ramp end. They carry `"exit": {"to": "Tomei Expressway", "operator": "NEXCO Central"}`.

## carriageways

A carriageway is one-way, directed from `from` to `to`, and travels in the
direction of increasing `points` index. Japan drives on the left.

```json
{
  "id": "C1.CW.004",          // stable: <route>.<dir>.<seq>, seq numbered along the route
  "route": "C1",              // route id from docs/shuto/route_manifest.json
  "dir": "CW|CCW|IN|OUT|N|S|E|W", // inbound (toward C1) / outbound for radials
  "kind": "main|ramp_on|ramp_off|jct|pa|toll",
  "from": "N000120", "to": "N000123",
  "lanes": 2, "lane_width": 3.25,
  "shoulder_left": 0.75, "shoulder_right": 0.5,
  "speed_kmh": 50,
  "name_en": "...", "name_ja": "...",
  "points": [[x, y, z], ...],  // centreline, spacing <= 10 m, smoothed
  "bank": [rad, ...],          // optional per point, positive = left side up
  "structure": [{"s0": 0.0, "s1": 412.0, "type": "elevated|bridge|tunnel|underground|cut|at_grade|embankment"}],
  "osm_ways": [123, 456]
}
```

- `s` is the arc length along `points` in metres.
- The lane index counts from the left (lane 0 = leftmost; keep left except to pass).
- The centreline is the centre of the travelled lanes, without shoulders.

## facilities

`{"id": "PA.Shibaura", "type": "pa|toll|ic|jct|landmark", "name_en", "name_ja", "pos": [x, y, z], "carriageways": ["..."], "notes"}`

## excluded

Ways or sections removed because they were not open at the reference date:
`{"osm_ways": [...], "route": "C2", "reason": "opened 2007-12-22 (Yamate Tunnel)"}`.

## Derived NOX file

`data/shuto/nox_road_network.json` uses the `RoadNetwork` JSON with these extensions:
3D node `pos: [x, y, z]`, and per-edge `oneway`, `lanes`, `lane_width` and `carriageway` id.
Each centreline point is a node, and each consecutive pair of points is a directed edge.
Scenes reference it with `game.road_network = {"file": "data/shuto/nox_road_network.json"}`.

## Carriageway ids

Ids are `<route>.<dir>.<seq>` and are stable across rebuilds:

- `data/shuto/id_map.json` maps a key to an id. The key is
  `route|dir|OSM way ids|x0,z0` where the OSM way ids are the carriageway's ways (sorted,
  comma separated) and `x0,z0` is the first centreline point quantised to 25 m. A carriageway
  whose key is already in the map keeps its id.
- A new key takes the next free sequence number for its route and direction. Sequence numbers
  are never reused, even after a carriageway disappears.
- The map is committed with the data; delete an entry only when its id must be retired.
- Without a map (first build) chains are seeded per route and direction in `seq_key` order
  (angle about the route centre for loops, x for Wangan, radius for radials).
