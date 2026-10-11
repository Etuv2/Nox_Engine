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
POLE_H = 9.6
CELL = 20.0
GORE_FILL = 1.5   # m: same-level diverge/merge gaps narrower than this are paved (gore)
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
def run_ranges(flags, S):
    """Per sample: (s0, s1) of the contiguous run of True flags containing it, else None."""
    n = len(flags)
    out = [None] * n
    i = 0
    while i < n:
        if not flags[i]:
            i += 1
            continue
        j = i
        while j + 1 < n and flags[j + 1]:
            j += 1
        s1 = S[j + 1] if j + 1 < n else S[j]
        for k in range(i, j + 1):
            out[k] = (S[i], s1)
        i = j + 1
    return out


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
        # Effective structure type per sample. The contract's tunnel/cut ranges are checked against the
        # solved profile: a "tunnel" whose deck is above street level is an embankment (fill, no
        # tunnel walls or roof); a roofed tunnel needs 5 m of clearance under the cover, else it is an
        # open cut (retaining walls, no roof).
        self.ST = []
        for i, s in enumerate(self.S):
            t = self.type_at(s)
            if t in TUNNEL_TYPES:
                if self.Y[i] + self.eps > GROUND_Y + 1.0:
                    t = "embankment"
                elif t in ROOF_TYPES and self.Y[i] + 5.0 > GROUND_Y:
                    t = "cut"
            elif t not in ELEVATED_TYPES and self.Y[i] + self.eps < GROUND_Y - 0.5:
                # Tagged at grade/embankment but the solved deck is below street level (ramps
                # descending into a trench): open cut, so the street ground is removed above it and
                # retaining walls are built instead of the car meeting the ground plane.
                t = "cut"
            self.ST.append(t)
        self.COV = [t in TUNNEL_TYPES for t in self.ST]
        self.ROOF = [t in ROOF_TYPES for t in self.ST]
        self.ELEV = [t in ELEVATED_TYPES for t in self.ST]
        self.EMB = [t == "embankment" for t in self.ST]
        self.cov_range = run_ranges(self.COV, self.S)
        self.cov_roof = run_ranges(self.ROOF, self.S)

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
        widened = 0
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
                    if dot > 0.7 and dy < 0.3:
                        # Diverging/merging same-level carriageways: where the gap between the two
                        # decks is narrower than GORE_FILL the gore is paved (both decks widen to meet),
                        # so the barrier nose starts only once the roadways have really separated
                        # (no blunt barrier end in a sliver next to live lanes).
                        lat, lon = road.lateral(i, r.X[si], r.Z[si])
                        if abs(lon) > 1.0:
                            continue
                        if lat > 0.0:
                            gap = lat - road.half_l - r.half_r
                            if 0.0 < gap < GORE_FILL:
                                road.HL[i] = max(road.HL[i], road.half_l + gap * 0.5 + 0.05)
                                widened += 1
                        else:
                            gap = -lat - road.half_r - r.half_l
                            if 0.0 < gap < GORE_FILL:
                                road.HR[i] = max(road.HR[i], road.half_r + gap * 0.5 + 0.05)
                                widened += 1
                        continue
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
        log("median clipping: %d samples, gore fill: %d samples" % (clipped, widened))

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
            # At a carriageway's first/last sample (node joins), cover 2 m beyond the end so the last
            # barrier stubs of the roads meeting at that node are opened inside the joined deck.
            prev_gap = r.S[si] - r.S[si - 1] if si > 0 else 4.0
            next_gap = r.S[si + 1] - r.S[si] if si + 1 < r.N else 4.0
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
            # Any other deck between a little below the pier foot (at-grade/ramp roads can sit just
            # under the street ground) and the pier top blocks the pier.
            if ytop - DECK_SLAB < yhi and ytop > ylo - 1.5:
                return True
        return False

    def cell_covered(self, x, z):
        """None (normal ground), "roof" (tunnel below: render-only roof) or "open" (open trench, or an
        at-grade deck at street level: no ground plane there, the road deck itself is the surface)."""
        # At-grade decks near street level (ramp ends meeting the city streets): the ground plane must
        # not cover the asphalt (it would sit a few cm above it and turn the road's own parapets into
        # obstacles for a car riding on the ground).
        for ri, si in self.grid.near(x, z, 12.0):
            r = self.roads[ri]
            if r.ELEV[si] or r.COV[si] or abs(r.Y[si] - GROUND_Y) > 0.6:
                continue
            lat, along = r.lateral(si, x, z)
            gap = r.S[si + 1] - r.S[si] if si + 1 < r.N else 1.0
            if -r.HR[si] - 0.3 <= lat <= r.HL[si] + 0.3 and abs(along) <= gap:
                return "open"
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
DECK_RANK = {"main": 0, "jct": 0, "pa": 0, "toll": 0, "ramp_on": 1, "ramp_off": 1}
Z_LIFT_RENDER = 0.015  # render-only lift for a deck lying over another same-level deck (no z-fighting)


