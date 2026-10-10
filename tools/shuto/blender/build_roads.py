"""Generic Shuto road generator: contract network file -> chunked render/collision GLBs + .blend.

Run headless (Blender 4.3, Python 3.11):
  blender.exe -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/build_roads.py -- ^
      --network data/shuto/test_network.json --out models/shuto/test_site --bbox -700,-400,400,700 --chunk 500

Outputs (paths relative to the repo root unless absolute):
  <out>/render/<cx>_<cz>.glb            render meshes (world coordinates, node at origin)
  <out>/collision/<cx>_<cz>_col.glb     collision meshes (ground, ceiling, wall triangles only)
  <out>/chunks.json                     chunk bounds, files, triangle counts
  <out>/lamps.json                      light pole lamp heads (position + direction)
  models/shuto/src/<name>.blend         editable scene (name = basename of --out)

Frame: NOX (+X east, +Y up, +Z south, metres). Blender is written as (x, -z, y) so the glTF
exporter (Y-up) writes NOX coordinates back unchanged.

Geometry rules:
  * Decks: lanes + shoulders ribbon, banking from the contract, 1 m sampling, UVs in metres / tile.
  * Markings: 0.15 m strips raised 0.01 m; dashed (8 on / 12 off) dividers, solid edge lines.
  * Parapets: 1.1 m, vertical inner face (collision wall), sloped outer face. A barrier segment is
    omitted when its edge lies inside another carriageway's footprint with |dy| < 1.5 m.
  * Per-kind epsilon lifts ramps 1-2 cm over the mainline where they overlap (no z-fighting).
  * Cut/tunnel/underground ranges are covered boxes: walls to the cover level (ground_y), a ceiling
    at ground_y (collision: underside only), and a lighting strip. Ground cells under the cover are
    replaced by a roof top (render only), so no ground triangle sits above a tunnel road.
  * Piers every 30 m under elevated/bridge ranges, down to ground; they step aside (offsets) when a
    lower road is in the way. Embankments are 1:1 fills (ground class) down to the ground plane.
  * Ground plane: gravel cells at ground_y - 0.05 (so road decks at ground_y never z-fight it).
"""
import argparse
import bisect
import json
import math
import os
import sys
from collections import defaultdict

try:
    import bpy  # only available inside Blender
except ImportError:  # allows importing the module for syntax checks outside Blender
    bpy = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
TEX_DIR = os.path.join(ROOT, "textures", "shuto", "derived")
MANIFEST = os.path.join(TEX_DIR, "derived_manifest.json")

DECK_SLAB = 1.2
PARAPET_H = 1.1
COVER_HALF = 12.0
WALL_HALF = 5.0
PIER_SPACING = 30.0
PIER_HALF = 0.9
JOINT_SPACING = 40.0
POLE_SPACING = 35.0
CELL = 20.0
TUNNEL_TYPES = ("cut", "tunnel", "underground")   # trench: retaining walls up to ground level
ROOF_TYPES = ("tunnel", "underground")             # covered: ceiling, lighting; "cut" is an open trench
ELEVATED_TYPES = ("elevated", "bridge")
EPS_BY_KIND = {"main": 0.0, "jct": 0.0, "pa": 0.0, "toll": 0.0, "ramp_on": 0.01, "ramp_off": 0.02}
LAYERS_BOTH = ("render", "col")
UP = (0.0, 1.0, 0.0)

# Material key -> (texture asset id, use seamless variant, roughness, metallic, colour, emission)
MATERIALS = {
    "asphalt": ("asphalt_fine", False),
    "concrete_cast": ("concrete_cast", False),
    "concrete_weathered": ("concrete_weathered", False),
    "tunnel_tiles": ("tunnel_tiles", True),
    "galvanized": ("galvanized_steel", True),
    "ground_gravel": ("ground_gravel", False),
    "noise_barrier": ("noise_barrier", False),
}


def log(msg):
    print("[build_roads] " + msg, flush=True)


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


