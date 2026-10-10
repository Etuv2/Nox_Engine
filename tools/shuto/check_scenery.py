"""Validate scenery collision against the carriageway bands (pure Python, stdlib only).

Loads models/shuto/rep_section/scenery_collision/*.glb (binary glTF, node transforms applied)
and data/shuto/network.json, then asserts that no scenery collision triangle lies within any
carriageway band. The band is the centreline +- (lanes * lane_width / 2 + shoulder_left +
shoulder_right + 4 m), per carriageway. Triangles are tested in plan (XZ) by sampling every
edge at 0.5 m and the centroid; walls are vertical, so their plan footprint is a segment.

Usage:
  python -I tools/shuto/check_scenery.py [--col-dir DIR] [--network FILE] [--margin 4.0] [--max-report 40]

Exit code 0 when clean, 1 when offenders exist, 2 on load errors.
"""
import argparse
import glob
import json
import math
import os
import struct
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_COL = os.path.join(ROOT, "models", "shuto", "rep_section", "scenery_collision")
DEFAULT_NET = os.path.join(ROOT, "data", "shuto", "network.json")

COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def read_glb(path):
    """Return (list of triangles as tuples of 3 (x, y, z), node-transform applied) for a GLB."""
    with open(path, "rb") as f:
        data = f.read()
    magic, _ver, _length = struct.unpack("<III", data[:12])
    if magic != 0x46546C67:
        raise ValueError("not a GLB: " + path)
    jlen, jtype = struct.unpack("<II", data[12:20])
    doc = json.loads(data[20:20 + jlen].decode("utf-8"))
    off = 20 + jlen
    blen, _btype = struct.unpack("<II", data[off:off + 8])
    binary = data[off + 8:off + 8 + blen]

    def accessor(idx):
        acc = doc["accessors"][idx]
        bv = doc["bufferViews"][acc["bufferView"]]
        fmt, size = COMP[acc["componentType"]]
        n = NCOMP[acc["type"]]
        stride = bv.get("byteStride", size * n)
        base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
        out = []
        for i in range(acc["count"]):
            o = base + i * stride
            out.append(struct.unpack_from("<" + fmt * n, binary, o))
        return out

    # Node transforms: only translation is expected (Blender writes baked meshes).
    trans = {}
    for ni, node in enumerate(doc.get("nodes", [])):
        if "matrix" in node:
            m = node["matrix"]
            trans[ni] = (m[12], m[13], m[14])
            if any(abs(v) > 1e-6 for v in (m[0] - 1, m[5] - 1, m[10] - 1, m[1], m[2], m[4], m[6], m[8], m[9])):
                raise ValueError("%s: node %d has a non-translation matrix" % (path, ni))
        elif "translation" in node or "rotation" in node or "scale" in node:
            t = node.get("translation", (0, 0, 0))
            r = node.get("rotation", (0, 0, 0, 1))
            s = node.get("scale", (1, 1, 1))
            if any(abs(v) > 1e-6 for v in r[:3]) or any(abs(v - 1) > 1e-6 for v in s):
                raise ValueError("%s: node %d has rotation or scale" % (path, ni))
            trans[ni] = tuple(t)
        else:
            trans[ni] = (0.0, 0.0, 0.0)
    node_of_mesh = {}
    for ni, node in enumerate(doc.get("nodes", [])):
        if "mesh" in node:
            node_of_mesh[node["mesh"]] = ni

    tris = []
    for mi, mesh in enumerate(doc.get("meshes", [])):
        tx, ty, tz = trans.get(node_of_mesh.get(mi, -1), (0.0, 0.0, 0.0))
        for prim in mesh["primitives"]:
            pos = accessor(prim["attributes"]["POSITION"])
            pos = [(p[0] + tx, p[1] + ty, p[2] + tz) for p in pos]
            if "indices" in prim:
                idx = [v[0] for v in accessor(prim["indices"])]
            else:
                idx = list(range(len(pos)))
            for i in range(0, len(idx) - 2, 3):
                tris.append((pos[idx[i]], pos[idx[i + 1]], pos[idx[i + 2]]))
    return tris