def deck_overlapped(ctx, road, i):
    """True when another carriageway's deck lies over this sample at the same level (within 5 cm) and
    this deck is the lower-priority one (ramp under main; equal rank: the higher id). Collision is never
    changed by this: the overlapped deck keeps its full collision and only its render surface is lifted."""
    x, z = road.X[i], road.Z[i]
    y = road.top_y(i, 0.0)
    for r, si, lat, ytop in ctx.footprints(x, z, 0.0, exclude=road):
        if abs(ytop - y) >= 0.05:
            continue
        rr, rs = DECK_RANK.get(r.kind, 0), DECK_RANK.get(road.kind, 0)
        if rr < rs or (rr == rs and r.id < road.id):
            return True
    return False


def emit_deck(out, road, ctx):
    tile = TILE["asphalt"]
    for i in range(road.N - 1):
        l0 = road.top(i, road.HL[i])
        r0 = road.top(i, -road.HR[i])
        l1 = road.top(i + 1, road.HL[i + 1])
        r1 = road.top(i + 1, -road.HR[i + 1])
        s0 = road.S[i] / tile
        s1 = road.S[i + 1] / tile
        uv = [(road.HL[i] / tile, s0), (-road.HR[i] / tile, s0), (-road.HR[i + 1] / tile, s1), (road.HL[i + 1] / tile, s1)]
        if deck_overlapped(ctx, road, i):
            lift = (0.0, Z_LIFT_RENDER, 0.0)
            out.add_quad(("render",), "asphalt", add_v(l0, lift), add_v(r0, lift), add_v(r1, lift), add_v(l1, lift),
                         uv[0], uv[1], uv[2], uv[3], UP)
            out.add_quad(("col",), "asphalt", l0, r0, r1, l1, uv[0], uv[1], uv[2], uv[3], UP)
        else:
            out.add_quad(LAYERS_BOTH, "asphalt", l0, r0, r1, l1, uv[0], uv[1], uv[2], uv[3], UP)


KIND_RANK = {"main": 3, "jct": 2, "ramp_on": 1, "ramp_off": 1, "pa": 0, "toll": 0}


def marking_priority(road):
    """Which carriageway owns the markings where two decks overlap (merges/diverges): main lines
    over junction connectors over ramps, then more lanes, then a stable id order."""
    return (KIND_RANK.get(road.kind, 1), road.lanes, road.id)


def deck_owner_above(ctx, road, x, y, z):
    """The higher-priority carriageway whose same-level driving area contains (x, z), or None."""
    mine = marking_priority(road)
    best = None
    for r, si, lat, ytop in ctx.footprints(x, z, 0.0, exclude=road):
        if abs(ytop - y) < 1.5 and marking_priority(r) > mine:
            if best is None or marking_priority(r) > marking_priority(best):
                best = r
    return best