# ----------------------------------------------------------------------------------------------
# Road samples
# ----------------------------------------------------------------------------------------------
class Road:
    """One carriageway resampled to ~1 m with per-sample frame, banking and structure type."""

    def __init__(self, cw, step=1.0):
        self.id = cw["id"]
        self.kind = cw.get("kind", "main")
        self.lanes = int(cw.get("lanes", 2))
        self.lw = float(cw.get("lane_width", 3.25))
        self.shl = float(cw.get("shoulder_left", 0.75))
        self.shr = float(cw.get("shoulder_right", 0.5))
        self.half_l = self.lanes * self.lw * 0.5 + self.shl
        self.half_r = self.lanes * self.lw * 0.5 + self.shr
        self.eps = EPS_BY_KIND.get(self.kind, 0.0)
        self.structure = sorted(cw.get("structure", []), key=lambda r: r["s0"])
        self.name = cw.get("name_en", self.id)

        pts = cw["points"]
        banks = cw.get("bank") or []
        if len(banks) != len(pts):
            banks = [0.0] * len(pts)
        cum = [0.0]
        for i in range(1, len(pts)):
            a, b = pts[i - 1], pts[i]
            cum.append(cum[-1] + math.dist(a, b))
        total = cum[-1]
        n = max(2, int(math.ceil(total / step)) + 1)
        self.S, self.X, self.Y, self.Z, bank = [], [], [], [], []
        for k in range(n):
            t = total * k / (n - 1)
            j = clamp(bisect.bisect_right(cum, t) - 1, 0, len(pts) - 2)
            seg = cum[j + 1] - cum[j]
            f = 0.0 if seg <= 1e-9 else (t - cum[j]) / seg
            a, b = pts[j], pts[j + 1]
            self.S.append(t)
            self.X.append(a[0] + (b[0] - a[0]) * f)
            self.Y.append(a[1] + (b[1] - a[1]) * f)
            self.Z.append(a[2] + (b[2] - a[2]) * f)
            bank.append(banks[j] + (banks[j + 1] - banks[j]) * f)
        self.N = n
        # Effective deck half widths per sample (clipped to a shared median where an
        # opposite-direction carriageway runs alongside; see Context.compute_edges).
        self.HL = [self.half_l] * n
        self.HR = [self.half_r] * n
        self.BANK = bank
        self.SB = [math.sin(b) for b in bank]
        # Tangent (xz) and left normal (dz, -dx): left of travel with +Y up.
        self.TX, self.TZ, self.LX, self.LZ = [], [], [], []
        for i in range(n):
            a = max(0, i - 1)
            b = min(n - 1, i + 1)
            dx = self.X[b] - self.X[a]
            dz = self.Z[b] - self.Z[a]
            ln = math.hypot(dx, dz) or 1.0
            dx /= ln
            dz /= ln
            self.TX.append(dx)
            self.TZ.append(dz)
            self.LX.append(dz)
            self.LZ.append(-dx)
        # Structure type per sample.
        self.ST = [self.type_at(s) for s in self.S]
        self.COV = [t in TUNNEL_TYPES for t in self.ST]
        self.ROOF = [t in ROOF_TYPES for t in self.ST]
        self.ELEV = [t in ELEVATED_TYPES for t in self.ST]
        self.EMB = [t == "embankment" for t in self.ST]
        self.cov_range = [self.covered_range(s) if c else None for s, c in zip(self.S, self.COV)]
        self.cov_roof = [self.covered_range(s, ROOF_TYPES) if c else None for s, c in zip(self.S, self.ROOF)]

    def type_at(self, s):
        for r in self.structure:
            if r["s0"] - 1e-6 <= s < r["s1"] - 1e-6:
                return r["type"]
        if self.structure:
            return self.structure[-1]["type"]
        return "at_grade"

    def covered_range(self, s, types=TUNNEL_TYPES):
        for r in self.structure:
            if r["s0"] - 1e-6 <= s < r["s1"] - 1e-6 and r["type"] in types:
                return (r["s0"], r["s1"])
        return None

    def top_y(self, i, u):
        return self.Y[i] + self.eps + u * self.SB[i]

    def top(self, i, u):
        return (self.X[i] + u * self.LX[i], self.top_y(i, u), self.Z[i] + u * self.LZ[i])

    def lateral(self, i, x, z):
        dx = x - self.X[i]
        dz = z - self.Z[i]
        return dx * self.LX[i] + dz * self.LZ[i], dx * self.TX[i] + dz * self.TZ[i]


class Grid:
    def __init__(self, cell):
        self.cell = cell
        self.buckets = defaultdict(list)

    def add(self, x, z, val):
        self.buckets[(math.floor(x / self.cell), math.floor(z / self.cell))].append(val)

    def near(self, x, z, r):
        c = self.cell
        i0, i1 = math.floor((x - r) / c), math.floor((x + r) / c)
        k0, k1 = math.floor((z - r) / c), math.floor((z + r) / c)
        for i in range(i0, i1 + 1):
            for k in range(k0, k1 + 1):
                for v in self.buckets.get((i, k), ()):
                    yield v


