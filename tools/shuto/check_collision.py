"""Checks Shuto collision GLBs against the contract network (pure python + optional numpy).

Run: python -I tools/shuto/check_collision.py --collision-dir models/shuto/test_site/collision
                                              --network data/shuto/test_network.json

Checks:
  1. Centreline rays: for every carriageway, lane centres every 2 m. A ray starts at expected deck
     height + 1.5 m and drops; the highest ground (normal y >= 0.6) hit must be within 0.05 m of the
     expected height. Samples where a higher road (other carriageway) lies within the car window
     are reported as shadowed; shadowing outside merge/diverge zones is a failure.
  2. Barrier continuity: samples every 0.25 m along both inner barrier faces. A sample needs a wall
     triangle (normal |y| < 0.6) within 0.2 m, spanning deck+0.2..deck+1.0 m, unless it lies inside
     another carriageway's footprint with |dy| < 1.5 m (the footprint opening rule). Gaps > 0.3 m
     that are not opened fail.
  3. Seam step: shared edges of ground triangles; the largest height difference across an edge.
Exit code 0 when all hard checks pass.
"""
import argparse
import glob
import json
import math
import os
import struct
import sys
from collections import defaultdict

try:
    import numpy as np
except ImportError:  # numpy is optional; the GLB reader then uses plain lists
    np = None

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EPS_BY_KIND = {"main": 0.0, "jct": 0.0, "pa": 0.0, "toll": 0.0, "ramp_on": 0.0, "ramp_off": 0.0}  # as build_roads.py
TUNNEL_TYPES = ("cut", "tunnel", "underground")


# ----------------------------------------------------------------------------------------------
# GLB reading (positions + indices, node transforms applied)
# ----------------------------------------------------------------------------------------------
COMP = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise ValueError("not a GLB: " + path)
    off = 12
    js = None
    binc = b""
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        off += clen
        if ctype == 0x4E4F534A:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:
            binc = chunk
    return js, binc


def accessor_array(js, binc, idx):
    acc = js["accessors"][idx]
    bv = js["bufferViews"][acc["bufferView"]]
    fmt, size = COMP[acc["componentType"]]
    ncomp = NCOMP[acc["type"]]
    count = acc["count"]
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", size * ncomp)
    if np is not None:
        dt = {"f": "<f4", "I": "<u4", "H": "<u2", "B": "u1", "h": "<i2", "b": "i1"}[fmt]
        arr = np.ndarray(shape=(count, ncomp), dtype=dt, buffer=binc, offset=base,
                         strides=(stride, size))
        return arr.astype(np.float64) if ncomp in (1, 3) and fmt == "f" else arr.astype(np.int64) if fmt != "f" else arr
    out = []
    for k in range(count):
        o = base + k * stride
        out.append(struct.unpack_from("<" + fmt * ncomp, binc, o))
    return out


def node_matrix(node):
    if "matrix" in node:
        m = node["matrix"]
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    t = node.get("translation", [0, 0, 0])
    r = node.get("rotation", [0, 0, 0, 1])
    s = node.get("scale", [1, 1, 1])
    x, y, z, w = r
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    R = [[1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)],
         [2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)],
         [2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)]]
    M = [[R[i][j] * s[j] for j in range(3)] + [t[i]] for i in range(3)]
    M.append([0.0, 0.0, 0.0, 1.0])
    return M


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def xform(M, p):
    x, y, z = p
    return (M[0][0] * x + M[0][1] * y + M[0][2] * z + M[0][3],
            M[1][0] * x + M[1][1] * y + M[1][2] * z + M[1][3],
            M[2][0] * x + M[2][1] * y + M[2][2] * z + M[2][3])


def glb_triangles(path, with_material=False):
    """Returns a list of triangles ((x,y,z) x3) in world space; with_material adds the material name."""
    js, binc = read_glb(path)
    tris = []
    ident = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
    scene_nodes = js.get("scenes", [{}])[js.get("scene", 0)].get("nodes", list(range(len(js.get("nodes", [])))))

    def visit(ni, parent):
        node = js["nodes"][ni]
        M = matmul(parent, node_matrix(node))
        if "mesh" in node:
            mesh = js["meshes"][node["mesh"]]
            for prim in mesh["primitives"]:
                if prim.get("mode", 4) != 4:
                    continue
                pos = accessor_array(js, binc, prim["attributes"]["POSITION"])
                if "indices" in prim:
                    ind = accessor_array(js, binc, prim["indices"])
                    ind = [int(v) for v in (ind[:, 0] if np is not None else [t[0] for t in ind])]
                else:
                    ind = list(range(len(pos)))
                pts = []
                for p in pos:
                    q = tuple(float(v) for v in p[:3])
                    pts.append(xform(M, q))
                mat = None
                if with_material and "material" in prim:
                    mat = js["materials"][prim["material"]].get("name")
                for k in range(0, len(ind) - 2, 3):
                    if with_material:
                        tris.append((pts[ind[k]], pts[ind[k + 1]], pts[ind[k + 2]], mat))
                    else:
                        tris.append((pts[ind[k]], pts[ind[k + 1]], pts[ind[k + 2]]))
        for c in node.get("children", []):
            visit(c, M)

    for ni in scene_nodes:
        visit(ni, ident)
    return tris