def lower_deck_alongside(ctx, road, x, y, z):
    """True when a lower-priority same-level deck covers (x, z): a lane joining or leaving here."""
    mine = marking_priority(road)
    for r, si, lat, ytop in ctx.footprints(x, z, 0.0, exclude=road):
        if abs(ytop - y) < 1.5 and marking_priority(r) < mine:
            return True
    return False


def emit_markings(out, road, ctx):
    """Japanese expressway markings (white):
      * lane dividers: 8 m dash / 12 m gap, 0.15 m;
      * edge lines: solid 0.15 m; where a lower-priority carriageway joins or leaves alongside (merge
        or diverge taper), the edge line becomes the thick dashed lane-addition line (0.45 m, 6/6 m);
      * a carriageway's markings are dropped inside a higher-priority deck at the same level, so the
        main line's markings run continuously through merges and diverges and the joining ramp's
        lines end where its lane becomes part of the main line;
      * paved gores (deck widened by GORE_FILL) get diagonal hatching.
    """
    half_edge = road.lanes * road.lw * 0.5
    dividers = [-(road.lanes / 2.0 - k) * road.lw for k in range(1, road.lanes)]
    lift = (0.0, 0.01, 0.0)

    def strip(i, u0, u1, ua=None, ub=None):
        a0 = road.top(i, u0 if ua is None else ua[0])
        a1 = road.top(i, u1 if ua is None else ua[1])
        b0 = road.top(i + 1, u0 if ub is None else ub[0])
        b1 = road.top(i + 1, u1 if ub is None else ub[1])
        out.add_quad(LAYERS_BOTH[:1], "paint", add_v(a0, lift), add_v(a1, lift), add_v(b1, lift),
                     add_v(b0, lift), (0, 0), (1, 0), (1, 1), (0, 1), UP)

    def owned_by_other(i, u):
        ca = road.top(i, u)
        cb = road.top(i + 1, u)
        mx, my, mz = 0.5 * (ca[0] + cb[0]), 0.5 * (ca[1] + cb[1]), 0.5 * (ca[2] + cb[2])
        return deck_owner_above(ctx, road, mx, my, mz) is not None

    # Lane dividers.
    for u in dividers:
        for i in range(road.N - 1):
            if road.S[i] % 20.0 >= 8.0 or owned_by_other(i, u):
                continue
            strip(i, u - 0.075, u + 0.075)
    # Edge lines (solid, or thick dashed along a joining/leaving lane).
    for sgn in (1.0, -1.0):
        u = sgn * (half_edge - 0.25)
        for i in range(road.N - 1):
            if owned_by_other(i, u):
                continue
            # Probe just outside the edge line: a lower-priority deck there means a lane is joining.
            probe = road.top(i, sgn * (half_edge + 1.0))
            if lower_deck_alongside(ctx, road, probe[0], probe[1], probe[2]):
                if road.S[i] % 12.0 >= 6.0:
                    continue
                strip(i, u - 0.225, u + 0.225)
            else:
                strip(i, u - 0.075, u + 0.075)
    # Gore hatching inside the paved fill band (outside the edge line).
    for sgn, H, half in ((1.0, road.HL, road.half_l), (-1.0, road.HR, road.half_r)):
        for i in range(road.N - 1):
            if H[i] < half + 0.3 or H[i + 1] < half + 0.3:
                continue
            if road.S[i] % 4.0 >= 1.0:
                continue
            inner = half_edge - 0.25 + 0.3
            outer_i, outer_n = H[i] - 0.15, H[i + 1] - 0.15
            # Diagonal stripe: from the edge line at sample i to the deck edge at sample i+1.
            strip(i, 0.0, 0.0, ua=(sgn * inner, sgn * (inner + 0.45)), ub=(sgn * (outer_n - 0.45), sgn * outer_n))


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