class Context:
    def __init__(self, roads):
        self.roads = roads
        self.grid = Grid(12.0)
        self.cgrid = Grid(12.0)
        for ri, r in enumerate(roads):
            for si in range(r.N):
                self.grid.add(r.X[si], r.Z[si], (ri, si))
                if r.COV[si]:
                    self.cgrid.add(r.X[si], r.Z[si], (ri, si))

    def compute_edges(self):
        """Clip decks where opposite-direction carriageways overlap side by side (shared trenches,
        twin decks with OSM centrelines closer than the two deck widths): each side's edge moves to
        the midline, where emit_barriers then places the median barrier. Same-direction overlaps
        (merges/diverges) are left alone; the footprint rule opens those barriers."""
        clipped = 0
        for road in self.roads:
            for i in range(road.N):
                for ri, si in self.grid.near(road.X[i], road.Z[i], 14.0):
                    r = self.roads[ri]
                    if r is road:
                        continue
                    dy = abs(r.Y[si] - road.Y[i])
                    # Only genuinely side-by-side opposite carriageways (near anti-parallel);
                    # carriageways crossing at an angle are crossings, not medians.
                    dot = road.TX[i] * r.TX[si] + road.TZ[i] * r.TZ[si]
                    opposite = dot < -0.9
                    # Opposite directions overlapping in plan share a median (same level) or are a
                    # split-level pair (< 7 m apart vertically: one cannot pass under the other);
                    # both decks are clipped to the midline and walled there.
                    # Same-direction overlaps are diverges/merges (ramps leaving and climbing):
                    # left to the footprint rule, never clipped.
                    # Same level: median. Split level: only for (nearly) antiparallel decks; a
                    # shallow-angle crossing at a different level is a crossing, not a pair.
                    if not (opposite and (dy <= 2.0 or (dy < 7.0 and dot < -0.985))):
                        continue
                    lat, lon = road.lateral(i, r.X[si], r.Z[si])
                    if abs(lon) > 1.0:
                        continue
                    if abs(lat) < road.lanes * road.lw:
                        continue  # centrelines closer than the lanes need: data overlap, never clip into lanes
                    if lat > 0.0 and lat < road.half_l + r.half_l:
                        if lat * 0.5 < road.HL[i]:
                            road.HL[i] = max(lat * 0.5, 1.0)
                            clipped += 1
                    elif lat < 0.0 and -lat < road.half_r + r.half_l:
                        if -lat * 0.5 < road.HR[i]:
                            road.HR[i] = max(-lat * 0.5, 1.0)
                            clipped += 1
        log("median clipping: %d samples" % clipped)

    def footprints(self, x, z, margin, exclude=None):
        """Other roads whose lateral band (plus margin) contains (x, z), using their nearest sample."""
        best = {}
        for ri, si in self.grid.near(x, z, 14.0):
            r = self.roads[ri]
            if r is exclude:
                continue
            d2 = (r.X[si] - x) ** 2 + (r.Z[si] - z) ** 2
            if d2 > 196.0:
                continue
            cur = best.get(ri)
            if cur is None or d2 < cur[0]:
                best[ri] = (d2, si)
        out = []
        for ri, (_, si) in best.items():
            r = self.roads[ri]
            lat, lon = r.lateral(si, x, z)
            # Strictly inside (5 cm), so a shared median edge does not count as covered.
            if lat < -r.HR[si] - margin + 0.05 or lat > r.HL[si] + margin - 0.05:
                continue
            # Longitudinal bound: the nearest sample only represents the road up to half a sample
            # spacing either side, and nothing beyond its first/last sample (otherwise a ramp's
            # extended axis would "cover" the barrier of the road it leaves, opening a long gap).
            prev_gap = r.S[si] - r.S[si - 1] if si > 0 else 0.0
            next_gap = r.S[si + 1] - r.S[si] if si + 1 < r.N else 0.0
            if lon < -(prev_gap * 0.5 + margin) or lon > next_gap * 0.5 + margin:
                continue
            out.append((r, si, lat, r.top_y(si, lat)))
        return out

    def blocked(self, road, x, y, z):
        for r, si, lat, ytop in self.footprints(x, z, 0.0, exclude=road):
            if abs(ytop - y) < 1.5:
                return True
        return False

    def pier_conflict(self, road, x, z, ylo, yhi):
        for r, si, lat, ytop in self.footprints(x, z, PIER_HALF + 1.0, exclude=road):
            if ytop - DECK_SLAB < yhi and ytop > ylo:
                return True
        return False

    def cell_covered(self, x, z):
        """None (normal ground), "roof" (tunnel below: render-only roof) or "open" (open trench: no ground)."""
        best = None
        for ri, si in self.cgrid.near(x, z, 12.0):
            r = self.roads[ri]
            d2 = (r.X[si] - x) ** 2 + (r.Z[si] - z) ** 2
            if best is None or d2 < best[0]:
                best = (d2, ri, si)
        if best is None:
            return None
        _, ri, si = best
        r = self.roads[ri]
        lat, along = r.lateral(si, x, z)
        if lat > r.HL[si] + 0.3 or lat < -r.HR[si] - 0.3:
            return None
        rng = r.cov_range[si]
        if rng is None:
            return None
        sc = r.S[si] + along
        if not (rng[0] - 1e-6 <= sc <= rng[1] + 1e-6):
            return None
        roof = r.cov_roof[si]
        return "roof" if roof is not None and roof[0] - 1e-6 <= sc <= roof[1] + 1e-6 else "open"


# ----------------------------------------------------------------------------------------------
# Triangle output (per chunk, layer and material)
# ----------------------------------------------------------------------------------------------
class Output:
    def __init__(self, chunk, bbox):
        self.chunk = chunk
        self.bbox = bbox
        self.tris = defaultdict(list)  # (cx, cz, layer, mat) -> [(pts, uvs)]
        self.lamps = []

    def add_tri(self, layers, mat, pts, uvs, want=None):
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = pts
        ux, uy, uz = bx - ax, by - ay, bz - az
        vx, vy, vz = cx - ax, cy - ay, cz - az
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        area2 = math.sqrt(nx * nx + ny * ny + nz * nz)
        if area2 < 1e-9:
            return
        if want is not None and (nx * want[0] + ny * want[1] + nz * want[2]) < 0.0:
            pts = (pts[0], pts[2], pts[1])
            uvs = (uvs[0], uvs[2], uvs[1])
        mx = (ax + bx + cx) / 3.0
        mz = (az + bz + cz) / 3.0
        if self.bbox is not None:
            x0, z0, x1, z1 = self.bbox
            if not (x0 <= mx <= x1 and z0 <= mz <= z1):
                return
        key_cx = math.floor(mx / self.chunk)
        key_cz = math.floor(mz / self.chunk)
        for layer in layers:
            self.tris[(key_cx, key_cz, layer, mat)].append((pts, uvs))

    def add_quad(self, layers, mat, p0, p1, p2, p3, uv0, uv1, uv2, uv3, want=None):
        self.add_tri(layers, mat, (p0, p1, p2), (uv0, uv1, uv2), want)
        self.add_tri(layers, mat, (p0, p2, p3), (uv0, uv2, uv3), want)

    def add_box(self, layers, mat, c, axes, half, uvscale=1.0):
        """Closed box: centre c, orthonormal axes (3 tuples), half extents (3 floats)."""
        for k in range(3):
            for sgn in (-1.0, 1.0):
                fc = [c[j] + sgn * half[k] * axes[k][j] for j in range(3)]
                a1, a2 = [(k + 1) % 3, (k + 2) % 3]
                corners = []
                for s1, s2 in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                    corners.append(tuple(fc[j] + s1 * half[a1] * axes[a1][j] + s2 * half[a2] * axes[a2][j]
                                         for j in range(3)))
                want = tuple(sgn * axes[k][j] for j in range(3))
                uv = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
                self.add_quad(layers, mat, corners[0], corners[1], corners[2], corners[3],
                              uv[0], uv[1], uv[2], uv[3], want)


def add_v(a, b, s=1.0):
    return (a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s)


