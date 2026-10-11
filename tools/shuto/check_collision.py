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
EPS_BY_KIND = {"main": 0.0, "jct": 0.0, "pa": 0.0, "toll": 0.0, "ramp_on": 0.01, "ramp_off": 0.02}
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
        self.TX, self.TZ = [], []
        for i in range(n):
            a = max(0, i - 1)
            b = min(n - 1, i + 1)
            dx, dz = self.X[b] - self.X[a], self.Z[b] - self.Z[a]
            ln = math.hypot(dx, dz) or 1.0
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
            s_list = [k * args.step for k in range(int(sc.S[-1] // args.step) + 1)]
            for s in s_list:
                i = min(sc.N - 1, int(round(s)))
                x, z = sc.pos(i, u)
                if not in_box(x, z):
                    continue
                y_exp = sc.top_y(i, u)
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
    for own, sc in enumerate(sampled):
        for side, ue in (("L", sc.half_l), ("R", -sc.half_r)):
            run_start = None
            run_len = 0.0
            step_s = 0.25
            m = int(sc.S[-1] / step_s)
            for k in range(m + 1):
                s = k * step_s
                i = min(sc.N - 1, int(round(s)))
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
    rdir = os.path.join(build_root, "render")
    zfight, floating = 0, []
    if os.path.isdir(rdir):
        zfight, zf_examples, floating, n_lamps, n_sup = render_checks(
            rdir, os.path.join(build_root, "lamps.json"))
        print("render z-fight pairs (asphalt 3 mm-0.15 m apart): %d %s" % (zfight, zf_examples))
        print("lamp supports checked: %d of %d, floating: %d %s" % (n_sup, n_lamps, len(floating), floating[:8]))

    hard_fail = fail + missing + shadow_bad + len(bad_gaps) + len(spots) + zfight + len(floating)
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