def emit_portals(out, road):
    """Portal face at each end of a roofed tunnel: a lintel from 5 m above the deck up to the cover
    (ground) level, across the carriageway and its walls. The ground continues over the roof."""
    tile = TILE["tunnel_tiles"]
    for i in range(road.N - 1):
        start = road.ROOF[i] and (i == 0 or not road.ROOF[i - 1])
        end = road.ROOF[i] and (i + 1 >= road.N or not road.ROOF[i + 1])
        if not (start or end):
            continue
        ul, ur = road.HL[i] + 0.3, -(road.HR[i] + 0.3)
        b_l = road.top_y(i, ul) + 5.0
        b_r = road.top_y(i, ur) + 5.0
        if min(b_l, b_r) >= GROUND_Y - 0.2:
            continue
        pl = (road.X[i] + ul * road.LX[i], b_l, road.Z[i] + ul * road.LZ[i])
        pr = (road.X[i] + ur * road.LX[i], b_r, road.Z[i] + ur * road.LZ[i])
        tl = (pl[0], GROUND_Y, pl[2])
        tr = (pr[0], GROUND_Y, pr[2])
        sgn = -1.0 if start else 1.0          # the face looks back toward the approaching car
        want = (sgn * road.TX[i], 0.0, sgn * road.TZ[i])
        out.add_quad(("render", "col"), "tunnel_tiles", pl, pr, tr, tl,
                     (0, 0), (WALL_HALF / tile, 0), (WALL_HALF / tile, 1), (0, 1), want)


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
        # 0.25 m wide strip (samples are 1 m apart), across the effective deck width.
        def at(t, u):
            p0, p1 = road.top(i, u), road.top(i + 1, u)
            return tuple(p0[k] + (p1[k] - p0[k]) * t for k in range(3))
        a = at(0.0, road.HL[i])
        b = at(0.0, -road.HR[i])
        c = at(0.25, -road.HR[i])
        d = at(0.25, road.HL[i])
        lift = (0.0, 0.006, 0.0)
        out.add_quad(("render",), "joint", add_v(a, lift), add_v(b, lift), add_v(c, lift), add_v(d, lift),
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


def emit_tunnel_fixtures(out, road, lamps, spacing=20.0):
    """Tunnel light fixtures: one rectangular housing per lane hung from the ceiling every 20 m, with
    an emissive lens on its underside. The lamp entry is the lens centre (luminaire position)."""
    tile_lens = (0.6, 0.15)
    k = 0
    while True:
        i = int(round(spacing * (k + 0.5)))
        k += 1
        if i >= road.N:
            break
        if not road.ROOF[i]:
            continue
        # keep fixtures 6 m clear of the tunnel ends (the portal lintels and ceiling edges)
        lo = road.cov_roof[i][0]
        hi = road.cov_roof[i][1]
        if road.S[i] - lo < 6.0 or hi - road.S[i] < 6.0:
            continue
        axes = [(road.TX[i], 0.0, road.TZ[i]), (0.0, 1.0, 0.0), (road.LX[i], 0.0, road.LZ[i])]
        for lane in range(road.lanes):
            u = -(road.lanes / 2.0 - lane - 0.5) * road.lw
            cx, cz = road.X[i] + u * road.LX[i], road.Z[i] + u * road.LZ[i]
            # housing: 1.2 m x 0.3 m x 0.1 m, underside at 0.1 m below the ceiling
            out.add_box(("render",), "galvanized", (cx, GROUND_Y - 0.05, cz), axes, (0.6, 0.05, 0.15))
            lens_y = GROUND_Y - 0.1 - 0.004
            l0 = (cx - 0.6 * road.TX[i] - tile_lens[1] * road.LX[i], lens_y, cz - 0.6 * road.TZ[i] - tile_lens[1] * road.LZ[i])
            l1 = (cx + 0.6 * road.TX[i] - tile_lens[1] * road.LX[i], lens_y, cz + 0.6 * road.TZ[i] - tile_lens[1] * road.LZ[i])
            l2 = (cx + 0.6 * road.TX[i] + tile_lens[1] * road.LX[i], lens_y, cz + 0.6 * road.TZ[i] + tile_lens[1] * road.LZ[i])
            l3 = (cx - 0.6 * road.TX[i] + tile_lens[1] * road.LX[i], lens_y, cz - 0.6 * road.TZ[i] + tile_lens[1] * road.LZ[i])
            out.add_quad(("render",), "tunnel_light", l0, l1, l2, l3, (0, 0), (1, 0), (1, 1), (0, 1), (0, -1, 0))
            lamps.append({
                "carriageway": road.id, "kind": "tunnel",
                "pos": [round(cx, 3), round(lens_y, 3), round(cz, 3)],
                "dir": [0.0, -1.0, 0.0], "color": [0.85, 0.95, 1.0], "intensity": 8.0, "range": 14.0,
            })


def add_prism(out, mat, a, b, r0, r1, sides=8):
    """Tapered prism (closed, with caps) from point a (radius r0) to b (radius r1), render only."""
    dx, dy, dz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    ln = math.sqrt(dx * dx + dy * dy + dz * dz)
    if ln < 1e-6:
        return
    d = (dx / ln, dy / ln, dz / ln)
    ref = (1.0, 0.0, 0.0) if abs(d[0]) < 0.9 else (0.0, 0.0, 1.0)
    u = (d[1] * ref[2] - d[2] * ref[1], d[2] * ref[0] - d[0] * ref[2], d[0] * ref[1] - d[1] * ref[0])
    ul = math.sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2])
    u = (u[0] / ul, u[1] / ul, u[2] / ul)
    v = (d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0])
    ring_a, ring_b, dirs = [], [], []
    for k in range(sides):
        t = 2.0 * math.pi * k / sides
        c, s = math.cos(t), math.sin(t)
        n = (c * u[0] + s * v[0], c * u[1] + s * v[1], c * u[2] + s * v[2])
        dirs.append(n)
        ring_a.append(add_v(a, n, r0))
        ring_b.append(add_v(b, n, r1))
    tile = TILE.get("galvanized", 1.0)
    for k in range(sides):
        k1 = (k + 1) % sides
        mid = (dirs[k][0] + dirs[k1][0], dirs[k][1] + dirs[k1][1], dirs[k][2] + dirs[k1][2])
        out.add_quad(("render",), mat, ring_a[k], ring_b[k], ring_b[k1], ring_a[k1],
                     (k / sides, 0.0), (k / sides, ln / tile), (k1 / sides, ln / tile), (k1 / sides, 0.0), mid)
    out.add_tri(("render",), mat, (ring_b[0], ring_b[1], b), ((0, 0), (1, 0), (0.5, 1)), d)
    out.add_tri(("render",), mat, (ring_a[0], ring_a[1], a), ((0, 0), (1, 0), (0.5, 1)),
                (-d[0], -d[1], -d[2]))