# ----------------------------------------------------------------------------------------------
# Per-road emitters
# ----------------------------------------------------------------------------------------------
def emit_deck(out, road):
    tile = TILE["asphalt"]
    for i in range(road.N - 1):
        l0 = road.top(i, road.HL[i])
        r0 = road.top(i, -road.HR[i])
        l1 = road.top(i + 1, road.HL[i + 1])
        r1 = road.top(i + 1, -road.HR[i + 1])
        s0 = road.S[i] / tile
        s1 = road.S[i + 1] / tile
        out.add_quad(LAYERS_BOTH, "asphalt", l0, r0, r1, l1,
                     (road.HL[i] / tile, s0), (-road.HR[i] / tile, s0), (-road.HR[i + 1] / tile, s1), (road.HL[i + 1] / tile, s1), UP)


def emit_markings(out, road):
    half_edge = road.lanes * road.lw * 0.5
    dividers = [-(road.lanes / 2.0 - k) * road.lw for k in range(1, road.lanes)]
    lines = [(u, True) for u in dividers] + [(half_edge - 0.25, False), (-(half_edge - 0.25), False)]
    width = 0.15
    for u, dashed in lines:
        for i in range(road.N - 1):
            if dashed:
                phase = road.S[i] % 20.0
                if phase >= 8.0:
                    continue
            a0 = road.top(i, u - width * 0.5)
            a1 = road.top(i, u + width * 0.5)
            b0 = road.top(i + 1, u - width * 0.5)
            b1 = road.top(i + 1, u + width * 0.5)
            lift = (0.0, 0.01, 0.0)
            out.add_quad(LAYERS_BOTH[:1], "paint", add_v(a0, lift), add_v(a1, lift), add_v(b1, lift),
                         add_v(b0, lift), (0, 0), (1, 0), (1, 1), (0, 1), UP)


def emit_barriers(out, road, ctx):
    """Parapets on both edges; segments inside another carriageway footprint at similar height are skipped."""
    for sgn in (1.0, -1.0):
        edge = road.HL if sgn > 0 else [-v for v in road.HR]
        for i in range(road.N - 1):
            ue = edge[i]
            # Open the segment when its start, middle or end lies on another deck at this level
            # (merges with kinked/crossing centrelines would otherwise leave stubs in the lanes).
            ex, ey, ez = road.top(i, ue)
            fx, fy, fz = road.top(i + 1, edge[i + 1])
            if (ctx.blocked(road, ex, ey, ez) or ctx.blocked(road, fx, fy, fz) or
                    ctx.blocked(road, 0.5 * (ex + fx), 0.5 * (ey + fy), 0.5 * (ez + fz))):
                continue
            prof = [(0.0, 0.0), (0.0, PARAPET_H), (0.2, PARAPET_H), (0.15, 0.0)]  # (outward, height)

            def P(j, k):
                o, h = prof[k]
                x, y, z = road.top(j, edge[j] + sgn * o)
                return (x, y + h, z)

            inward = (-sgn * road.LX[i], 0.0, -sgn * road.LZ[i])
            inward2 = (-sgn * road.LX[i + 1], 0.0, -sgn * road.LZ[i + 1])
            # Inner face (collision wall and render).
            out.add_quad(LAYERS_BOTH, "concrete_cast", P(i, 0), P(i + 1, 0), P(i + 1, 1), P(i, 1),
                         (sgn * 0.0 + road.S[i] / 3.0, 0.0), (road.S[i + 1] / 3.0, 0.0),
                         (road.S[i + 1] / 3.0, PARAPET_H / 3.0), (road.S[i] / 3.0, PARAPET_H / 3.0),
                         tuple((inward[j] + inward2[j]) * 0.5 for j in range(3)))
            # Top cap (render).
            out.add_quad(("render",), "concrete_cast", P(i, 1), P(i, 2), P(i + 1, 2), P(i + 1, 1),
                         (0, 0), (0.2 / 3.0, 0), (0.2 / 3.0, 0.1), (0, 0.1), UP)
            # Outer sloped face (render).
            outward = (sgn * road.LX[i], 0.3, sgn * road.LZ[i])
            out.add_quad(("render",), "concrete_cast", P(i, 2), P(i, 3), P(i + 1, 3), P(i + 1, 2),
                         (0, 0), (0.1, 0), (0.1, 0.1), (0, 0.1), outward)


