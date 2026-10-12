"""Automated placement check for the Shuto building section (no Blender needed).

For every placement in data/shuto/buildings/placements.json (landmarks use their collision ring):
  * ground: the base height (2.9 m) against the ground surface under each footprint corner and the
    centroid. Ground is read from the exported road chunks (ground_gravel; upward cut-and-cover lid slabs at
    y 2.4-3.3 in concrete_cast/tunnel_tiles), the context chunks
    (sidewalk, street_asphalt) and the building site pads recorded in placements.json.
    "void" = no ground triangle under a corner ("void_skirted" when the placement carries a base
    skirt down to y -1.5 at the edge of the exported ground), "floating" = ground more than 0.3 m below the base,
    "buried" = ground more than 1.0 m above the base.
  * water: corners/centroid inside the exported water surface ("in_water" = centroid in water).
  * roads: footprint inside a carriageway band (lanes + shoulders, "on_carriageway") or inside the
    4 m clearance beyond it ("in_clearance"), only where the road surface lies within the building's
    vertical span plus 2 m (decks passing well above a low building are reported separately).
  * local streets: more than 5 % of interior samples on exported street asphalt ("on_street").
  * overlaps: footprint intersection with another placement (> 2 m2 and > 5 % of the smaller),
    excluding parts of one multipart source building.
  * heights: h < 2.5 m, h > 250 m, slenderness h / min(w, d) > 12, or an inferred (untagged)
    height above 60 m.

Usage: python tools/shuto/check_building_placement.py [--out FILE] [--strict]
Writes data/shuto/buildings/placement_check.json by default. --strict exits 1 when any hard
failure (void, floating, in_water, on_carriageway, in_clearance, on_street, overlap) remains.
"""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_buildings import accessor, read  # noqa: E402

BASE = 2.9
CELL = 25.0
HARD = ("void", "floating", "in_water", "on_carriageway", "in_clearance", "on_street", "overlap")


class TriIndex:
    """XZ spatial hash of triangles, returning the max surface height under a point."""

    def __init__(self):
        self.cells = defaultdict(list)
        self.tris = []

    def add(self, tri):
        i = len(self.tris)
        self.tris.append(tri)
        xs, zs = tri[:, 0], tri[:, 2]
        for cx in range(math.floor(xs.min() / CELL), math.floor(xs.max() / CELL) + 1):
            for cz in range(math.floor(zs.min() / CELL), math.floor(zs.max() / CELL) + 1):
                self.cells[(cx, cz)].append(i)

    def height(self, x, z):
        best = None
        for i in self.cells.get((math.floor(x / CELL), math.floor(z / CELL)), ()):
            a, b, c = self.tris[i]
            d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
            if abs(d) < 1e-9:
                continue
            l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
            l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
            l3 = 1 - l1 - l2
            if min(l1, l2, l3) < -1e-6:
                continue
            y = l1 * a[1] + l2 * b[1] + l3 * c[1]
            best = y if best is None else max(best, y)
        return best


def load_surfaces(folders, wanted):
    index = {name: TriIndex() for name in set(wanted.values())}
    for folder in folders:
        for path in sorted((ROOT / folder).glob("*.glb")):
            doc, binary = read(path)
            for mesh in doc.get("meshes", []):
                for prim in mesh["primitives"]:
                    name = doc["materials"][prim["material"]]["name"]
                    kind = wanted.get(name)
                    if not kind:
                        continue
                    pos = accessor(doc, binary, prim["attributes"]["POSITION"]).astype(float)
                    faces = accessor(doc, binary, prim["indices"]).reshape(-1, 3)
                    tris = pos[faces]
                    # Only upward-facing surfaces count as ground/water/street.
                    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
                    up = np.abs(n[:, 1]) > 0.5 * np.linalg.norm(n, axis=1)
                    if kind == "lid":
                        # Cut-and-cover lids: front-facing upward slabs at ground level only.
                        ys = tris[:, :, 1]
                        up = (n[:, 1] > 0.5 * np.linalg.norm(n, axis=1)) & (ys.min(axis=1) > 2.4) & (ys.max(axis=1) < 3.3)
                    for tri in tris[up]:
                        index[kind].add(tri)
    return index


def point_in_ring(x, z, ring):
    inside = False
    j = len(ring) - 1
    for i in range(len(ring)):
        xi, zi = ring[i]
        xj, zj = ring[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi + 1e-12) + xi:
            inside = not inside
        j = i
    return inside