def barrier_omitted(road, ctx, i, sgn):
    """True when emit_barriers leaves this sample's parapet out (footprint opening)."""
    edge = road.HL if sgn > 0 else [-v for v in road.HR]
    ex, ey, ez = road.top(i, edge[i])
    fx, fy, fz = road.top(i + 1, edge[i + 1])
    return (ctx.blocked(road, ex, ey, ez) or ctx.blocked(road, fx, fy, fz) or
            ctx.blocked(road, 0.5 * (ex + fx), 0.5 * (ey + fy), 0.5 * (ez + fz)))


def pole_site(road, i, ctx, bbox):
    """Support for a lamp pole at sample i on the left edge, or None. Poles stand on the parapet cap of
    an elevated deck, on the ground beyond the toe of an embankment, or on the ground beside an
    at-grade road. Never on a gap (omitted parapet, clipped median edge, roof, section end) and never
    inside another carriageway's footprint."""
    if i <= 0 or i >= road.N - 2 or road.ROOF[i] or road.COV[i] and not road.EMB[i]:
        return None
    if road.HL[i] < road.half_l - 0.05:
        return None  # clipped median edge
    if bbox is not None:
        x0, z0, x1, z1 = bbox
        if not (x0 + 15.0 <= road.X[i] <= x1 - 15.0 and z0 + 15.0 <= road.Z[i] <= z1 - 15.0):
            return None
    gy = GROUND_Y - 0.05
    if road.ELEV[i]:
        if barrier_omitted(road, ctx, i, 1.0):
            return None
        u = road.half_l + 0.1
        x, y, z = road.top(i, u)
        if ctx.footprints(x, z, 0.5, exclude=road):
            return None
        return (i, "parapet", (x, y + PARAPET_H, z), u)
    deck = road.top_y(i, 0.0)
    if road.EMB[i]:
        h = road.top_y(i, road.half_l) - gy
        if h > 14.0:
            return None
        u = road.half_l + h + 1.0
    elif deck - gy < 1.0:
        # Beyond the street-level hole cut around at-grade decks (HL + 0.3 m plus up to one
        # ground-quadtree leaf), so the pole stands on ground.
        u = road.half_l + 2.0
    else:
        return None
    x = road.X[i] + u * road.LX[i]
    z = road.Z[i] + u * road.LZ[i]
    if ctx.footprints(x, z, 0.5, exclude=road) or ctx.cell_covered(x, z) is not None:
        return None
    return (i, "ground", (x, gy, z), u)