def emit_tunnel(out, road, ctx):
    """Covered box: walls to the cover level, a ceiling at ground_y and a lighting strip."""
    tile = TILE["tunnel_tiles"]
    for i in range(road.N - 1):
        if not (road.COV[i] and road.COV[i + 1]):
            continue
        for sgn in (1.0, -1.0):
            uw = (road.HL[i] + 0.3) if sgn > 0 else -(road.HR[i] + 0.3)
            uw1 = (road.HL[i + 1] + 0.3) if sgn > 0 else -(road.HR[i + 1] + 0.3)
            wx, wy, wz = road.top(i, uw)
            if ctx.blocked(road, wx, wy, wz):
                continue  # inside a neighbouring carriageway of the same trench
            b0 = road.top_y(i, uw)
            b1 = road.top_y(i + 1, uw1)
            if GROUND_Y - b0 < 0.02 and GROUND_Y - b1 < 0.02:
                continue
            p0 = (road.X[i] + uw * road.LX[i], b0, road.Z[i] + uw * road.LZ[i])
            p1 = (road.X[i] + uw * road.LX[i], GROUND_Y, road.Z[i] + uw * road.LZ[i])
            p2 = (road.X[i + 1] + uw1 * road.LX[i + 1], GROUND_Y, road.Z[i + 1] + uw1 * road.LZ[i + 1])
            p3 = (road.X[i + 1] + uw1 * road.LX[i + 1], b1, road.Z[i + 1] + uw1 * road.LZ[i + 1])
            want = (-sgn * road.LX[i], 0.0, -sgn * road.LZ[i])
            s0 = road.S[i] / tile
            s1 = road.S[i + 1] / tile
            v0 = (GROUND_Y - b0) / tile
            v1 = (GROUND_Y - b1) / tile
            out.add_quad(LAYERS_BOTH, "tunnel_tiles", p0, p1, p2, p3,
                         (s0, v0), (s0, 0.0), (s1, 0.0), (s1, v1), want)
        if not (road.ROOF[i] and road.ROOF[i + 1]):
            continue
        # Ceiling underside (collision ceiling class, n.y = -1).
        c0 = [road.top(i, u) for u in (-road.HR[i] - 0.3, road.HL[i] + 0.3)]
        c1 = [road.top(i + 1, u) for u in (-road.HR[i + 1] - 0.3, road.HL[i + 1] + 0.3)]
        cz0 = (c0[0][0], GROUND_Y, c0[0][2])
        cz1 = (c0[1][0], GROUND_Y, c0[1][2])
        cz2 = (c1[1][0], GROUND_Y, c1[1][2])
        cz3 = (c1[0][0], GROUND_Y, c1[0][2])
        out.add_quad(LAYERS_BOTH, "tunnel_tiles", cz0, cz1, cz2, cz3,
                     (-WALL_HALF / tile, road.S[i] / tile), (WALL_HALF / tile, road.S[i] / tile),
                     (WALL_HALF / tile, road.S[i + 1] / tile), (-WALL_HALF / tile, road.S[i + 1] / tile), (0, -1, 0))
        # Emissive lighting strip under the ceiling (render only).
        l0 = [road.X[i] + u * road.LX[i] for u in (-1.0, 1.0)]
        zz0 = [road.Z[i] + u * road.LZ[i] for u in (-1.0, 1.0)]
        l1 = [road.X[i + 1] + u * road.LX[i + 1] for u in (-1.0, 1.0)]
        zz1 = [road.Z[i + 1] + u * road.LZ[i + 1] for u in (-1.0, 1.0)]
        y = GROUND_Y - 0.03
        out.add_quad(("render",), "tunnel_light", (l0[0], y, zz0[0]), (l0[1], y, zz0[1]),
                     (l1[1], y, zz1[1]), (l1[0], y, zz1[0]), (0, 0), (1, 0), (1, 1), (0, 1), (0, -1, 0))


def emit_slabs(out, road):
    """Deck underside and fascia for elevated ranges (render only)."""
    for i in range(road.N - 1):
        if not (road.ELEV[i] and road.ELEV[i + 1]):
            continue
        ul, ur = road.half_l, -road.half_r
        u0 = [road.top(i, ul), road.top(i, ur)]
        u1 = [road.top(i + 1, ul), road.top(i + 1, ur)]
        dn0 = [(p[0], p[1] - DECK_SLAB, p[2]) for p in u0]
        dn1 = [(p[0], p[1] - DECK_SLAB, p[2]) for p in u1]
        out.add_quad(("render",), "concrete_cast", dn0[0], dn0[1], dn1[1], dn1[0],
                     (0, 0), (0.5, 0), (0.5, 0.5), (0, 0.5), (0, -1, 0))
        for sgn, idx in ((1.0, 0), (-1.0, 1)):
            t0, t1 = u0[idx], u1[idx]
            b0 = dn0[idx]
            b1 = dn1[idx]
            want = (sgn * road.LX[i], 0.0, sgn * road.LZ[i])
            out.add_quad(("render",), "concrete_cast", t0, t1, b1, b0,
                         (0, 0), (0.5, 0), (0.5, 0.5), (0, 0.5), want)


def emit_fill(out, road):
    """1:1 embankment fill from the deck edge down to the ground plane (ground class)."""
    tile = TILE["ground_gravel"]
    ground_plane = GROUND_Y - 0.05
    for i in range(road.N - 1):
        if not (road.EMB[i] and road.EMB[i + 1]):
            continue
        for sgn in (1.0, -1.0):
            ue = road.half_l if sgn > 0 else -road.half_r
            h0 = road.top_y(i, ue) - ground_plane
            h1 = road.top_y(i + 1, ue) - ground_plane
            if h0 < 0.3 or h1 < 0.3:
                continue
            e0 = road.top(i, ue)
            e1 = road.top(i + 1, ue)
            ut0 = ue + sgn * h0
            ut1 = ue + sgn * h1
            t0 = (road.X[i] + ut0 * road.LX[i], ground_plane, road.Z[i] + ut0 * road.LZ[i])
            t1 = (road.X[i + 1] + ut1 * road.LX[i + 1], ground_plane, road.Z[i + 1] + ut1 * road.LZ[i + 1])
            want = (sgn * road.LX[i], 1.0, sgn * road.LZ[i])
            out.add_quad(LAYERS_BOTH, "ground_gravel", e0, t0, t1, e1,
                         (ue / tile, road.S[i] / tile), (ut0 / tile, road.S[i] / tile),
                         (ut1 / tile, road.S[i + 1] / tile), (ue / tile, road.S[i + 1] / tile), want)


def emit_piers(out, road, ctx):
    k = 0
    while True:
        s_target = PIER_SPACING / 2.0 + PIER_SPACING * k
        k += 1
        i0 = int(round(s_target))
        if i0 >= road.N - 1:
            break
        if not road.ELEV[i0]:
            continue
        for off in (0, 4, -4, 8, -8, 12, -12):
            j = clamp(i0 + off, 0, road.N - 1)
            if not road.ELEV[j]:
                continue
            x, z = road.X[j], road.Z[j]
            deck = road.top_y(j, 0.0)
            pier_top = deck - DECK_SLAB
            pier_bot = GROUND_Y - 0.05
            if pier_top - pier_bot < 1.0:
                break
            if ctx.pier_conflict(road, x, z, pier_bot, pier_top):
                continue
            c = (x, (pier_top + pier_bot) * 0.5, z)
            axes = [(road.TX[j], 0.0, road.TZ[j]), (0.0, 1.0, 0.0), (road.LX[j], 0.0, road.LZ[j])]
            half = (PIER_HALF, (pier_top - pier_bot) * 0.5, PIER_HALF)
            out.add_box(LAYERS_BOTH, "concrete_weathered", c, axes, half)
            break