def interior_samples(ring, limit=400):
    xs = [p[0] for p in ring]
    zs = [p[1] for p in ring]
    area = abs(sum(ring[i][0] * ring[i - 1][1] - ring[i - 1][0] * ring[i][1] for i in range(len(ring)))) / 2
    step = max(1.0, math.sqrt(max(area, 1.0) / limit))
    out = []
    x = min(xs) + step / 2
    while x < max(xs):
        z = min(zs) + step / 2
        while z < max(zs):
            if point_in_ring(x, z, ring):
                out.append((x, z))
            z += step
        x += step
    return out, step * step, area


def seg_dist(px, pz, a, b):
    dx, dz = b[0] - a[0], b[2] - a[2]
    l2 = dx * dx + dz * dz
    t = 0 if l2 < 1e-12 else max(0.0, min(1.0, ((px - a[0]) * dx + (pz - a[2]) * dz) / l2))
    return math.hypot(px - a[0] - dx * t, pz - a[2] - dz * t), a[1] + (b[1] - a[1]) * t


def road_segments(net):
    segs = defaultdict(list)
    for cw in net["carriageways"]:
        hw = cw["lanes"] * cw["lane_width"] / 2 + cw.get("shoulder_left", 0) + cw.get("shoulder_right", 0)
        pts = cw["points"]
        for a, b in zip(pts[:-1], pts[1:]):
            seg = (a, b, hw, cw["id"])
            for cx in range(math.floor((min(a[0], b[0]) - 30) / CELL), math.floor((max(a[0], b[0]) + 30) / CELL) + 1):
                for cz in range(math.floor((min(a[2], b[2]) - 30) / CELL), math.floor((max(a[2], b[2]) + 30) / CELL) + 1):
                    segs[(cx, cz)].append(seg)
    return segs