class Bands:
    """Carriageway centreline segments with per-carriageway band half-width, hashed in plan."""
    CELL = 20.0

    def __init__(self, net, margin):
        self.cells = defaultdict(list)
        self.count = 0
        self.ids = []
        for cw in net["carriageways"]:
            hw = (cw["lanes"] * cw["lane_width"] / 2.0 + cw.get("shoulder_left", 0.0)
                  + cw.get("shoulder_right", 0.0) + margin)
            pts = cw["points"]
            for a, b in zip(pts[:-1], pts[1:]):
                ax, az, bx, bz = a[0], a[2], b[0], b[2]
                cid = len(self.ids)
                self.ids.append((cw["id"], hw))
                seg = (ax, az, bx, bz, hw, cid)
                i0, k0 = self._cell(min(ax, bx) - hw, min(az, bz) - hw)
                i1, k1 = self._cell(max(ax, bx) + hw, max(az, bz) + hw)
                for i in range(i0, i1 + 1):
                    for k in range(k0, k1 + 1):
                        self.cells[(i, k)].append(seg)
                self.count += 1

    def _cell(self, x, z):
        return (math.floor(x / self.CELL), math.floor(z / self.CELL))

    def hit(self, x, z):
        """Return (carriageway id, distance) of the first band containing (x, z), else None."""
        i, k = self._cell(x, z)
        for di in (-1, 0, 1):
            for dk in (-1, 0, 1):
                for (ax, az, bx, bz, hw, cid) in self.cells.get((i + di, k + dk), ()):
                    dx, dz = bx - ax, bz - az
                    L2 = dx * dx + dz * dz
                    t = 0.0 if L2 <= 1e-12 else max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / L2))
                    qx, qz = ax + dx * t, az + dz * t
                    d = math.hypot(x - qx, z - qz)
                    if d <= hw:
                        return self.ids[cid][0], d
        return None


def sample_points(tri):
    a, b, c = tri
    pts = [(a[0], a[2]), (b[0], b[2]), (c[0], c[2]), ((a[0] + b[0] + c[0]) / 3.0, (a[2] + b[2] + c[2]) / 3.0)]
    for p, q in ((a, b), (b, c), (c, a)):
        L = math.hypot(q[0] - p[0], q[2] - p[2])
        n = int(math.ceil(L / 0.5))
        for i in range(1, n):
            t = i / n
            pts.append((p[0] + (q[0] - p[0]) * t, p[2] + (q[2] - p[2]) * t))
    return pts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--col-dir", default=DEFAULT_COL)
    ap.add_argument("--network", default=DEFAULT_NET)
    ap.add_argument("--margin", type=float, default=4.0)
    ap.add_argument("--max-report", type=int, default=40)
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.col_dir, "*_col.glb")))
    if not files:
        print("no collision GLBs in " + args.col_dir)
        return 2
    with open(args.network, "r", encoding="utf-8") as f:
        net = json.load(f)
    bands = Bands(net, args.margin)
    print("bands: %d segments from %d carriageways, margin %.2f m" % (bands.count, len(net["carriageways"]), args.margin))

    total_tris = 0
    offenders = []
    for path in files:
        try:
            tris = read_glb(path)
        except (ValueError, KeyError, struct.error) as ex:
            print("LOAD ERROR %s: %s" % (path, ex))
            return 2
        total_tris += len(tris)
        for ti, tri in enumerate(tris):
            for (x, z) in sample_points(tri):
                hit = bands.hit(x, z)
                if hit is not None:
                    offenders.append((os.path.basename(path), ti, round(x, 2), round(z, 2), hit[0], round(hit[1], 2),
                                      [tuple(round(v, 2) for v in p) for p in tri]))
                    break
    print("checked %d collision triangles in %d files" % (total_tris, len(files)))
    if not offenders:
        print("PASS: zero scenery collision triangles within any carriageway band")
        return 0
    per_file = defaultdict(int)
    for o in offenders:
        per_file[o[0]] += 1
    print("FAIL: %d offending triangles in %d files" % (len(offenders), len(per_file)))
    for name, n in sorted(per_file.items(), key=lambda kv: -kv[1])[:20]:
        print("  %s: %d" % (name, n))
    for o in offenders[:args.max_report]:
        print("  offender %s tri %d at (x %.2f, z %.2f) in band %s (d %.2f m) verts %s" % o)
    return 1


if __name__ == "__main__":
    sys.exit(main())