def emit_joints(out, road):
    """Galvanized expansion joints across the deck every 40 m on elevated ranges."""
    k = 1
    while JOINT_SPACING * k < road.S[-1]:
        i = int(round(JOINT_SPACING * k))
        k += 1
        if i + 1 >= road.N or not road.ELEV[i]:
            continue
        a = road.top(i, road.half_l)
        b = road.top(i, -road.half_r)
        c = road.top(i + 1, -road.half_r)
        d = road.top(i + 1, road.half_l)
        lift = (0.0, 0.006, 0.0)
        out.add_quad(("render",), "galvanized", add_v(a, lift), add_v(b, lift), add_v(c, lift), add_v(d, lift),
                     (0, 0), (0, 1), (1, 1), (1, 0), UP)


def emit_noise(out, road):
    """Optional noise screens on the outer edge of elevated ranges (render only)."""
    tile = TILE["noise_barrier"]
    for i in range(road.N - 1):
        if not (road.ELEV[i] and road.ELEV[i + 1]):
            continue
        u = road.half_l + 0.35
        pts = []
        for j in (i, i + 1):
            base = road.top(j, u)
            pts.append((base[0], base[1] + PARAPET_H, base[2]))
            pts.append((base[0], base[1] + PARAPET_H + 2.0, base[2]))
        out_dir = (road.LX[i], 0.0, road.LZ[i])
        p0, p1, p2, p3 = pts[0], pts[1], pts[3], pts[2]
        q0, q1, q2, q3 = pts[2], pts[3], pts[1], pts[0]
        for quad, want in (((p0, p1, p2, p3), out_dir), ((q0, q1, q2, q3), (-out_dir[0], 0.0, -out_dir[2]))):
            out.add_quad(("render",), "noise_barrier", quad[0], quad[1], quad[2], quad[3],
                         (road.S[i] / tile, 0), (road.S[i] / tile, 1), (road.S[i + 1] / tile, 1),
                         (road.S[i + 1] / tile, 0), want)


def emit_section_ends(out, road, bbox):
    """Wall across the carriageway where it leaves (or enters) the built section, so the car cannot
    drive off the end of the generated geometry. Render + collision, 3 m tall."""
    if bbox is None:
        return
    x0, z0, x1, z1 = bbox

    def inside(i):
        return x0 <= road.X[i] <= x1 and z0 <= road.Z[i] <= z1
    for i in range(road.N - 1):
        a, b = inside(i), inside(i + 1)
        if a == b:
            continue
        k = i if a else i + 1
        l = road.top(k, road.HL[k] + 0.3)
        r = road.top(k, -road.HR[k] - 0.3)
        h = 3.0
        fwd = (road.TX[k], 0.0, road.TZ[k]) if a else (-road.TX[k], 0.0, -road.TZ[k])
        n = (-fwd[0], 0.0, -fwd[2])  # faces the oncoming car
        out.add_quad(LAYERS_BOTH, "concrete_cast", r, l, (l[0], l[1] + h, l[2]), (r[0], r[1] + h, r[2]),
                     (0, 0), (1, 0), (1, 1), (0, 1), n)
        out.add_quad(LAYERS_BOTH, "concrete_cast", l, r, (r[0], r[1] + h, r[2]), (l[0], l[1] + h, l[2]),
                     (0, 0), (1, 0), (1, 1), (0, 1), fwd)


def emit_tunnel_lights(road, lamps, spacing=20.0):
    """Light entries along tunnel ceilings (the emissive strip is the visible fitting)."""
    k = 0
    while True:
        i = int(round(spacing * (k + 0.5)))
        k += 1
        if i >= road.N:
            break
        if not road.ROOF[i]:
            continue
        lamps.append({
            "carriageway": road.id, "kind": "tunnel",
            "pos": [round(road.X[i], 3), round(GROUND_Y - 0.25, 3), round(road.Z[i], 3)],
            "dir": [0.0, -1.0, 0.0], "color": [1.0, 0.62, 0.3], "intensity": 8.0, "range": 14.0,
        })


def emit_poles(out, road, lamps):
    """Light poles every 35 m on the left edge, lamp heads emissive; json entries for the light system."""
    k = 0
    while True:
        i = int(round(POLE_SPACING / 2.0 + POLE_SPACING * k))
        k += 1
        if i >= road.N:
            break
        if road.ROOF[i]:
            continue
        if road.ELEV[i]:
            u_base = road.half_l + 0.1
            base = road.top(i, u_base)
            base_y = base[1] + PARAPET_H
            height = 6.0
        else:
            u_base = road.half_l + 0.6
            base = road.top(i, u_base)
            base_y = base[1]
            height = 8.0
        px, pz = base[0], base[2]
        axes = [(road.TX[i], 0.0, road.TZ[i]), (0.0, 1.0, 0.0), (road.LX[i], 0.0, road.LZ[i])]
        out.add_box(("render",), "galvanized", (px, base_y + height * 0.5, pz), axes,
                    (0.125, height * 0.5, 0.125))
        u_end = road.half_l - 3.0
        arm_y = base_y + height - 0.3
        um = 0.5 * (u_base + u_end)
        ax, az = road.X[i] + um * road.LX[i], road.Z[i] + um * road.LZ[i]
        out.add_box(("render",), "galvanized", (ax, arm_y, az), axes,
                    (0.09, 0.09, abs(u_base - u_end) * 0.5))
        hx, hz = road.X[i] + u_end * road.LX[i], road.Z[i] + u_end * road.LZ[i]
        head_y = arm_y - 0.2
        out.add_box(("render",), "lamp", (hx, head_y, hz), axes, (0.3, 0.12, 0.2))
        dx, dz = -road.LX[i] * 0.3, -road.LZ[i] * 0.3
        dl = math.sqrt(dx * dx + 1.0 + dz * dz)
        lamps.append({
            "carriageway": road.id,
            "pos": [round(hx, 3), round(head_y - 0.12, 3), round(hz, 3)],
            "dir": [round(dx / dl, 4), round(-1.0 / dl, 4), round(dz / dl, 4)],
            "color": [1.0, 0.85, 0.55],
            "intensity": 12.0,
            "range": 30.0,
        })