def densify(ring, step=1.5):
    out = []
    for i in range(len(ring)):
        a, b = ring[i], ring[(i + 1) % len(ring)]
        n = max(1, int(math.dist(a, b) / step))
        out.extend((a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n) for k in range(n))
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", default="data/shuto/buildings/placements.json")
    parser.add_argument("--out", default="data/shuto/buildings/placement_check.json")
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args()
    plan = json.loads((ROOT / args.plan).read_text(encoding="utf-8"))
    net = json.loads((ROOT / "data/shuto/network.json").read_text(encoding="utf-8"))
    surf = load_surfaces(["models/shuto/rep_section/render", "models/shuto/buildings/context"],
                         {"ground_gravel": "ground", "sidewalk": "ground", "street_asphalt": "street",
                          "water": "water", "concrete_cast": "lid", "tunnel_tiles": "lid"})
    pads = TriIndex()
    for pad in plan.get("site_pads", []):
        ring = pad["ring"]
        for i in range(1, len(ring) - 1):
            pads.add(np.array([[ring[0][0], pad["top_y"], ring[0][1]], [ring[i][0], pad["top_y"], ring[i][1]],
                               [ring[i + 1][0], pad["top_y"], ring[i + 1][1]]]))
    segs = road_segments(net)
    buildings = plan["buildings"]
    issues = defaultdict(list)
    per_building = {}

    def ground_at(x, z):
        values = [v for v in (surf["ground"].height(x, z), surf["street"].height(x, z), surf["lid"].height(x, z),
                              pads.height(x, z)) if v is not None]
        return max(values) if values else None

    for b in buildings:
        ring = b.get("collision_ring", b["ring"])
        flags = set()
        centroid = (sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring))
        worst_gap = 0.0
        water_corners = 0
        for x, z in list(ring) + [centroid]:
            g = ground_at(x, z)
            if g is None:
                # Skirted bases continue to y -1.5 at the edge of the exported ground (no visible gap).
                flags.add("void_skirted" if b.get("skirt") else "void")
                continue
            if g < BASE - 0.3:
                flags.add("floating")
                worst_gap = max(worst_gap, BASE - g)
            if g > BASE + 1.0:
                flags.add("buried")
            w = surf["water"].height(x, z)
            if w is not None and w >= g - 0.01:
                water_corners += 1
        if surf["water"].height(*centroid) is not None and (ground_at(*centroid) or -99) <= surf["water"].height(*centroid) + 0.01:
            flags.add("in_water")
        elif water_corners:
            flags.add("shoreline")
        top = BASE + b["h"]
        hit = None
        for x, z in densify(ring) + [centroid]:
            for a, c, hw, cid in segs.get((math.floor(x / CELL), math.floor(z / CELL)), ()):
                d, y = seg_dist(x, z, a, c)
                if d > hw + 4.0:
                    continue
                if y > top + 2.0:
                    flags.add("under_deck")
                    continue
                kind = "on_carriageway" if d <= hw else "in_clearance"
                if hit is None or kind == "on_carriageway":
                    hit = (kind, cid, round(hw + 4.0 - d, 2))
        if hit:
            flags.add(hit[0])
        samples, cell_area, area = interior_samples(ring)
        street_hits = sum(1 for x, z in samples if surf["street"].height(x, z) is not None)
        if samples and street_hits / len(samples) > 0.05:
            flags.add("on_street")
        o = b["obb"]
        h_tag = b.get("h_tag")
        if b.get("kind") == "infill_yard":
            pass    # fenced yard: 1.8 m boundary fence, not a building mass
        elif b["h"] < 2.5 or b["h"] > 250 or b["h"] / max(1.0, min(o["w"], o["d"])) > 12 or (h_tag is None and b["h"] > 60):
            flags.add("implausible_height")
        per_building[b["id"]] = {"samples": samples, "cell": cell_area, "area": area,
                                 "bbox": (min(p[0] for p in ring), min(p[1] for p in ring),
                                          max(p[0] for p in ring), max(p[1] for p in ring)), "ring": ring}
        for f in flags:
            issues[f].append({"id": b["id"], "detail": b["detail"], "h": b["h"],
                              **({"road": hit[1], "intrusion_m": hit[2]} if hit and f == hit[0] else {}),
                              **({"gap_m": round(worst_gap, 2)} if f == "floating" else {})})
    # Pairwise footprint overlaps via a coarse hash on bounding boxes.
    grid = defaultdict(list)
    for b in buildings:
        x0, z0, x1, z1 = per_building[b["id"]]["bbox"]
        for cx in range(math.floor(x0 / CELL), math.floor(x1 / CELL) + 1):
            for cz in range(math.floor(z0 / CELL), math.floor(z1 / CELL) + 1):
                grid[(cx, cz)].append(b)
    seen = set()
    for cell, members in grid.items():
        for i, a in enumerate(members):
            for b in members[i + 1:]:
                key = tuple(sorted((a["id"], b["id"])))
                if key in seen or a.get("seed_basis") == b.get("seed_basis"):
                    continue
                seen.add(key)
                pa, pb = per_building[a["id"]], per_building[b["id"]]
                if pa["bbox"][0] > pb["bbox"][2] or pb["bbox"][0] > pa["bbox"][2] or \
                        pa["bbox"][1] > pb["bbox"][3] or pb["bbox"][1] > pa["bbox"][3]:
                    continue
                small, large = (pa, pb) if pa["area"] <= pb["area"] else (pb, pa)
                inside = sum(1 for x, z in small["samples"] if point_in_ring(x, z, large["ring"]))
                overlap = inside * small["cell"]
                if overlap > 2.0 and overlap > 0.05 * small["area"]:
                    nested = [x for x in (a, b) if x.get("podium_inset")]
                    if nested and overlap >= 0.9 * small["area"]:
                        # Tower on a podium footprint, walls inset 0.4 m (intended, informational).
                        issues["nested_tower"].append({"id": nested[0]["id"], "podium": key[1] if nested[0]["id"] == key[0] else key[0]})
                        continue
                    issues["overlap"].append({"id": key[0], "other": key[1], "area_m2": round(overlap, 1),
                                              "fraction_of_smaller": round(overlap / max(small["area"], 1), 3)})
    counts = {k: len(v) for k, v in sorted(issues.items())}
    hard = sum(counts.get(k, 0) for k in HARD)
    report = {"placements": len(buildings), "base_y": BASE, "counts": counts, "hard_failures": hard,
              "notes": "void = no exported ground under a corner (outside the built ground section); void_skirted = same, with a base skirt to y -1.5; "
                       "under_deck = an elevated deck passes more than 2 m above the roof (informational); "
                       "shoreline = some corners on water; nested_tower = tower footprint inset inside its podium; buried = ground > 1 m above base",
              "issues": {k: v for k, v in sorted(issues.items())}}
    out = ROOT / args.out
    out.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print("placements", len(buildings), "hard failures", hard, json.dumps(counts))
    if args.strict and hard:
        sys.exit(1)


if __name__ == "__main__":
    main()