def emit_poles(out, road, ctx, lamps, bbox):
    """Street lamps every 35 m on the left edge: base plate, tapered 9.6 m shaft, curved arm over the
    carriageway and a rectangular housing with an emissive sodium lens. The lamp entry is the lens."""
    k = 0
    while True:
        i0 = int(round(POLE_SPACING / 2.0 + POLE_SPACING * k))
        k += 1
        if i0 >= road.N:
            break
        site = None
        for off in (0, 3, -3, 6, -6, 9, -9):
            if 0 <= i0 + off < road.N:
                site = pole_site(road, i0 + off, ctx, bbox)
                if site is not None:
                    break
        if site is None:
            continue
        i, kind, base, u_pole = site
        px, py, pz = base
        axes = [(road.TX[i], 0.0, road.TZ[i]), (0.0, 1.0, 0.0), (road.LX[i], 0.0, road.LZ[i])]
        out.add_box(("render",), "galvanized", (px, py + 0.03, pz), axes, (0.3, 0.03, 0.3))
        top = py + POLE_H
        add_prism(out, "galvanized", (px, py + 0.06, pz), (px, top, pz), 0.16, 0.08, sides=8)
        # curved arm: five tube segments from the pole top toward the carriageway
        u_end = road.half_l - 1.2
        pts = []
        for t in [j / 4.0 for j in range(5)]:
            uu = u_pole + (u_end - u_pole) * t
            pts.append((road.X[i] + uu * road.LX[i], top - 0.35 * t * t, road.Z[i] + uu * road.LZ[i]))
        for a, b in zip(pts[:-1], pts[1:]):
            add_prism(out, "galvanized", a, b, 0.07, 0.06, sides=6)
        ex, ey, ez = pts[-1]
        housing_top = ey - 0.14
        add_prism(out, "galvanized", (ex, ey, ez), (ex, housing_top, ez), 0.05, 0.05, sides=6)
        out.add_box(("render",), "galvanized", (ex, housing_top - 0.06, ez), axes, (0.4, 0.06, 0.2))
        lens_y = housing_top - 0.12 - 0.003
        lens = [(ex + sx * 0.36 * road.TX[i] + sz * 0.15 * road.LX[i], lens_y, ez + sx * 0.36 * road.TZ[i] + sz * 0.15 * road.LZ[i])
                for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        out.add_quad(("render",), "lamp", lens[0], lens[1], lens[2], lens[3], (0, 0), (1, 0), (1, 1), (0, 1), (0, -1, 0))
        dx, dz = -road.LX[i] * 0.3, -road.LZ[i] * 0.3
        dl = math.sqrt(dx * dx + 1.0 + dz * dz)
        lamps.append({
            "carriageway": road.id, "kind": "road", "support": kind,
            "base": [round(px, 3), round(py, 3), round(pz, 3)],
            "pos": [round(ex, 3), round(lens_y, 3), round(ez, 3)],
            "dir": [round(dx / dl, 4), round(-1.0 / dl, 4), round(dz / dl, 4)],
            "color": [1.0, 0.56, 0.2],
            "intensity": 12.0,
            "range": 30.0,
        })


def emit_ground(out, ctx, bbox_cells, gy):
    """Street-level ground plane with holes over open trenches and render-only roofs over tunnels.

    Cells are refined quadtree-style (CELL down to ~1.25 m) wherever a 3x3 sample grid is not
    uniformly clear, and a leaf is only kept as walkable ground when none of its samples is over a
    trench: ground never overhangs a cut, so a car on a ramp descending below street level cannot
    snap up onto (or drop off) street-level ground cells.
    """
    tile = TILE["ground_gravel"]
    x0, z0, x1, z1 = bbox_cells
    counts = {"cells": 0, "roof": 0}
    MIN_SIZE = CELL / 16.0

    def quad(ix, iz, size, y, layers):
        p0 = (ix, y, iz)
        p1 = (ix, y, iz + size)
        p2 = (ix + size, y, iz + size)
        p3 = (ix + size, y, iz)
        out.add_quad(layers, "ground_gravel", p0, p1, p2, p3,
                     (ix / tile, iz / tile), (ix / tile, (iz + size) / tile),
                     ((ix + size) / tile, (iz + size) / tile), ((ix + size) / tile, iz / tile), UP)

    def visit(ix, iz, size):
        samples = [ctx.cell_covered(ix + size * fx, iz + size * fz) for fx in (0.0, 0.5, 1.0) for fz in (0.0, 0.5, 1.0)]
        if all(c is None for c in samples):
            quad(ix, iz, size, gy, LAYERS_BOTH)
            counts["cells"] += 1
            return
        if size > MIN_SIZE + 1e-6 and len(set(samples)) > 1:
            h = size * 0.5
            for ox in (0.0, h):
                for oz in (0.0, h):
                    visit(ix + ox, iz + oz, h)
            return
        if "open" in samples:
            return  # over (or at the edge of) an open trench: no ground
        if all(c == "roof" for c in samples) or size <= MIN_SIZE + 1e-6:
            quad(ix, iz, size, GROUND_Y, ("render",))  # roof over a covered tunnel (render only)
            counts["roof"] += 1

    ix = x0
    while ix < x1:
        iz = z0
        while iz < z1:
            visit(ix, iz, CELL)
            iz += CELL
        ix += CELL
    return counts["cells"], counts["roof"]


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
    # Expansion joint: dark worn steel finger plate (reads as a thin dark line, not a stop bar).
    mats["joint"] = make_material("joint", None, 0.45, 0.85, color=(0.16, 0.16, 0.17, 1.0))
    mats["lamp"] = make_material("lamp", None, 0.4, 0.0, color=(0.1, 0.1, 0.1, 1.0),
                                 emit=(1.0, 0.6, 0.25, 1.0), emit_strength=12.0)
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
        emit_deck(out, r, ctx)
        emit_markings(out, r, ctx)
        emit_barriers(out, r, ctx)
        emit_tunnel(out, r, ctx)
        emit_slabs(out, r)
        emit_fill(out, r)
        emit_piers(out, r, ctx)
        emit_joints(out, r)
        emit_portals(out, r)
        emit_poles(out, r, ctx, lamps, bbox)
        emit_tunnel_fixtures(out, r, lamps)
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