def emit_ground(out, ctx, bbox_cells, gy):
    tile = TILE["ground_gravel"]
    x0, z0, x1, z1 = bbox_cells
    cells = 0
    roof = 0
    ix = x0
    while ix < x1:
        iz = z0
        while iz < z1:
            cx = ix + CELL * 0.5
            cz = iz + CELL * 0.5
            cover = ctx.cell_covered(cx, cz)
            if cover == "open":
                iz += CELL
                continue
            if cover == "roof":
                y = GROUND_Y
                layers = ("render",)
                roof += 1
            else:
                y = gy
                layers = LAYERS_BOTH
                cells += 1
            p0 = (ix, y, iz)
            p1 = (ix, y, iz + CELL)
            p2 = (ix + CELL, y, iz + CELL)
            p3 = (ix + CELL, y, iz)
            out.add_quad(layers, "ground_gravel", p0, p1, p2, p3,
                         (ix / tile, iz / tile), (ix / tile, (iz + CELL) / tile),
                         ((ix + CELL) / tile, (iz + CELL) / tile), ((ix + CELL) / tile, iz / tile), UP)
            iz += CELL
        ix += CELL
    return cells, roof


# ----------------------------------------------------------------------------------------------
# Blender output
# ----------------------------------------------------------------------------------------------
GROUND_Y = 3.0
TILE = {}


def load_manifest():
    with open(MANIFEST, "r", encoding="utf-8") as f:
        man = json.load(f)
    info = {}
    for a in man["assets"]:
        info[a["id"]] = a
    for key, (aid, seamless) in MATERIALS.items():
        a = info[aid]
        TILE[key] = float(a.get("suggested_tile_meters", [1.0, 1.0])[0])
    return info


def bl(p):
    return (p[0], -p[2], p[1])


def make_materials(info):
    mats = {}
    for key, (aid, seamless) in MATERIALS.items():
        a = info[aid]
        fname = a.get("seamless_file") if (seamless and a.get("seamless_file")) else a["file"]
        path = os.path.join(TEX_DIR, fname)
        mats[key] = make_material(key, path, float(a["roughness"]), float(a["metallic"]))
    mats["paint"] = make_material("paint", None, 0.6, 0.0, color=(0.95, 0.95, 0.92, 1.0))
    mats["lamp"] = make_material("lamp", None, 0.4, 0.0, color=(0.1, 0.1, 0.1, 1.0),
                                 emit=(1.0, 0.85, 0.55, 1.0), emit_strength=12.0)
    mats["tunnel_light"] = make_material("tunnel_light", None, 0.4, 0.0, color=(0.1, 0.1, 0.1, 1.0),
                                         emit=(0.85, 0.95, 1.0, 1.0), emit_strength=4.0)
    return mats