# ----------------------------------------------------------------------------------------------
# Geometry helpers
# ----------------------------------------------------------------------------------------------
def tri_normal(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    if ln < 1e-12:
        return None
    return (nx / ln, ny / ln, nz / ln)


class Tri2D:
    """Ground/wall triangle with a plan-view hash."""
    __slots__ = ("a", "b", "c", "n", "minx", "maxx", "minz", "maxz", "den")

    def __init__(self, a, b, c, n):
        self.a, self.b, self.c, self.n = a, b, c, n
        self.minx = min(a[0], b[0], c[0])
        self.maxx = max(a[0], b[0], c[0])
        self.minz = min(a[2], b[2], c[2])
        self.maxz = max(a[2], b[2], c[2])
        d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        self.den = d

    def height(self, x, z):
        """Plane height at (x, z); None if the point is outside the triangle (2D)."""
        a, b, c = self.a, self.b, self.c
        if abs(self.den) < 1e-12:
            return None
        l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / self.den
        l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / self.den
        l3 = 1.0 - l1 - l2
        eps = -1e-7
        if l1 < eps or l2 < eps or l3 < eps:
            return None
        return l1 * a[1] + l2 * b[1] + l3 * c[1]


class Hash2D:
    def __init__(self, tris, cell):
        self.cell = cell
        self.map = defaultdict(list)
        for i, t in enumerate(tris):
            i0, i1 = math.floor(t.minx / cell), math.floor(t.maxx / cell)
            k0, k1 = math.floor(t.minz / cell), math.floor(t.maxz / cell)
            for ix in range(i0, i1 + 1):
                for iz in range(k0, k1 + 1):
                    self.map[(ix, iz)].append(i)
        self.tris = tris

    def query(self, x, z):
        return self.map.get((math.floor(x / self.cell), math.floor(z / self.cell)), ())


def ray_ground(ground, hash_g, x, z, y0):
    best = None
    for i in hash_g.query(x, z):
        t = ground[i]
        if x < t.minx - 1e-6 or x > t.maxx + 1e-6 or z < t.minz - 1e-6 or z > t.maxz + 1e-6:
            continue
        h = t.height(x, z)
        if h is None or h > y0 + 1e-6:
            continue
        if best is None or h > best[0]:
            best = (h, i)
    return best


# ----------------------------------------------------------------------------------------------
# Network sampling (same rules as build_roads.py)
# ----------------------------------------------------------------------------------------------
class Sampled:
    def __init__(self, cw, step=1.0):
        pts = cw["points"]
        banks = cw.get("bank") or []
        if len(banks) != len(pts):
            banks = [0.0] * len(pts)
        cum = [0.0]
        for i in range(1, len(pts)):
            cum.append(cum[-1] + math.dist(pts[i - 1], pts[i]))
        total = cum[-1]
        n = max(2, int(math.ceil(total / step)) + 1)
        self.id = cw["id"]
        self.kind = cw.get("kind", "main")
        self.lanes = int(cw.get("lanes", 2))
        self.lw = float(cw.get("lane_width", 3.25))
        self.half_l = self.lanes * self.lw / 2 + float(cw.get("shoulder_left", 0.75))
        self.half_r = self.lanes * self.lw / 2 + float(cw.get("shoulder_right", 0.5))
        self.eps = EPS_BY_KIND.get(self.kind, 0.0)
        self.structure = cw.get("structure", [])
        self.S, self.X, self.Y, self.Z, self.B = [], [], [], [], []
        seg = 0
        for k in range(n):
            t = total * k / (n - 1)
            while seg < len(pts) - 2 and cum[seg + 1] < t:
                seg += 1
            span = cum[seg + 1] - cum[seg]
            f = 0.0 if span <= 1e-9 else min(1.0, max(0.0, (t - cum[seg]) / span))
            a, b = pts[seg], pts[seg + 1]
            self.S.append(t)
            self.X.append(a[0] + (b[0] - a[0]) * f)
            self.Y.append(a[1] + (b[1] - a[1]) * f)
            self.Z.append(a[2] + (b[2] - a[2]) * f)
            self.B.append(banks[seg] + (banks[seg + 1] - banks[seg]) * f)
        self.N = n
        # Same window-averaged tangents as build_roads.py (smoothed_frames, +-4 samples).
        rx, rz = [], []
        for i in range(n):
            a = max(0, i - 1)
            b = min(n - 1, i + 1)
            dx, dz = self.X[b] - self.X[a], self.Z[b] - self.Z[a]
            ln = math.hypot(dx, dz) or 1.0
            rx.append(dx / ln)
            rz.append(dz / ln)
        self.TX, self.TZ = [], []
        for i in range(n):
            a, b = max(0, i - 4), min(n, i + 5)
            dx, dz = sum(rx[a:b]), sum(rz[a:b])
            ln = math.hypot(dx, dz)
            if ln < 1e-9:
                dx, dz, ln = rx[i], rz[i], 1.0
            self.TX.append(dx / ln)
            self.TZ.append(dz / ln)

    def lane_centres(self):
        return [((self.lanes / 2.0) - k - 0.5) * self.lw for k in range(self.lanes)]

    def top_y(self, i, u):
        return self.Y[i] + self.eps + u * math.sin(self.B[i])

    def pos(self, i, u):
        lx, lz = self.TZ[i], -self.TX[i]
        return self.X[i] + u * lx, self.Z[i] + u * lz

    def type_at(self, s):
        for r in self.structure:
            if r["s0"] - 1e-6 <= s < r["s1"] - 1e-6:
                return r["type"]
        return self.structure[-1]["type"] if self.structure else "at_grade"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--collision-dir", required=True)
    ap.add_argument("--network", required=True)
    ap.add_argument("--chunks", default=None, help="chunks.json (optional; default: all *_col.glb in dir)")
    ap.add_argument("--step", type=float, default=2.0)
    ap.add_argument("--bbox", default=None, help="xmin,zmin,xmax,zmax: only check samples inside (shrunk by 40 m)")
    ap.add_argument("--render-dir", default=None, help="render GLB dir (default: <collision-dir>/../render)")
    ap.add_argument("--json-out", default=None, help="write the metrics as JSON (before/after comparisons)")
    args = ap.parse_args()

    cdir = args.collision_dir if os.path.isabs(args.collision_dir) else os.path.join(ROOT, args.collision_dir)
    net_path = args.network if os.path.isabs(args.network) else os.path.join(ROOT, args.network)
    with open(net_path, "r", encoding="utf-8") as f:
        net = json.load(f)

    files = sorted(glob.glob(os.path.join(cdir, "*_col.glb")))
    if not files:
        print("FAIL no collision GLBs in " + cdir)
        return 1

    ground, walls, ceilings = [], [], []
    n_raw = 0
    for path in files:
        for a, b, c in glb_triangles(path):
            n_raw += 1
            n = tri_normal(a, b, c)
            if n is None:
                continue
            t = Tri2D(a, b, c, n)
            if n[1] >= 0.6:
                ground.append(t)
            elif n[1] <= -0.6:
                ceilings.append(t)
            else:
                walls.append(t)
    print("collision files: %d, triangles: %d (ground %d, wall %d, ceiling %d)"
          % (len(files), n_raw, len(ground), len(walls), len(ceilings)))
    hash_g = Hash2D(ground, 10.0)
    hash_w = Hash2D(walls, 5.0)

    sampled = [Sampled(cw) for cw in net["carriageways"] if cw.get("route") != "PA"]
    # Deck profile conformance applied by build_roads.py (same-level overlaps): expected heights follow it.
    off_path = os.path.join(os.path.dirname(os.path.normpath(cdir)), "deck_offsets.json")
    lot_of = {}
    closed_end = {}
    if os.path.exists(off_path):
        offdoc = json.load(open(off_path, encoding="utf-8"))
        closed_end = {}
        for cid, i in offdoc.get("closed_ends", []):
            closed_end.setdefault(cid, []).append(i)
        offs = offdoc.get("offsets", {})
        lot_of = {l["carriageway"]: l for l in offdoc.get("lots", [])}
        n_off = 0
        for sc in sampled:
            for rec in offs.get(sc.id, ()):
                i, dy = rec[0], rec[1]
                if i < sc.N:
                    sc.Y[i] += dy
                    if len(rec) > 2:
                        sc.B[i] = math.asin(max(-0.2, min(0.2, math.sin(sc.B[i]) + rec[2])))
                    n_off += 1
        print("deck offsets applied: %d carriageways, %d samples" % (len(offs), n_off))
    if args.bbox:
        bx0, bz0, bx1, bz1 = [float(v) for v in args.bbox.split(",")]
        bx0, bz0, bx1, bz1 = bx0 + 40.0, bz0 + 40.0, bx1 - 40.0, bz1 - 40.0
    else:
        bx0 = bz0 = -1e18
        bx1 = bz1 = 1e18

    def in_box(x, z):
        return bx0 <= x <= bx1 and bz0 <= z <= bz1
    # Merge/diverge zones (node positions) for shadow classification.
    zone_pts = [(n["pos"][0], n["pos"][2]) for n in net["nodes"] if n.get("kind") in ("merge", "diverge", "split")]

    # Footprint grid of all samples for the opening rule.
    fgrid = defaultdict(list)
    for ri, sc in enumerate(sampled):
        for si in range(sc.N):
            fgrid[(math.floor(sc.X[si] / 12.0), math.floor(sc.Z[si] / 12.0))].append((ri, si))

    def footprint_hits(x, z, y, own):
        best = {}
        cx, cz = math.floor(x / 12.0), math.floor(z / 12.0)
        for ix in (cx - 1, cx, cx + 1):
            for iz in (cz - 1, cz, cz + 1):
                for ri, si in fgrid.get((ix, iz), ()):
                    if ri == own:
                        continue
                    sc = sampled[ri]
                    d2 = (sc.X[si] - x) ** 2 + (sc.Z[si] - z) ** 2
                    if d2 > 196.0:
                        continue
                    if ri not in best or d2 < best[ri][0]:
                        best[ri] = (d2, si)
        for ri, (_, si) in best.items():
            sc = sampled[ri]
            lx, lz = sc.TZ[si], -sc.TX[si]
            lat = (x - sc.X[si]) * lx + (z - sc.Z[si]) * lz
            # Paved gores (build_roads GORE_FILL = 1.5 m): a same-level neighbour within 1.5 m of the
            # deck edge is joined by gore paving, so no barrier is expected there either.
            gore = 1.6 if abs(sc.top_y(si, 0.0) - y) < 0.3 else 0.0
            if lat < -sc.half_r - gore or lat > sc.half_l + gore:
                continue
            if abs(sc.top_y(si, max(-sc.half_r, min(sc.half_l, lat))) - y) < 1.5:
                return True
        return False

    # 1. Centreline rays.
    ok = fail = shadow_ok = shadow_bad = missing = 0
    fails = []
    shadow_list = []
    for sc in sampled:
        for u in sc.lane_centres():
            # Sample positions avoid the exact carriageway ends: a point exactly on the shared end line of
            # two meshes is a degenerate ray (on an edge of both), not a gap.
            s_list = [min(max(k * args.step, 0.25), sc.S[-1] - 0.25) for k in range(int(sc.S[-1] // args.step) + 1)]
            for s in s_list:
                i = min(sc.N - 2, int(math.floor(s)))
                f = s - sc.S[i]
                x0, z0 = sc.pos(i, u)
                x1, z1 = sc.pos(i + 1, u)
                x, z = x0 + (x1 - x0) * f, z0 + (z1 - z0) * f
                if not in_box(x, z):
                    continue
                y_exp = sc.top_y(i, u) + (sc.top_y(i + 1, u) - sc.top_y(i, u)) * f
                best = ray_ground(ground, hash_g, x, z, y_exp + 1.5)
                if best is None:
                    missing += 1
                    fails.append((sc.id, round(s, 1), "no ground", round(y_exp, 3)))
                    continue
                hit = best[0]
                if hit > y_exp + 0.05:
                    near_zone = any(math.hypot(x - zx, z - zz) < 25.0 for zx, zz in zone_pts)
                    if near_zone:
                        shadow_ok += 1
                    else:
                        shadow_bad += 1
                        shadow_list.append((sc.id, round(s, 1), round(hit, 3), round(y_exp, 3)))
                    continue
                if abs(hit - y_exp) <= 0.05:
                    ok += 1
                else:
                    fail += 1
                    fails.append((sc.id, round(s, 1), round(hit, 3), round(y_exp, 3)))
    print("centreline rays: ok=%d  missing=%d  height-mismatch=%d  shadowed-in-merge-zone=%d  shadowed-elsewhere=%d"
          % (ok, missing, fail, shadow_ok, shadow_bad))
    for f in (fails + shadow_list)[:12]:
        print("   sample:", f)

    # 1b. Walls inside driving lanes: any wall triangle within a lane (lateral |u| < lane edge -
    # 0.3 m) between deck+0.25 and deck+2.2 m (the car's wall sphere band) blocks the road.
    lane_walls = []
    for sc in sampled:
        lane_edge = sc.lanes * sc.lw * 0.5 - 0.3
        for s in range(0, int(sc.S[-1]), 2):
            i = min(sc.N - 1, s)
            for u in (-lane_edge, -lane_edge * 0.5, 0.0, lane_edge * 0.5, lane_edge):
                x, z = sc.pos(i, u)
                if not in_box(x, z):
                    continue
                y = sc.top_y(i, u)
                for ti in hash_w.query(x, z):
                    t = walls[ti]
                    if x < t.minx - 0.6 or x > t.maxx + 0.6 or z < t.minz - 0.6 or z > t.maxz + 0.6:
                        continue
                    # Same band as the car (DriveMode): wall sphere bottom +0.25 m, contacts up to +1.6 m;
                    # slivers under 0.15 m tall are not walls (CollisionWorld::minWallHeight).
                    if max(t.a[1], t.b[1], t.c[1]) < y + 0.25 or min(t.a[1], t.b[1], t.c[1]) > y + 1.6:
                        continue
                    if max(t.a[1], t.b[1], t.c[1]) - min(t.a[1], t.b[1], t.c[1]) < 0.15:
                        continue
                    e1 = [t.b[k] - t.a[k] for k in range(3)]
                    e2 = [t.c[k] - t.a[k] for k in range(3)]
                    cr = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
                    longest = max(math.dist(t.a, t.b), math.dist(t.a, t.c), math.dist(t.b, t.c))
                    if longest > 1e-6 and math.sqrt(sum(v * v for v in cr)) / longest < 0.05:
                        continue  # needle sliver (CollisionWorld ignores it too)
                    if any(abs(i - ce) <= 2 for ce in closed_end.get(sc.id, ())):
                        continue   # the wall closing an unbuilt ramp end (build_roads emit_ramp_end_walls)
                    if seg_dist_xz(x, z, t) <= 0.5:
                        lane_walls.append((sc.id, s, round(x, 1), round(y, 2), round(z, 1)))
                        break
    # Collapse to distinct spots (10 m).
    spots = []
    for w in lane_walls:
        if not any(abs(w[2] - q[2]) < 10 and abs(w[4] - q[4]) < 10 and abs(w[3] - q[3]) < 3 for q in spots):
            spots.append(w)
    print("walls inside lanes: samples=%d  distinct spots=%d" % (len(lane_walls), len(spots)))
    for w in spots[:25]:
        print("   lane wall:", w)

    # 2. Barrier continuity along both inner faces.
    checked = opened = covered_n = 0
    gaps = []
    def lot_width(sc, side, s):
        """Parking apron widening (build_roads apply_lots) of this carriageway side at s."""
        lot = lot_of.get(sc.id)
        if lot is None or lot["side"] != side:
            return 0.0
        s0, s1, tp, W = lot["s0"], lot["s1"], lot["taper"], lot["width"]
        if s0 <= s <= s1:
            return W
        if s0 - tp < s < s0:
            return W * (s - (s0 - tp)) / tp
        if s1 < s < s1 + tp:
            return W * ((s1 + tp) - s) / tp
        return 0.0

    for own, sc in enumerate(sampled):
        for side, ue0 in (("L", sc.half_l), ("R", -sc.half_r)):
            run_start = None
            run_len = 0.0
            step_s = 0.25
            m = int(sc.S[-1] / step_s)
            for k in range(m + 1):
                s = k * step_s
                i = min(sc.N - 1, int(round(s)))
                ue = ue0 + (lot_width(sc, side, s) if ue0 > 0 else -lot_width(sc, side, s))
                x, z = sc.pos(i, ue)
                y = sc.top_y(i, ue)
                if not in_box(x, z):
                    continue
                checked += 1
                # The barrier may sit inward of the nominal edge (shared median where opposite
                # carriageways overlap): search from the nominal edge in to the lane edge.
                lane_edge = sc.lanes * sc.lw * 0.5
                found_inward = False
                # Gore-filled decks (build_roads GORE_FILL) move the barrier outward by up to ~0.8 m.
                u = abs(ue) + 0.8
                while u >= lane_edge - 1e-6 and not found_inward:
                    xx, zz = sc.pos(i, u if ue > 0 else -u)
                    for ti in hash_w.query(xx, zz):
                        t = walls[ti]
                        if xx < t.minx - 0.2 or xx > t.maxx + 0.2 or zz < t.minz - 0.2 or zz > t.maxz + 0.2:
                            continue
                        if max(t.a[1], t.b[1], t.c[1]) < y + 0.2 or min(t.a[1], t.b[1], t.c[1]) > y + 1.0:
                            continue
                        if seg_dist_xz(xx, zz, t) <= 0.2:
                            found_inward = True
                            break
                    u -= 0.25
                if found_inward:
                    covered_n += 1
                    if run_start is not None:
                        gaps.append((sc.id, side, run_start, run_len))
                        run_start = None
                        run_len = 0.0
                    continue
                if footprint_hits(x, z, y, own):
                    opened += 1
                    if run_start is not None:
                        gaps.append((sc.id, side, run_start, run_len))
                        run_start = None
                        run_len = 0.0
                    continue
                found = False
                for ti in hash_w.query(x, z):
                    t = walls[ti]
                    if x < t.minx - 0.2 or x > t.maxx + 0.2 or z < t.minz - 0.2 or z > t.maxz + 0.2:
                        continue
                    ymin = min(t.a[1], t.b[1], t.c[1])
                    ymax = max(t.a[1], t.b[1], t.c[1])
                    if ymax < y + 0.2 or ymin > y + 1.0:
                        continue
                    d = seg_dist_xz(x, z, t)
                    if d <= 0.2:
                        found = True
                        break
                if found:
                    covered_n += 1
                    if run_start is not None:
                        gaps.append((sc.id, side, run_start, run_len))
                        run_start = None
                        run_len = 0.0
                else:
                    if run_start is None:
                        run_start = s
                        run_len = step_s
                    else:
                        run_len += step_s
            if run_start is not None:
                gaps.append((sc.id, side, run_start, run_len))
    bad_gaps = [g for g in gaps if g[3] > 0.3]
    print("barrier samples: %d  covered=%d  opened(footprint rule)=%d  gaps=%d  gaps>0.3m=%d"
          % (checked, covered_n, opened, len(gaps), len(bad_gaps)))
    for g in bad_gaps[:10]:
        print("   gap:", g[0], g[1], "start_s=%.2f" % g[2], "len=%.2f" % g[3])

    # 3. Seam steps across shared ground edges.
    edges = defaultdict(list)
    for ti, t in enumerate(ground):
        verts = (t.a, t.b, t.c)
        for k in range(3):
            p = verts[k]
            q = verts[(k + 1) % 3]
            key = tuple(sorted(((round(p[0], 3), round(p[2], 3)), (round(q[0], 3), round(q[2], 3)))))
            edges[key].append(ti)
    max_step = 0.0
    max_at = None
    n_shared = 0
    n_step_gt = 0
    for key, tl in edges.items():
        if len(tl) != 2:
            continue
        n_shared += 1
        (x0, z0), (x1, z1) = key
        mx, mz = (x0 + x1) * 0.5, (z0 + z1) * 0.5
        h = []
        for ti in tl:
            t = ground[ti]
            hh = height_plane(t, mx, mz)
            h.append(hh)
        step = abs(h[0] - h[1])
        if step > 0.05:
            n_step_gt += 1
        if step > max_step:
            max_step = step
            max_at = (round(mx, 2), round(mz, 2), round(h[0], 3), round(h[1], 3))
    print("seams: shared ground edges=%d  max step=%.4f m at %s  edges with step>0.05=%d"
          % (n_shared, max_step, max_at, n_step_gt))

    # Render-layer checks: deck z-fighting and lamp pole support (build root = parent of collision/).
    build_root = os.path.dirname(os.path.normpath(cdir))
    rdir = args.render_dir or os.path.join(build_root, "render")
    if not os.path.isabs(rdir):
        rdir = os.path.join(ROOT, rdir)
    zfight, legacy, floating = 0, 0, []
    metrics = {"height_mismatch": fail, "missing": missing, "shadow_merge_zone": shadow_ok,
               "shadow_elsewhere": shadow_bad, "lane_wall_spots": len(spots), "barrier_gaps": len(bad_gaps),
               "barrier_gap_list": [list(g) for g in bad_gaps], "lane_wall_list": [list(w) for w in spots],
               "shadow_list": [list(s) for s in shadow_list[:200]]}
    if os.path.isdir(rdir):
        legacy, zf_examples, floating, n_lamps, n_sup = render_checks(
            rdir, os.path.join(build_root, "lamps.json"))
        print("render z-fight pairs, legacy metric (asphalt only, 3-10 mm apart): %d %s" % (legacy, zf_examples))
        print("lamp supports checked: %d of %d, floating: %d %s" % (n_sup, n_lamps, len(floating), floating[:8]))
        metrics.update({"zfight_legacy": legacy, "floating_lamps": len(floating), "lamps": n_lamps})
        rb = None if args.bbox is None else (bx0, bz0, bx1, bz1)
        v2 = render_overlap_checks(rdir, rb)
        if v2 is not None:
            res, ex = v2
            zfight = res["zfight_total"]
            print("render overlaps (all upward render surfaces incl. markings; triangles affected): "
                  "deck-overlap(<%.2f m)=%d [fight<%d mm: %d, %.1f m2]  decal-under=%d (paint %d, joint %d)  "
                  "decal-fight=%d  ground-fight=%d  paint-paint(same colour, info)=%d  -> z-fight total=%d"
                  % (OVERLAP_BAND, res["deck_overlap"], FIGHT_DY * 1000, res["deck_fight"], res["deck_overlap_area_m2"],
                     res["decal_under"], res["paint_under"], res["joint_under"], res["decal_fight"],
                     res["ground_fight"], res["paint_paint"], zfight))
            for k, v in ex.items():
                if v:
                    print("   %s at:" % k, v[:6])
            metrics.update(res)
        else:
            zfight = legacy
        cons = render_collision_consistency(rdir, sampled, ground, hash_g, in_box)
        if cons is not None:
            print("render vs collision (lane centres every 4 m): compared=%d  car-above-visible-deck(>3 cm)=%d  "
                  "car-below(>3 cm)=%d %s" % (cons["compared"], cons["car_above_visible"], cons["car_below_visible"],
                                              cons["examples"][:4]))
            metrics["render_collision"] = cons

    hard_fail = fail + missing + shadow_bad + len(bad_gaps) + len(spots) + zfight + len(floating)
    metrics["zfight"] = zfight
    metrics["result"] = "PASS" if hard_fail == 0 else "FAIL"
    if args.json_out:
        with open(args.json_out, "w", encoding="ascii", newline="\n") as f:
            json.dump(metrics, f, indent=1)
    print("RESULT: %s (height-mismatch=%d missing=%d shadow-elsewhere=%d gaps>0.3=%d lane-wall-spots=%d zfight=%d floating-lamps=%d)"
          % ("PASS" if hard_fail == 0 else "FAIL", fail, missing, shadow_bad, len(bad_gaps), len(spots), zfight, len(floating)))
    return 0 if hard_fail == 0 else 1


def render_checks(render_dir, lamps_path):
    """Render-layer checks (added for the v2 roads pass).

    1. Z-fight: upward asphalt faces (decks) of different surfaces at the same plan position whose
       heights differ by 3-10 mm. Adjacent triangles of one surface are coplanar and ignored.
    2. Lamp support: each pole base (lamps.json "base", ground or parapet support) must sit on the
       highest upward surface below it within 0.12 m. Floating poles fail.
    Returns (zfight_pairs, zfight_examples, floating_lamps, lamp_count, support_checked).
    """
    files = sorted(glob.glob(os.path.join(render_dir, "*.glb")))
    decks = []
    support = []  # render upward surfaces: decks, parapet caps (render only), gravel
    for path in files:
        for a, b, c, mat in glb_triangles(path, with_material=True):
            n = tri_normal(a, b, c)
            if n is None or n[1] < 0.6:
                continue
            if mat in ("asphalt", "concrete_cast", "ground_gravel"):
                support.append(Tri2D(a, b, c, n))
            if mat == "asphalt" and n[1] >= 0.9:
                decks.append(Tri2D(a, b, c, n))
    hash_d = Hash2D(decks, 10.0)
    hash_s = Hash2D(support, 10.0)
    pairs = set()
    examples = []
    for t in decks:
        cx = (t.a[0] + t.b[0] + t.c[0]) / 3.0
        cz = (t.a[2] + t.b[2] + t.c[2]) / 3.0
        h1 = t.height(cx, cz)
        if h1 is None:
            continue
        for j in hash_d.query(cx, cz):
            u = decks[j]
            if u is t:
                continue
            h2 = u.height(cx, cz)
            if h2 is None:
                continue
            dy = abs(h2 - h1)
            if 0.003 < dy < 0.010:  # real depth fight; 10-150 mm pairs are the render lift (Z_LIFT_RENDER)

                key = (min(id(t), id(u)), max(id(t), id(u)))
                if key not in pairs:
                    pairs.add(key)
                    if len(examples) < 5:
                        examples.append((round(cx, 1), round(cz, 1), round(h1, 3), round(h2, 3)))
    floating = []
    n_lamps = 0
    checked = 0
    if lamps_path and os.path.exists(lamps_path):
        with open(lamps_path, "r", encoding="utf-8") as f:
            lamps = json.load(f)["lamps"]
        for lp in lamps:
            n_lamps += 1
            if "base" not in lp:
                continue
            checked += 1
            bx, by, bz = lp["base"]
            hit = ray_ground(support, hash_s, bx, bz, by + 0.5)
            if hit is None or abs(hit[0] - by) > 0.12:
                floating.append((lp.get("id"), round(bx, 1), round(by, 2), round(bz, 1),
                                 None if hit is None else round(hit[0], 2)))
    return len(pairs), examples, floating, n_lamps, checked


# ----------------------------------------------------------------------------------------------
# Render overlap check v2 (numpy): every upward render surface, markings included
# ----------------------------------------------------------------------------------------------
OVERLAP_BAND = 0.15   # m: two upward surfaces closer than this in height at one plan point overlap
DECAL_CLEAR = 0.004   # m: a marking / joint must sit at least this far above any deck it lies on
FIGHT_DY = 0.010      # m: below this, two opaque surfaces depth-fight at driving distances


def render_tris_np(render_dir):
    """{material: (N, 3, 3) float64 world triangles} for all render GLBs (numpy required)."""
    out = defaultdict(list)
    for path in sorted(glob.glob(os.path.join(render_dir, "*.glb"))):
        js, binc = read_glb(path)
        ident = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
        scene_nodes = js.get("scenes", [{}])[js.get("scene", 0)].get("nodes", list(range(len(js.get("nodes", [])))))

        def visit(ni, parent):
            node = js["nodes"][ni]
            M = matmul(parent, node_matrix(node))
            if "mesh" in node:
                for prim in js["meshes"][node["mesh"]]["primitives"]:
                    if prim.get("mode", 4) != 4:
                        continue
                    mat = js["materials"][prim["material"]].get("name") if "material" in prim else None
                    pos = np.asarray(accessor_array(js, binc, prim["attributes"]["POSITION"]), dtype=np.float64)[:, :3]
                    Mn = np.asarray(M, dtype=np.float64)
                    pos = pos @ Mn[:3, :3].T + Mn[:3, 3]
                    if "indices" in prim:
                        ind = np.asarray(accessor_array(js, binc, prim["indices"])).reshape(-1).astype(np.int64)
                    else:
                        ind = np.arange(len(pos))
                    ind = ind[: len(ind) // 3 * 3].reshape(-1, 3)
                    out[mat].append(pos[ind])
            for c in node.get("children", []):
                visit(c, M)

        for ni in scene_nodes:
            visit(ni, ident)
    return {k: np.concatenate(v, axis=0) for k, v in out.items() if v}


def _upward(tris, min_ny):
    e1 = tris[:, 1] - tris[:, 0]
    e2 = tris[:, 2] - tris[:, 0]
    n = np.cross(e1, e2)
    ln = np.linalg.norm(n, axis=1)
    ok = ln > 1e-9
    ny = np.zeros(len(tris))
    ny[ok] = np.abs(n[ok, 1]) / ln[ok]
    return tris[ok & (ny >= min_ny)]


def _contain_pairs(P, T, cell=4.0):
    """Candidate (point index, triangle index, triangle height at the point) for points P (M, 2: x, z)
    lying inside the plan projection of triangles T (N, 3, 3)."""
    if len(P) == 0 or len(T) == 0:
        return np.zeros(0, np.int64), np.zeros(0, np.int64), np.zeros(0)
    tx, tz = T[:, :, 0], T[:, :, 2]
    i0 = np.floor(tx.min(1) / cell).astype(np.int64)
    i1 = np.floor(tx.max(1) / cell).astype(np.int64)
    k0 = np.floor(tz.min(1) / cell).astype(np.int64)
    k1 = np.floor(tz.max(1) / cell).astype(np.int64)
    ni, nk = i1 - i0 + 1, k1 - k0 + 1
    cnt = ni * nk
    tri_of = np.repeat(np.arange(len(T)), cnt)
    off = np.arange(cnt.sum()) - np.repeat(np.cumsum(cnt) - cnt, cnt)
    ci = np.repeat(i0, cnt) + off % np.repeat(ni, cnt)
    ck = np.repeat(k0, cnt) + off // np.repeat(ni, cnt)
    key = ci * 1000003 + ck
    order = np.argsort(key, kind="stable")
    key, tri_of = key[order], tri_of[order]
    pkey = np.floor(P[:, 0] / cell).astype(np.int64) * 1000003 + np.floor(P[:, 1] / cell).astype(np.int64)
    lo = np.searchsorted(key, pkey, "left")
    hi = np.searchsorted(key, pkey, "right")
    m = hi - lo
    pi = np.repeat(np.arange(len(P)), m)
    ti = tri_of[np.repeat(lo, m) + (np.arange(m.sum()) - np.repeat(np.cumsum(m) - m, m))]
    a, b, c = T[ti, 0], T[ti, 1], T[ti, 2]
    x, z = P[pi, 0], P[pi, 1]
    den = (b[:, 2] - c[:, 2]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 2] - c[:, 2])
    ok = np.abs(den) > 1e-12
    den = np.where(ok, den, 1.0)
    l1 = ((b[:, 2] - c[:, 2]) * (x - c[:, 0]) + (c[:, 0] - b[:, 0]) * (z - c[:, 2])) / den
    l2 = ((c[:, 2] - a[:, 2]) * (x - c[:, 0]) + (a[:, 0] - c[:, 0]) * (z - c[:, 2])) / den
    l3 = 1.0 - l1 - l2
    inside = ok & (l1 > 1e-6) & (l2 > 1e-6) & (l3 > 1e-6)
    h = l1 * a[:, 1] + l2 * b[:, 1] + l3 * c[:, 1]
    return pi[inside], ti[inside], h[inside]


def render_overlap_checks(render_dir, bbox=None):
    """Plan overlaps between upward render surfaces, sampled at triangle centroids.

    deck-overlap:  two asphalt (deck) surfaces within OVERLAP_BAND in height at one point (duplicated
                   drivable surfaces in the render mesh; 0 when lower-priority decks are clipped).
    deck-fight:    the subset closer than FIGHT_DY (visible depth fighting).
    decal-under:   a marking (paint) or joint triangle not at least DECAL_CLEAR above every deck within
                   OVERLAP_BAND below/above it (hidden or flickering marking).
    decal-fight:   paint over a joint (or joint over paint) closer than DECAL_CLEAR.
    paint-paint:   overlapping paint of different strips (same colour; reported only).
    ground-fight:  street ground/roof cells within FIGHT_DY of a deck.
    Counts are triangles whose centroid is affected (one per triangle, not per pair)."""
    if np is None:
        return None
    mats = render_tris_np(render_dir)
    asph = _upward(mats.get("asphalt", np.zeros((0, 3, 3))), 0.9)
    paint = _upward(mats.get("paint", np.zeros((0, 3, 3))), 0.9)
    joint = _upward(mats.get("joint", np.zeros((0, 3, 3))), 0.9)
    ground = _upward(mats.get("ground_gravel", np.zeros((0, 3, 3))), 0.99)

    def cen(T):
        c = T.mean(axis=1)
        keep = np.ones(len(T), bool)
        if bbox is not None:
            x0, z0, x1, z1 = bbox
            keep = (c[:, 0] >= x0) & (c[:, 0] <= x1) & (c[:, 2] >= z0) & (c[:, 2] <= z1)
        return c, keep

    res = {"tris": {"asphalt": len(asph), "paint": len(paint), "joint": len(joint), "ground": len(ground)}}
    ex = {}
    # Deck vs deck.
    c, keep = cen(asph)
    pi, ti, h = _contain_pairs(c[:, [0, 2]], asph)
    sel = (pi != ti) & keep[pi]
    dy = np.abs(h - c[pi, 1])
    ov = sel & (dy < OVERLAP_BAND)
    fi = sel & (dy < FIGHT_DY)
    res["deck_overlap"] = int(len(np.unique(pi[ov])))
    res["deck_fight"] = int(len(np.unique(pi[fi])))
    uo = np.unique(pi[ov])
    e1 = asph[uo, 1] - asph[uo, 0]
    e2 = asph[uo, 2] - asph[uo, 0]
    res["deck_overlap_area_m2"] = round(float(np.sum(0.5 * np.abs(e1[:, 0] * e2[:, 2] - e1[:, 2] * e2[:, 0]))), 1)
    ex["deck_overlap"] = [tuple(round(float(v), 2) for v in c[k]) for k in np.unique(pi[ov])[:8]]
    # Decals (paint, joints) vs decks.
    for name, T in (("paint", paint), ("joint", joint)):
        c, keep = cen(T)
        pi, ti, h = _contain_pairs(c[:, [0, 2]], asph)
        d = c[pi, 1] - h          # decal height above the deck at the decal centroid
        bad = keep[pi] & (d < DECAL_CLEAR) & (d > -OVERLAP_BAND)
        res[name + "_under"] = int(len(np.unique(pi[bad])))
        ex[name + "_under"] = [tuple(round(float(v), 2) for v in c[k]) for k in np.unique(pi[bad])[:8]]
    res["decal_under"] = res["paint_under"] + res["joint_under"]
    # Paint vs joint, paint vs paint.
    c, keep = cen(paint)
    pi, ti, h = _contain_pairs(c[:, [0, 2]], joint)
    bad = keep[pi] & (np.abs(c[pi, 1] - h) < DECAL_CLEAR)
    res["decal_fight"] = int(len(np.unique(pi[bad])))
    ex["decal_fight"] = [tuple(round(float(v), 2) for v in c[k]) for k in np.unique(pi[bad])[:8]]
    pi, ti, h = _contain_pairs(c[:, [0, 2]], paint)
    d = np.abs(c[pi, 1] - h)
    bad = keep[pi] & (pi != ti) & (d > 0.0005) & (d < OVERLAP_BAND)
    res["paint_paint"] = int(len(np.unique(pi[bad])))
    # Ground vs deck.
    c, keep = cen(ground)
    pi, ti, h = _contain_pairs(c[:, [0, 2]], asph)
    bad = keep[pi] & (np.abs(c[pi, 1] - h) < FIGHT_DY)
    res["ground_fight"] = int(len(np.unique(pi[bad])))
    res["zfight_total"] = res["deck_overlap"] + res["decal_under"] + res["decal_fight"] + res["ground_fight"]
    return res, ex


def render_collision_consistency(render_dir, sampled, ground, hash_g, in_box, step=4.0):
    """Lane-centre samples: top render deck (asphalt) vs top collision ground below the expected deck
    + 1.5 m. A car rides on collision; the visible deck must be within 3 cm of it."""
    if np is None:
        return None
    mats = render_tris_np(render_dir)
    asph = _upward(mats.get("asphalt", np.zeros((0, 3, 3))), 0.6)
    pts, ys = [], []
    for sc in sampled:
        for u in sc.lane_centres():
            for k in range(int(sc.S[-1] // step) + 1):
                i = min(sc.N - 2, max(1, int(round(k * step))))
                x, z = sc.pos(i, u)
                if in_box(x, z):
                    pts.append((x, z))
                    ys.append(sc.top_y(i, u))
    P = np.asarray(pts)
    Y = np.asarray(ys)
    pi, ti, h = _contain_pairs(P, asph)
    ok = h <= Y[pi] + 1.5
    top_r = np.full(len(P), -1e9)
    np.maximum.at(top_r, pi[ok], h[ok])
    worst = []
    n_float = n_sink = n_cmp = 0
    for k in range(len(P)):
        hit = ray_ground(ground, hash_g, P[k, 0], P[k, 1], Y[k] + 1.5)
        if hit is None or top_r[k] < -1e8:
            continue
        n_cmp += 1
        d = hit[0] - top_r[k]
        if d > 0.03:
            n_float += 1
        elif d < -0.03:
            n_sink += 1
        if abs(d) > 0.03 and len(worst) < 8:
            worst.append((round(float(P[k, 0]), 1), round(float(P[k, 1]), 1), round(float(top_r[k]), 3), round(float(hit[0]), 3)))
    return {"compared": n_cmp, "car_above_visible": n_float, "car_below_visible": n_sink, "examples": worst}


def height_plane(t, x, z):
    n = t.n
    if abs(n[1]) < 1e-9:
        return t.a[1]
    return t.a[1] - (n[0] * (x - t.a[0]) + n[2] * (z - t.a[2])) / n[1]


def seg_dist_xz(x, z, t):
    """Distance in plan from (x, z) to the triangle's plan projection (segment for vertical faces)."""
    pts = [(t.a[0], t.a[2]), (t.b[0], t.b[2]), (t.c[0], t.c[2])]
    best = 1e18
    for k in range(3):
        ax, az = pts[k]
        bx, bz = pts[(k + 1) % 3]
        dx, dz = bx - ax, bz - az
        L2 = dx * dx + dz * dz
        if L2 < 1e-12:
            d = math.hypot(x - ax, z - az)
        else:
            f = max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / L2))
            d = math.hypot(x - (ax + f * dx), z - (az + f * dz))
        best = min(best, d)
    return best


if __name__ == "__main__":
    sys.exit(main())