def make_material(name, tex_path, rough, metal, color=(1.0, 1.0, 1.0, 1.0), emit=None, emit_strength=0.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    if tex_path:
        img = bpy.data.images.load(tex_path, check_existing=True)
        tn = nt.nodes.new("ShaderNodeTexImage")
        tn.image = img
        tn.extension = "REPEAT"
        nt.links.new(tn.outputs["Color"], bsdf.inputs["Base Color"])
    else:
        bsdf.inputs["Base Color"].default_value = color
    if emit is not None:
        bsdf.inputs["Emission Color"].default_value = emit
        bsdf.inputs["Emission Strength"].default_value = emit_strength
    return mat


def make_blender_object(name, tris, mat):
    verts = []
    faces = []
    uvs = []
    for pts, uv in tris:
        base = len(verts)
        for p in pts:
            verts.append(bl(p))
        uvs.extend(uv)
        faces.append((base, base + 1, base + 2))
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    me.update()
    layer = me.uv_layers.new(name="UVMap")
    for li in range(len(me.loops)):
        layer.data[li].uv = uvs[li]
    if mat is not None:
        me.materials.append(mat)
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def export_objects(objs, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_yup=True,
                              export_image_format="AUTO", export_cameras=False, export_lights=False,
                              export_animations=False)


def resolve(p):
    return p if os.path.isabs(p) else os.path.join(ROOT, p)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--network", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--bbox", default=None, help="xmin,zmin,xmax,zmax")
    ap.add_argument("--chunk", type=float, default=500.0)
    ap.add_argument("--noise-barriers", action="store_true")
    ap.add_argument("--no-blend", action="store_true")
    args = ap.parse_args(argv)

    if bpy is None:
        raise SystemExit("build_roads.py must run inside Blender (bpy missing)")

    net_path = resolve(args.network)
    out_dir = resolve(args.out)
    name = os.path.basename(os.path.normpath(out_dir))
    with open(net_path, "r", encoding="utf-8") as f:
        net = json.load(f)
    global GROUND_Y
    GROUND_Y = float(net.get("ground_y", 3.0))

    bbox = None
    if args.bbox:
        bbox = tuple(float(v) for v in args.bbox.split(","))

    info = load_manifest()
    log("network %s: %d carriageways" % (net_path, len(net["carriageways"])))
    # PA lot service roads are not yet connected to the carriageway graph (data gap): they would
    # stand as isolated decks inside junction loops, so they are not generated.
    roads = [Road(cw) for cw in net["carriageways"] if cw.get("route") != "PA"]
    if bbox is not None:
        x0, z0, x1, z1 = bbox
        roads = [r for r in roads if any(x0 - 20 <= r.X[i] <= x1 + 20 and z0 - 20 <= r.Z[i] <= z1 + 20
                                         for i in range(0, r.N, 5))]
    ctx = Context(roads)
    ctx.compute_edges()
    out = Output(args.chunk, bbox)
    lamps = []

    # Ground extent from the network (plus margin), or the bbox.
    if bbox is not None:
        gx0, gz0, gx1, gz1 = bbox
    else:
        xs = [v for r in roads for v in r.X]
        zs = [v for r in roads for v in r.Z]
        gx0, gz0, gx1, gz1 = min(xs) - 40, min(zs) - 40, max(xs) + 40, max(zs) + 40
    cells = (math.floor(gx0 / CELL) * CELL, math.floor(gz0 / CELL) * CELL,
             math.ceil(gx1 / CELL) * CELL, math.ceil(gz1 / CELL) * CELL)

    log("emitting %d roads" % len(roads))
    for r in roads:
        emit_deck(out, r)
        emit_markings(out, r)
        emit_barriers(out, r, ctx)
        emit_tunnel(out, r, ctx)
        emit_slabs(out, r)
        emit_fill(out, r)
        emit_piers(out, r, ctx)
        emit_joints(out, r)
        emit_poles(out, r, lamps)
        emit_tunnel_lights(r, lamps)
        emit_section_ends(out, r, bbox)
        if args.noise_barriers:
            emit_noise(out, r)
    n_cells, n_roof = emit_ground(out, ctx, cells, GROUND_Y - 0.05)
    log("ground cells %d, roof cells %d" % (n_cells, n_roof))

    info_mats = make_materials(info)
    # Clear default scene.
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    chunks = defaultdict(lambda: {"render": 0, "col": 0, "min": [1e18, 1e18], "max": [-1e18, -1e18],
                                  "render_objs": [], "col_objs": []})
    for (cx, cz, layer, mat), tris in sorted(out.tris.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2], str(kv[0][3]))):
        ch = chunks[(cx, cz)]
        ch[layer] += len(tris)
        for pts, _ in tris:
            for p in pts:
                ch["min"][0] = min(ch["min"][0], p[0])
                ch["min"][1] = min(ch["min"][1], p[2])
                ch["max"][0] = max(ch["max"][0], p[0])
                ch["max"][1] = max(ch["max"][1], p[2])
        if layer == "render":
            objname = "r_%d_%d_%s" % (cx, cz, mat)
            ob = make_blender_object(objname, tris, info_mats[mat])
            ch["render_objs"].append(ob)
        else:
            objname = "c_%d_%d" % (cx, cz)
            ob = make_blender_object(objname + "_%s" % mat, tris, None)
            ch["col_objs"].append(ob)

    chunk_meta = []
    total_render = 0
    total_col = 0
    for (cx, cz) in sorted(chunks.keys()):
        ch = chunks[(cx, cz)]
        base = "%d_%d" % (cx, cz)
        rel_r = "render/%s.glb" % base
        rel_c = "collision/%s_col.glb" % base
        if ch["render_objs"]:
            export_objects(ch["render_objs"], os.path.join(out_dir, rel_r))
        if ch["col_objs"]:
            export_objects(ch["col_objs"], os.path.join(out_dir, rel_c))
        total_render += ch["render"]
        total_col += ch["col"]
        chunk_meta.append({
            "cx": cx, "cz": cz,
            "bounds_m": [cx * args.chunk, cz * args.chunk, (cx + 1) * args.chunk, (cz + 1) * args.chunk],
            "extent_xz": [round(ch["min"][0], 2), round(ch["min"][1], 2), round(ch["max"][0], 2), round(ch["max"][1], 2)],
            "render": rel_r if ch["render_objs"] else None,
            "collision": rel_c if ch["col_objs"] else None,
            "render_tris": ch["render"],
            "collision_tris": ch["col"],
        })
        log("chunk %s render_tris=%d collision_tris=%d" % (base, ch["render"], ch["col"]))

    os.makedirs(out_dir, exist_ok=True)
    meta = {
        "format": "nox-shuto-chunks",
        "version": 1,
        "name": name,
        "network": os.path.relpath(net_path, ROOT).replace("\\", "/"),
        "chunk_m": args.chunk,
        "ground_y": GROUND_Y,
        "bbox": list(bbox) if bbox else None,
        "noise_barriers": bool(args.noise_barriers),
        "carriageways": [r.id for r in roads],
        "chunks": chunk_meta,
        "totals": {"render_tris": total_render, "collision_tris": total_col, "lamps": len(lamps)},
        "materials": {k: MATERIALS[k][0] for k in MATERIALS},
    }
    with open(os.path.join(out_dir, "chunks.json"), "w", encoding="ascii", newline="\n") as f:
        json.dump(meta, f, indent=1)
        f.write("\n")
    with open(os.path.join(out_dir, "lamps.json"), "w", encoding="ascii", newline="\n") as f:
        json.dump({"format": "nox-shuto-lamps", "version": 1, "lamps": [
            dict(id=i, **l) for i, l in enumerate(lamps)]}, f, indent=1)
        f.write("\n")

    if not args.no_blend:
        src_dir = os.path.join(ROOT, "models", "shuto", "src")
        os.makedirs(src_dir, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(src_dir, name + ".blend"), check_existing=False)
    log("done: render_tris=%d collision_tris=%d lamps=%d" % (total_render, total_col, len(lamps)))


if __name__ == "__main__":
    main()
