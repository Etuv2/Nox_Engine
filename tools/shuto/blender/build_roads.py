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
  * Deck ownership: where same-level decks overlap (merges, diverges, ramp lanes alongside, gores) the
    higher marking_priority deck owns the area; the other deck's render surface, markings and joints
    are clipped exactly to the owner's edge with a watertight seam (DeckOwnership). Collision keeps
    every deck whole.
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
# Per-kind deck offsets. Formerly 1-2 cm lifts for ramps (a z-fight band-aid); overlapping same-level
# decks are now resolved by DeckOwnership, so every deck sits on its solved centreline height.
EPS_BY_KIND = {"main": 0.0, "jct": 0.0, "pa": 0.0, "toll": 0.0, "ramp_on": 0.0, "ramp_off": 0.0}
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


TANGENT_WINDOW = 4


def smoothed_frames(X, Z, window=TANGENT_WINDOW):
    """Per-sample unit tangent and left normal from a polyline sampled at ~1 m (window-averaged)."""
    n = len(X)
    rx, rz = [], []
    for i in range(n):
        a = max(0, i - 1)
        b = min(n - 1, i + 1)
        dx, dz = X[b] - X[a], Z[b] - Z[a]
        ln = math.hypot(dx, dz) or 1.0
        rx.append(dx / ln)
        rz.append(dz / ln)
    TX, TZ, LX, LZ = [], [], [], []
    for i in range(n):
        a, b = max(0, i - window), min(n, i + window + 1)
        dx, dz = sum(rx[a:b]), sum(rz[a:b])
        ln = math.hypot(dx, dz)
        if ln < 1e-9:
            dx, dz, ln = rx[i], rz[i], 1.0
        dx /= ln
        dz /= ln
        TX.append(dx)
        TZ.append(dz)
        LX.append(dz)
        LZ.append(-dx)
    return TX, TZ, LX, LZ


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
        self.frm = cw.get("from")
        self.to = cw.get("to")
        self.lot = None                 # parking apron (apply_lots)

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
        self.LOT = [0.0] * n            # apron width per sample (0 outside the lot)
        # Effective deck half widths per sample (clipped to a shared median where an
        # opposite-direction carriageway runs alongside; see Context.compute_edges).
        self.HL = [self.half_l] * n
        self.HR = [self.half_r] * n
        self.BANK = bank
        self.SB = [math.sin(b) for b in bank]
        # Tangent (xz) and left normal (dz, -dx): left of travel with +Y up. Tangents are averaged over
        # +-TANGENT_WINDOW samples, so a kink in the source polyline turns the cross-lines gradually and
        # the inner deck edge never folds back over itself.
        self.TX, self.TZ, self.LX, self.LZ = smoothed_frames(self.X, self.Z)
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


def mitre_nodes(roads):
    """Give every carriageway end at a shared node the same cross-line direction (the bisector of the
    main in/out tangents), so decks meeting at a node join along one line without wedge cracks."""
    ends = defaultdict(list)   # node -> [(road, sample index, is_end)]
    for r in roads:
        if r.frm:
            ends[r.frm].append((r, 0, False))
        if r.to:
            ends[r.to].append((r, r.N - 1, True))
    n = 0
    for node, lst in ends.items():
        if len(lst) < 2:
            continue
        ins = [e for e in lst if e[2]]
        outs = [e for e in lst if not e[2]]
        ref = []
        for group in (ins, outs):
            if group:
                r, i, _ = max(group, key=lambda e: marking_priority(e[0]))
                ref.append((r.TX[i], r.TZ[i]))
        tx = sum(v[0] for v in ref)
        tz = sum(v[1] for v in ref)
        ln = math.hypot(tx, tz)
        if ln < 1e-6:
            continue
        tx, tz = tx / ln, tz / ln
        p0 = (lst[0][0].X[lst[0][1]], lst[0][0].Z[lst[0][1]])
        for r, i, _ in lst:
            if math.hypot(r.X[i] - p0[0], r.Z[i] - p0[1]) > 0.5:
                continue
            if r.TX[i] * tx + r.TZ[i] * tz < 0.8:
                continue   # a carriageway leaving at a sharp angle keeps its own end line
            r.TX[i], r.TZ[i], r.LX[i], r.LZ[i] = tx, tz, tz, -tx
            n += 1
    log("node mitres: %d carriageway ends aligned" % n)


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
            # (+0.1 m: on curves a point can project just past the half spacing of both neighbours)
            if lon < -(prev_gap * 0.5 + 0.1 + margin) or lon > next_gap * 0.5 + 0.1 + margin:
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

    def cover_near(self, x0, z0, size):
        """Any trench/tunnel sample, or at-grade deck sample near street level, whose deck band can
        reach into the square cell [x0, x0 + size] x [z0, z0 + size]."""
        cx, cz = x0 + 0.5 * size, z0 + 0.5 * size
        for grid, at_grade in ((self.cgrid, False), (self.grid, True)):
            for ri, si in grid.near(cx, cz, 0.5 * size + 8.0):
                r = self.roads[ri]
                if at_grade and (r.ELEV[si] or r.COV[si] or abs(r.Y[si] - GROUND_Y) > 0.6):
                    continue
                m = max(r.HL[si], r.HR[si]) + 0.3
                if x0 - m <= r.X[si] <= x0 + size + m and z0 - m <= r.Z[si] <= z0 + size + m:
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
        # Covered (trench / tunnel) roads: the nearest sample of every such road nearby (not only the
        # nearest covered sample overall: a ramp climbing out of a cut beside a tunnel must still open
        # the ground above it).
        per_road = {}
        for ri, si in self.cgrid.near(x, z, 12.0):
            r = self.roads[ri]
            d2 = (r.X[si] - x) ** 2 + (r.Z[si] - z) ** 2
            if ri not in per_road or d2 < per_road[ri][0]:
                per_road[ri] = (d2, si)
        result = None
        for ri, (_, si) in per_road.items():
            r = self.roads[ri]
            lat, along = r.lateral(si, x, z)
            if lat > r.HL[si] + 0.3 or lat < -r.HR[si] - 0.3:
                continue
            rng = r.cov_range[si]
            if rng is None:
                continue
            sc = r.S[si] + along
            if not (rng[0] - 1e-6 <= sc <= rng[1] + 1e-6):
                continue
            roof = r.cov_roof[si]
            if roof is not None and roof[0] - 1e-6 <= sc <= roof[1] + 1e-6:
                result = "roof"
            else:
                return "open"
        return result


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
KIND_RANK = {"main": 3, "jct": 2, "ramp_on": 1, "ramp_off": 1, "pa": 0, "toll": 0}
LEVEL_DY = 0.35    # m: decks closer than this in height where they overlap in plan are the same level
CONFORM_DY = 0.6   # m: overlapping decks within this are conformed to their owner (conform_profiles)


def marking_priority(road):
    """Which carriageway owns the surface where two same-level decks overlap (merges, diverges, a ramp
    lane running alongside, paved gores): main lines over junction connectors over ramps, then more
    lanes, then a stable id order. The owner's deck and markings run through; the other deck is clipped
    to the owner's edge (render), see DeckOwnership."""
    return (KIND_RANK.get(road.kind, 1), road.lanes, road.id)


# ----------------------------------------------------------------------------------------------
# Deck ownership: render clipping of lower-priority same-level decks
# ----------------------------------------------------------------------------------------------
def _lerp3(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def _cross2(ax, az, bx, bz):
    return ax * bz - az * bx


def _line_hit(a, b, p, q):
    """Plan intersection of line a->b (param w) with line p->q (param s); (w, s) or None if parallel."""
    dx, dz = b[0] - a[0], b[2] - a[2]
    ex, ez = q[0] - p[0], q[2] - p[2]
    den = _cross2(dx, dz, ex, ez)
    if abs(den) < 1e-12:
        return None
    fx, fz = p[0] - a[0], p[2] - a[2]
    return _cross2(fx, fz, ex, ez) / den, _cross2(fx, fz, dx, dz) / den


class Strip:
    """One segment of a carriageway ribbon (or a sub-strip such as a marking): right edge A0->A1
    (w = 0), left edge B0->B1 (w = 1); the t-line at t runs from A(t) to B(t)."""

    def __init__(self, a0, a1, b0, b1):
        self.a0, self.a1, self.b0, self.b1 = a0, a1, b0, b1

    def A(self, t):
        return _lerp3(self.a0, self.a1, t)

    def B(self, t):
        return _lerp3(self.b0, self.b1, t)

    def at(self, w, t):
        return _lerp3(self.A(t), self.B(t), w)

    def inverse(self, p):
        """(w, t) of plan point p (Newton on the bilinear map)."""
        w, t = 0.5, 0.5
        for _ in range(12):
            q = self.at(w, t)
            fx, fz = q[0] - p[0], q[2] - p[2]
            a, b = self.A(t), self.B(t)
            dwx, dwz = b[0] - a[0], b[2] - a[2]
            da = (self.a1[0] - self.a0[0], self.a1[2] - self.a0[2])
            db = (self.b1[0] - self.b0[0], self.b1[2] - self.b0[2])
            dtx = da[0] + (db[0] - da[0]) * w
            dtz = da[1] + (db[1] - da[1]) * w
            det = dwx * dtz - dwz * dtx
            if abs(det) < 1e-12:
                break
            dw = (fx * dtz - fz * dtx) / det
            dt = (dwx * fz - dwz * fx) / det
            w -= dw
            t -= dt
            if abs(dw) < 1e-12 and abs(dt) < 1e-12:
                break
        return w, t


def _poly_contains(poly, x, z):
    """Point inside a convex polygon (plan), any orientation."""
    sgn = 0
    n = len(poly)
    for k in range(n):
        a, b = poly[k], poly[(k + 1) % n]
        c = _cross2(b[0] - a[0], b[2] - a[2], x - a[0], z - a[2])
        if abs(c) < 1e-12:
            continue
        s = 1 if c > 0 else -1
        if sgn == 0:
            sgn = s
        elif s != sgn:
            return False
    return True


class DeckOwnership:
    """Where two carriageways' decks overlap at the same level (|dy| < LEVEL_DY), the higher
    marking_priority deck owns the area. The lower deck's render surface (and its markings and joints)
    is clipped exactly to the union of the owners' segment quads. The seam is watertight: the clipped
    edge follows the owner's edge vertices, and every point where a clipped deck's sample line meets
    an owner edge is inserted into the owner's edge as well (no T-junctions). Collision is unchanged:
    every deck keeps its full collision surface, so the drivable surface stays continuous through
    merges and diverges (the ground ray takes the upper surface; see check_collision render-vs-collision)."""

    def __init__(self, roads):
        self.roads = roads
        self.prio = {r.id: marking_priority(r) for r in roads}
        self.cell = 8.0
        self.grid = defaultdict(list)
        self.quads = {}
        for ri, r in enumerate(roads):
            for j in range(r.N - 1):
                q = self.quad(r, j)
                xs = [p[0] for p in q]
                zs = [p[2] for p in q]
                for cx in range(math.floor(min(xs) / self.cell), math.floor(max(xs) / self.cell) + 1):
                    for cz in range(math.floor(min(zs) / self.cell), math.floor(max(zs) / self.cell) + 1):
                        self.grid[(cx, cz)].append((ri, j))
        self.level_cache = {}
        self.inserts = defaultdict(set)   # (road id, segment, "L"/"R") -> {param along that edge}
        self.stats = defaultdict(int)

    def quad(self, r, j):
        key = (r.id, j)
        q = self.quads.get(key)
        if q is None:
            # (l_j, r_j, r_j+1, l_j+1): the deck quad as emitted
            q = (r.top(j, r.HL[j]), r.top(j, -r.HR[j]), r.top(j + 1, -r.HR[j + 1]), r.top(j + 1, r.HL[j + 1]))
            self.quads[key] = q
        return q

    def level(self, road, o, j, thr=None):
        """Owner quad (o, j) is at road's level where the two decks overlap: the smallest height
        difference at points of the owner quad lying on road's deck (or, failing that, at road's
        cross-section points lying in the quad). Depends on (road, o, j) only, so adjacent segments of
        road agree on the owner set along their shared sample line."""
        key = (road.id, o.id, j)
        v = self.level_cache.get(key)
        if v is not None:
            return v < (LEVEL_DY if thr is None else thr)
        q = self.quad(o, j)
        pts = list(q) + [_lerp3(q[k], q[(k + 1) % 4], 0.5) for k in range(4)] + [_lerp3(q[0], q[2], 0.5)]
        best = None
        for p in pts:
            si = self._nearest_sample(road, p[0], p[2])
            if si is None:
                continue
            lat, lon = road.lateral(si, p[0], p[2])
            if -road.HR[si] - 0.05 <= lat <= road.HL[si] + 0.05 and abs(lon) <= 0.75:
                d = abs(road.top_y(si, lat) - p[1])
                best = d if best is None else min(best, d)
        if best is None:
            cx = sum(p[0] for p in q) * 0.25
            cz = sum(p[2] for p in q) * 0.25
            si0 = self._nearest_sample(road, cx, cz)
            if si0 is not None:
                for si in range(max(0, si0 - 3), min(road.N, si0 + 4)):
                    for u in (-road.HR[si], -0.5 * road.HR[si], 0.0, 0.5 * road.HL[si], road.HL[si]):
                        p = road.top(si, u)
                        if _poly_contains(q, p[0], p[2]):
                            d = abs(owner_height_at(self, o, j, p) - p[1])
                            best = d if best is None else min(best, d)
        v = 1e9 if best is None else best
        self.level_cache[key] = v
        return v < (LEVEL_DY if thr is None else thr)

    def forget(self, road):
        """Drop cached quads and level tests involving road (its heights changed)."""
        for j in range(road.N - 1):
            self.quads.pop((road.id, j), None)
        for k in [k for k in self.level_cache if k[0] == road.id or k[1] == road.id]:
            del self.level_cache[k]

    def _nearest_sample(self, road, x, z):
        best = None
        cx, cz = math.floor(x / self.cell), math.floor(z / self.cell)
        for ix in (cx - 1, cx, cx + 1):
            for iz in (cz - 1, cz, cz + 1):
                for ri, j in self.grid.get((ix, iz), ()):
                    r = self.roads[ri]
                    if r is not road:
                        continue
                    for k in (j, j + 1):
                        d2 = (r.X[k] - x) ** 2 + (r.Z[k] - z) ** 2
                        if best is None or d2 < best[0]:
                            best = (d2, k)
        return None if best is None else best[1]

    def owners(self, road, pts, thr=None):
        """Owner quads (o, j) whose plan bbox overlaps the bbox of pts and that are higher priority and
        at the same level (|dy| < thr, default LEVEL_DY)."""
        mine = self.prio[road.id]
        xs = [p[0] for p in pts]
        zs = [p[2] for p in pts]
        x0, x1, z0, z1 = min(xs) - 0.01, max(xs) + 0.01, min(zs) - 0.01, max(zs) + 0.01
        seen = set()
        out = []
        for cx in range(math.floor(x0 / self.cell), math.floor(x1 / self.cell) + 1):
            for cz in range(math.floor(z0 / self.cell), math.floor(z1 / self.cell) + 1):
                for ri, j in self.grid.get((cx, cz), ()):
                    if (ri, j) in seen:
                        continue
                    seen.add((ri, j))
                    o = self.roads[ri]
                    if o is road or self.prio[o.id] <= mine:
                        continue
                    q = self.quad(o, j)
                    if (max(p[0] for p in q) < x0 or min(p[0] for p in q) > x1 or
                            max(p[2] for p in q) < z0 or min(p[2] for p in q) > z1):
                        continue
                    if self.level(road, o, j, thr):
                        out.append((o, j))
        return out

    def owned_point(self, road, x, z, pts=None):
        for o, j in self.owners(road, pts or [(x, 0.0, z)]):
            if _poly_contains(self.quad(o, j), x, z):
                return True
        return False

    def boundary_edges(self, road, owners):
        """Owner quad edges that bound the owned area: (P, Q, owner, j, kind) with kind L/R (deck edges)
        or C (a cap: owner start/end, or the owner leaves road's level there)."""
        edges = []
        for o, j in owners:
            l0, r0, r1, l1 = self.quad(o, j)
            edges.append((l0, l1, o, j, "L"))
            edges.append((r0, r1, o, j, "R"))
            if j == 0 or not self.level(road, o, j - 1):
                edges.append((r0, l0, o, j, "C"))
            if j + 1 >= o.N - 1 or not self.level(road, o, j + 1):
                edges.append((r1, l1, o, j, "C"))
        return edges

    def clip(self, road, strip, owners, own_edges=None, record=False, owner_height=True):
        """Visible parts of strip outside the owners' quads: a list of trapezoids, each 4 corners
        [(point, w, t)] ordered lo(ta), hi(ta), hi(tb), lo(tb) (lo = toward the right edge, w = 0).
        own_edges: {"L": params, "R": params} extra breakpoints on the strip's own edges (inserted
        seam points of lower decks). record: insert seam points into the owners' edges.
        Corners are (point, w, t, owner_point): point at the strip's own height (owner_height=False) or
        the owner's (True); owner_point is the same plan point at the owner's height on a seam, else None."""
        polys = [self.quad(o, j) for o, j in owners]
        edges = self.boundary_edges(road, owners)
        ts = {0.0, 1.0}
        for P, Q, o, j, kind in edges:
            for p in (P, Q):
                w, t = strip.inverse(p)
                if 1e-9 < t < 1.0 - 1e-9 and -1e-6 <= w <= 1.0 + 1e-6:
                    ts.add(t)
            for e0, e1 in ((strip.a0, strip.a1), (strip.b0, strip.b1)):
                h = _line_hit(e0, e1, P, Q)
                if h is not None and 1e-9 < h[0] < 1.0 - 1e-9 and -1e-9 <= h[1] <= 1.0 + 1e-9:
                    ts.add(h[0])
        for a in range(len(edges)):
            for b in range(a + 1, len(edges)):
                Pa, Qa, oa, ja, _ = edges[a]
                Pb, Qb, ob, jb, _ = edges[b]
                if oa is ob and abs(ja - jb) <= 1:
                    continue
                h = _line_hit(Pa, Qa, Pb, Qb)
                if h is None or not (0.0 <= h[0] <= 1.0 and 0.0 <= h[1] <= 1.0):
                    continue
                w, t = strip.inverse(_lerp3(Pa, Qa, h[0]))
                if 1e-9 < t < 1.0 - 1e-9 and 0.0 <= w <= 1.0:
                    ts.add(t)
        if own_edges:
            for params in own_edges.values():
                for t in params:
                    if 1e-9 < t < 1.0 - 1e-9:
                        ts.add(t)
        ts = sorted(ts)
        merged = [ts[0]]
        for t in ts[1:]:
            if t - merged[-1] > 1e-7:
                merged.append(t)
        merged[-1] = 1.0
        out = []
        for ta, tb in zip(merged[:-1], merged[1:]):
            tm = 0.5 * (ta + tb)
            a, b = strip.A(tm), strip.B(tm)
            spans = []
            for (o, j), poly in zip(owners, polys):
                cb = self._clip_line(a, b, poly)
                if cb is None:
                    continue
                w0, k0, w1, k1 = cb
                if w1 <= 1e-9 or w0 >= 1.0 - 1e-9:
                    continue
                e0 = None if w0 <= 0.0 else self._edge_of(road, o, j, poly, k0)
                e1 = None if w1 >= 1.0 else self._edge_of(road, o, j, poly, k1)
                spans.append((max(w0, 0.0), e0, min(w1, 1.0), e1))
            spans.sort(key=lambda s: s[0])
            vis = []
            cur_w, cur_e = 0.0, "A"
            k = 0
            while k < len(spans):
                w0, e0, w1, e1 = spans[k]
                k += 1
                # merge overlapping owned spans
                while k < len(spans) and spans[k][0] <= w1 + 1e-9:
                    if spans[k][2] > w1:
                        w1, e1 = spans[k][2], spans[k][3]
                    k += 1
                if w0 > cur_w + 1e-7:
                    vis.append((cur_e, e0 if e0 is not None else "B"))
                if w1 > cur_w:
                    cur_w, cur_e = w1, (e1 if e1 is not None else "A")
            if cur_w < 1.0 - 1e-7:
                vis.append((cur_e, "B"))
            for lo, hi in vis:
                c = [self._eval(strip, lo, ta, record, owner_height), self._eval(strip, hi, ta, record, owner_height),
                     self._eval(strip, hi, tb, record, owner_height), self._eval(strip, lo, tb, record, owner_height)]
                if None in c:
                    self.stats["degenerate"] += 1
                    continue
                out.append(c)
        return out

    @staticmethod
    def _clip_line(a, b, poly):
        """Cyrus-Beck: the line a->b (param w, unbounded) against a convex polygon; (w_in, edge_in,
        w_out, edge_out) or None."""
        n = len(poly)
        area = 0.0
        for k in range(n):
            p, q = poly[k], poly[(k + 1) % n]
            area += _cross2(p[0], p[2], q[0], q[2])
        orient = 1.0 if area > 0 else -1.0
        w_in, w_out = -1e18, 1e18
        k_in = k_out = None
        dx, dz = b[0] - a[0], b[2] - a[2]
        for k in range(n):
            p, q = poly[k], poly[(k + 1) % n]
            ex, ez = q[0] - p[0], q[2] - p[2]
            # inside: orient * cross(e, x - p) >= 0
            num = orient * _cross2(ex, ez, a[0] - p[0], a[2] - p[2])
            den = orient * _cross2(ex, ez, dx, dz)
            if abs(den) < 1e-14:
                if num < 0.0:
                    return None
                continue
            w = -num / den
            if den > 0.0:
                if w > w_in:
                    w_in, k_in = w, k
            else:
                if w < w_out:
                    w_out, k_out = w, k
        if w_in >= w_out - 1e-12:
            return None
        return w_in, k_in, w_out, k_out

    def _edge_of(self, road, o, j, poly, k):
        """Polygon edge k of owner quad (l_j, r_j, r_j+1, l_j+1) as (P, Q, owner, j, kind)."""
        l0, r0, r1, l1 = poly
        if k == 0:
            return (r0, l0, o, j, "C")   # l_j -> r_j (start cap); stored P = r_j, Q = l_j
        if k == 1:
            return (r0, r1, o, j, "R")
        if k == 2:
            return (r1, l1, o, j, "C")
        return (l0, l1, o, j, "L")

    def _eval(self, strip, curve, t, record, owner_height):
        if curve == "A":
            return (strip.A(t), 0.0, t, None)
        if curve == "B":
            return (strip.B(t), 1.0, t, None)
        P, Q, o, j, kind = curve
        a, b = strip.A(t), strip.B(t)
        h = _line_hit(a, b, P, Q)
        if h is None:
            return None
        w, s = h
        if w < -1e-4 or w > 1.0 + 1e-4:
            # near-parallel cap: numerically unstable; fall back to the strip edge point
            wc = min(1.0, max(0.0, w))
            return (strip.at(wc, t), wc, t, None)
        if s < 1e-7:
            s = 0.0
        elif s > 1.0 - 1e-7:
            s = 1.0
        p = P if s == 0.0 else Q if s == 1.0 else _lerp3(P, Q, s)
        if record and kind in ("L", "R") and 0.0 < s < 1.0:
            self.inserts[(o.id, j, kind)].add(s)
        q = strip.at(min(1.0, max(0.0, w)), t)
        if owner_height:
            return (p, w, t, p)
        return ((p[0], q[1], p[2]), w, t, p)


def owner_height_at(own, o, j, p):
    """Height of owner quad (o, j) at plan point p (bilinear on the quad; p may be on its boundary)."""
    l0, r0, r1, l1 = own.quad(o, j)
    st = Strip(r0, r1, l0, l1)
    w, t = st.inverse(p)
    return st.at(min(1.0, max(0.0, w)), min(1.0, max(0.0, t)))[1]


def _ease_offsets(road, d, max_change):
    """Offsets d (None = free) eased back to 0 outside the given samples: smoothstep falloff over
    clamp(|d| / max_change, 30, 150) m, light smoothing, free ends pinned to 0 within 10 m."""
    over = [i for i in range(road.N) if d[i] is not None]
    f = [0.0] * road.N
    for i in over:
        f[i] = d[i]
    prev = None
    k = 0
    for i in range(road.N):
        if d[i] is not None:
            prev = i
            continue
        while k < len(over) and over[k] <= i:
            k += 1
        nxt = over[k] if k < len(over) else None
        acc, wsum = 0.0, 0.0
        for src in (prev, nxt):
            if src is None:
                continue
            L = clamp(abs(d[src]) / max_change, 30.0, 150.0)
            x = abs(road.S[i] - road.S[src]) / L
            if x >= 1.0:
                continue
            wgt = 1.0 - x * x * (3.0 - 2.0 * x)   # smoothstep falloff
            acc += wgt * d[src]
            wsum += wgt
        f[i] = acc / max(1.0, wsum)
    g = f[:]
    for i in range(road.N):
        a, b = max(0, i - 3), min(road.N, i + 4)
        g[i] = sum(f[a:b]) / (b - a)
    for i in range(road.N):
        e = min(road.S[i], road.S[-1] - road.S[i])
        if d[i] is None and e < 10.0:
            g[i] *= e / 10.0
    return g


def conform_profiles(roads, own, max_grade_change=0.008, max_bank_change=0.004):
    """Data-level repair of same-level overlaps: where a lower-priority deck overlaps an owner deck,
    its cross slope is set to the owner's (where they run parallel) and its centreline height so that
    its surface meets the owner's at the seam (or under its centreline when its lanes lie on the
    owner): inside the overlap the two decks are coplanar, so the visible owner surface is also the
    surface the car rides on. Both are eased back to the solved profile outside the overlap (at most
    max_grade_change of extra grade, max_bank_change of cross slope per metre). Roads are processed from
    the highest priority down, so a deck conforms to already conformed owners.
    Returns {road id: [(sample, dy, d_sin_bank), ...]} for the checker."""
    offsets = {}
    for road in sorted(roads, key=marking_priority, reverse=True):
        d = [None] * road.N
        db = [None] * road.N
        for i in range(road.N):
            l = road.top(i, road.HL[i])
            r = road.top(i, -road.HR[i])
            owners = own.owners(road, (l, r), CONFORM_DY)
            if not owners:
                continue
            spans = []
            for o, j in owners:
                cb = DeckOwnership._clip_line(r, l, own.quad(o, j))
                if cb is None or cb[2] <= 0.0 or cb[0] >= 1.0:
                    continue
                spans.append((max(cb[0], 0.0), min(cb[2], 1.0), o, j))
            if not spans:
                continue
            wc = road.HR[i] / (road.HL[i] + road.HR[i])      # centreline
            best = None
            for w0, w1, o, j in spans:
                if w0 <= wc <= w1:
                    ws = wc
                else:
                    ws = w0 if abs(w0 - wc) < abs(w1 - wc) else w1
                if best is None or abs(ws - wc) < abs(best[0] - wc):
                    best = (ws, o, j)
            ws, o, j = best
            p = _lerp3(r, l, ws)
            u = -road.HR[i] + ws * (road.HL[i] + road.HR[i])
            # owner's cross slope along this deck's lateral direction
            ol0, or0, _, _ = own.quad(o, j)
            ex, ez = ol0[0] - or0[0], ol0[2] - or0[2]
            ln = math.hypot(ex, ez)
            sb = road.SB[i]
            if ln > 1e-6:
                dot = (ex * road.LX[i] + ez * road.LZ[i]) / ln
                if abs(dot) > 0.9:
                    sb = (ol0[1] - or0[1]) / ln * dot
                    db[i] = sb - road.SB[i]
            d[i] = owner_height_at(own, o, j, p) - (road.Y[i] + road.eps + u * sb)
        if not any(v is not None for v in d):
            continue
        if max(abs(v) for v in d if v is not None) < 0.002 and                 max((abs(v) for v in db if v is not None), default=0.0) < 0.002:
            continue
        gb = _ease_offsets(road, [None if d[i] is None else (db[i] or 0.0) for i in range(road.N)], max_bank_change)
        g = _ease_offsets(road, d, max_grade_change)
        changed = []
        for i in range(road.N):
            if abs(g[i]) > 1e-4 or abs(gb[i]) > 1e-5:
                road.Y[i] += g[i]
                road.SB[i] = clamp(road.SB[i] + gb[i], -0.2, 0.2)
                road.BANK[i] = math.asin(road.SB[i])
                changed.append((i, round(g[i], 4), round(gb[i], 5)))
        if changed:
            offsets[road.id] = changed
            own.forget(road)
            own.stats["conformed_roads"] += 1
            own.stats["conformed_max_mm"] = max(own.stats["conformed_max_mm"], int(max(abs(v[1]) for v in changed) * 1000))
    return offsets


def emit_deck(out, road, ctx):
    """Deck surface. Render: clipped where a higher-priority same-level deck owns the area (DeckOwnership);
    collision: the full deck."""
    tile = TILE["asphalt"]
    own = ctx.own
    for i in range(road.N - 1):
        l0 = road.top(i, road.HL[i])
        r0 = road.top(i, -road.HR[i])
        l1 = road.top(i + 1, road.HL[i + 1])
        r1 = road.top(i + 1, -road.HR[i + 1])
        s0 = road.S[i] / tile
        s1 = road.S[i + 1] / tile
        uv = [(road.HL[i] / tile, s0), (-road.HR[i] / tile, s0), (-road.HR[i + 1] / tile, s1), (road.HL[i + 1] / tile, s1)]
        out.add_quad(("col",), "asphalt", l0, r0, r1, l1, uv[0], uv[1], uv[2], uv[3], UP)
        owners = own.owners(road, (l0, r0, r1, l1))
        extra = {k: own.inserts.get((road.id, i, k), ()) for k in ("L", "R")}
        if not owners and not extra["L"] and not extra["R"]:
            out.add_quad(("render",), "asphalt", l0, r0, r1, l1, uv[0], uv[1], uv[2], uv[3], UP)
            ctx.deck_tris[(road.id, i)] = [(l0, r0, r1), (l0, r1, l1)]
            continue
        strip = Strip(r0, r1, l0, l1)
        # own edge params: the strip's A edge is the right edge (R), B the left edge (L)
        # The clipped deck keeps its own (planar) surface; where it meets the owner's edge at a slightly
        # different height (after conform_profiles: median < 1 mm) a riser closes the seam, so the joint is
        # watertight and the visible surface is exactly the deck the car drives on.
        traps = own.clip(road, strip, owners, own_edges=extra, record=True, owner_height=False)
        if owners:
            own.stats["clipped_segments"] += 1
        tris = ctx.deck_tris.setdefault((road.id, i), [])
        for c in traps:
            tris.append((c[0][0], c[1][0], c[2][0]))
            tris.append((c[0][0], c[2][0], c[3][0]))
            uvs = []
            for p, w, t, _ in c:
                u = -(road.HR[i] + (road.HR[i + 1] - road.HR[i]) * t) * (1.0 - w) + \
                    (road.HL[i] + (road.HL[i + 1] - road.HL[i]) * t) * w
                s = road.S[i] + (road.S[i + 1] - road.S[i]) * t
                uvs.append((u / tile, s / tile))
            out.add_quad(("render",), "asphalt", c[0][0], c[1][0], c[2][0], c[3][0],
                         uvs[0], uvs[1], uvs[2], uvs[3], UP)
            # seam risers (lo side: corners 0-3, hi side: corners 1-2), facing the lower surface
            for ka, kb, uva, uvb in ((0, 3, uvs[0], uvs[3]), (1, 2, uvs[1], uvs[2])):
                qa, qb = c[ka][3], c[kb][3]
                if qa is None or qb is None:
                    continue
                pa, pb = c[ka][0], c[kb][0]
                if max(abs(pa[1] - qa[1]), abs(pb[1] - qb[1])) < 5e-4:
                    continue
                # plan normal of the seam pointing into this deck's visible part
                ex, ez = pb[0] - pa[0], pb[2] - pa[2]
                other = c[1][0] if ka == 0 else c[0][0]
                nx, nz = -ez, ex
                if nx * (other[0] - pa[0]) + nz * (other[2] - pa[2]) < 0.0:
                    nx, nz = -nx, -nz
                if (pa[1] + pb[1]) > (qa[1] + qb[1]):
                    nx, nz = -nx, -nz      # this deck is higher: the riser faces the owner's side
                out.add_quad(("render",), "asphalt", pa, pb, qb, qa, uva, uvb, uvb, uva, (nx, 0.0, nz))
                own.stats["seam_risers"] += 1


def _clip_convex(poly, clip):
    """Sutherland-Hodgman in plan: convex polygon poly (3D points) clipped by convex polygon clip."""
    area = 0.0
    for k in range(len(clip)):
        p, q = clip[k], clip[(k + 1) % len(clip)]
        area += _cross2(p[0], p[2], q[0], q[2])
    orient = 1.0 if area > 0 else -1.0
    out = list(poly)
    for k in range(len(clip)):
        if not out:
            break
        p, q = clip[k], clip[(k + 1) % len(clip)]
        ex, ez = q[0] - p[0], q[2] - p[2]

        def side(v):
            return orient * _cross2(ex, ez, v[0] - p[0], v[2] - p[2])
        res = []
        for m in range(len(out)):
            cur, nxt = out[m], out[(m + 1) % len(out)]
            sc, sn = side(cur), side(nxt)
            if sc >= 0.0:
                res.append(cur)
            if (sc >= 0.0) != (sn >= 0.0):
                f = sc / (sc - sn)
                res.append(_lerp3(cur, nxt, f))
        out = res
    return out


def _plane_y(tri, x, z):
    a, b, c = tri
    den = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
    if abs(den) < 1e-12:
        return a[1]
    l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / den
    l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / den
    return l1 * a[1] + l2 * b[1] + (1.0 - l1 - l2) * c[1]


def emit_decal_strip(out, road, ctx, mat, strip, lift, seg):
    """A marking / joint strip on road's deck segment seg, laid exactly on the rendered deck: the strip is
    intersected with each rendered deck triangle of the segment (so it ends where the deck is clipped by
    an owner) and every piece takes that triangle's plane, raised by lift."""
    quad = [strip.a0, strip.b0, strip.b1, strip.a1]
    tris = ctx.deck_tris.get((road.id, seg), ())
    if len(tris) == 2:
        # Unclipped segment: if the deck quad is planar within 1.5 mm, one strip quad with its corners on
        # the deck is enough (the piecewise path below would only add triangles).
        t0, t1 = tris
        if abs(_plane_y(t0, t1[2][0], t1[2][2]) - t1[2][1]) < 0.0015:
            pts = []
            for p in quad:
                y = _plane_y(t0, p[0], p[2])
                y1 = _plane_y(t1, p[0], p[2])
                pts.append((p[0], max(y, y1) + lift[1], p[2]))
            out.add_quad(("render",), mat, pts[0], pts[1], pts[2], pts[3],
                         (0, 0), (1, 0), (1, 1), (0, 1), UP)
            return
    for tri in tris:
        piece = _clip_convex(quad, list(tri))
        if len(piece) < 3:
            continue
        pts = [(p[0], _plane_y(tri, p[0], p[2]) + lift[1], p[2]) for p in piece]
        for k in range(1, len(pts) - 1):
            out.add_tri(("render",), mat, (pts[0], pts[k], pts[k + 1]),
                        ((pts[0][0], pts[0][2]), (pts[k][0], pts[k][2]), (pts[k + 1][0], pts[k + 1][2])), UP)


def higher_deck_alongside(ctx, road, x, y, z):
    """True when a higher-priority same-level deck covers (x, z)."""
    mine = marking_priority(road)
    for r, si, lat, ytop in ctx.footprints(x, z, 0.0, exclude=road):
        if abs(ytop - y) < LEVEL_DY and marking_priority(r) > mine:
            return True
    return False


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

    joints = joint_segments(road)

    def strip(i, u0, u1, ua=None, ub=None):
        a0 = road.top(i, u0 if ua is None else ua[0])
        b0 = road.top(i, u1 if ua is None else ua[1])
        a1 = road.top(i + 1, u0 if ub is None else ub[0])
        b1 = road.top(i + 1, u1 if ub is None else ub[1])
        if i in joints:
            # the expansion joint occupies t in [0, 0.25] of this segment: paint resumes after it
            a0, b0 = _lerp3(a0, a1, 0.3), _lerp3(b0, b1, 0.3)
        emit_decal_strip(out, road, ctx, "paint", Strip(a0, a1, b0, b1), lift, i)

    def owned_by_other(i, u):
        # cheap pre-test (the strip itself is clipped exactly by emit_decal_strip)
        ca = road.top(i, u)
        cb = road.top(i + 1, u)
        return (ctx.own.owned_point(road, ca[0], ca[2], (ca, cb)) and
                ctx.own.owned_point(road, cb[0], cb[2], (ca, cb)))

    # Lane dividers.
    for u in dividers:
        for i in range(road.N - 1):
            if road.S[i] % 20.0 >= 8.0 or owned_by_other(i, u):
                continue
            strip(i, u - 0.075, u + 0.075)
    # Edge lines (solid, or thick dashed along a joining/leaving lane).
    lot_sgn = None if road.lot is None else (1.0 if road.lot["side"] == "L" else -1.0)
    for sgn in (1.0, -1.0):
        u = sgn * (half_edge - 0.25)
        for i in range(road.N - 1):
            if owned_by_other(i, u):
                continue
            # Probe just outside the edge line: a lower-priority deck there means a lane is joining;
            # a higher-priority deck there (an added lane running alongside its main line) carries the
            # boundary line itself, so this deck's edge line is left out (no double line).
            probe = road.top(i, sgn * (half_edge + 1.0))
            if higher_deck_alongside(ctx, road, probe[0], probe[1], probe[2]):
                continue
            if sgn == lot_sgn and (road.LOT[i] > 0.0 or road.LOT[i + 1] > 0.0):
                # along the parking apron: dashed lane boundary (3 m / 3 m)
                if road.S[i] % 6.0 < 3.0:
                    strip(i, u - 0.075, u + 0.075)
                continue
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
            if road.LOT[i] > 0.0 or road.LOT[i + 1] > 0.0:
                continue
            if road.S[i] % 4.0 >= 1.0:
                continue
            inner = half_edge - 0.25 + 0.3
            outer_i, outer_n = H[i] - 0.15, H[i + 1] - 0.15
            # Diagonal stripe: from the edge line at sample i to the deck edge at sample i+1.
            strip(i, 0.0, 0.0, ua=(sgn * inner, sgn * (inner + 0.45)), ub=(sgn * (outer_n - 0.45), sgn * outer_n))


# Parking apron layout (lateral offsets beyond the lane's nominal edge, metres)
LOT_BAY = (0.5, 6.0)        # 90-degree bays, 2.5 m wide
LOT_WALK = (6.5, 8.5)       # raised walkway (0.12 m curb)
LOT_BUILDING = 9.0          # rest building from here to the apron edge - 0.6 m


def apply_lots(net, roads):
    """Widen a PA lane's deck into its parking apron (facility "lot", written by repair_network.py)."""
    by_id = {r.id: r for r in roads}
    n = 0
    for f in net.get("facilities", []):
        lot = f.get("lot")
        if not lot or lot["carriageway"] not in by_id:
            continue
        r = by_id[lot["carriageway"]]
        r.lot = dict(lot, facility=f["id"], name=f.get("name_en") or f["id"])
        W, s0, s1, tp = float(lot["width"]), float(lot["s0"]), float(lot["s1"]), float(lot.get("taper", 10.0))
        H, half = (r.HL, r.half_l) if lot["side"] == "L" else (r.HR, r.half_r)
        for i, s in enumerate(r.S):
            if s0 <= s <= s1:
                w = 1.0
            elif s0 - tp < s < s0:
                w = (s - (s0 - tp)) / tp
            elif s1 < s < s1 + tp:
                w = ((s1 + tp) - s) / tp
            else:
                continue
            r.LOT[i] = W * w
            H[i] = max(H[i], half + W * w)
        n += 1
    log("parking aprons: %d" % n)


def lot_point(road, s, u):
    i = clamp(int(bisect.bisect_right(road.S, s) - 1), 0, road.N - 2)
    t = (s - road.S[i]) / max(road.S[i + 1] - road.S[i], 1e-9)
    return _lerp3(road.top(i, u), road.top(i + 1, u), t), i, t


def emit_lot_markings(out, road, ctx):
    """Bay lines (stall separators every 2.5 m and a back line) on the parking apron."""
    lot = road.lot
    sg = 1.0 if lot["side"] == "L" else -1.0
    half = road.half_l if sg > 0 else road.half_r
    W = float(lot["width"])
    lift = (0.0, 0.01, 0.0)
    ua, ub = half + LOT_BAY[0], half + LOT_BAY[1]
    s = float(lot["s0"]) + 1.0
    while s <= float(lot["s1"]) - 1.0 + 1e-6:
        a0, i, t = lot_point(road, s, sg * ua)
        # keep each stall line inside one deck segment (decals are laid per segment)
        t = clamp(t, 0.08, 0.92)
        sc = road.S[i] + t * (road.S[i + 1] - road.S[i])
        a0, _, _ = lot_point(road, sc - 0.06, sg * ua)
        a1, _, _ = lot_point(road, sc + 0.06, sg * ua)
        b0, _, _ = lot_point(road, sc - 0.06, sg * ub)
        b1, _, _ = lot_point(road, sc + 0.06, sg * ub)
        emit_decal_strip(out, road, ctx, "paint", Strip(a0, a1, b0, b1), lift, i)
        s += 2.5
    for i in range(road.N - 1):
        if road.LOT[i] >= W - 1e-6 and road.LOT[i + 1] >= W - 1e-6:
            u0, u1 = sg * (ub - 0.06), sg * (ub + 0.06)
            emit_decal_strip(out, road, ctx, "paint", Strip(road.top(i, u0), road.top(i + 1, u0),
                                                             road.top(i, u1), road.top(i + 1, u1)), lift, i)


def emit_lot(out, road, ctx, lamps):
    """Parking apron furniture: raised walkway, rest building (shop / toilets) with a lit glass front,
    a row of vending machines and canopy lights. Render + collision (walls), lamps for the scene."""
    lot = road.lot
    sg = 1.0 if lot["side"] == "L" else -1.0
    half = road.half_l if sg > 0 else road.half_r
    W = float(lot["width"])
    s0, s1 = float(lot["s0"]), float(lot["s1"])
    # walkway: top (ground class, drivable) and its curb face (render only, 0.12 m)
    for i in range(road.N - 1):
        if not (road.LOT[i] >= W - 1e-6 and road.LOT[i + 1] >= W - 1e-6):
            continue
        w0, w1 = sg * (half + LOT_WALK[0]), sg * (half + LOT_WALK[1])
        p = [road.top(i, w0), road.top(i, w1), road.top(i + 1, w1), road.top(i + 1, w0)]
        up = [(q[0], q[1] + 0.12, q[2]) for q in p]
        out.add_quad(LAYERS_BOTH, "concrete_cast", up[0], up[1], up[2], up[3],
                     (0, road.S[i] / 2.0), (1, road.S[i] / 2.0), (1, road.S[i + 1] / 2.0), (0, road.S[i + 1] / 2.0), UP)
        out.add_quad(("render",), "concrete_cast", p[0], p[3], up[3], up[0], (0, 0), (1, 0), (1, 0.05), (0, 0.05),
                     (-sg * road.LX[i], 0.0, -sg * road.LZ[i]))
    # building
    sm = 0.5 * (s0 + s1)
    length = min(26.0, s1 - s0 - 8.0)
    ub0, ub1 = half + LOT_BUILDING, half + W - 0.6
    c_in, i, _ = lot_point(road, sm, sg * 0.5 * (ub0 + ub1))
    corners = [lot_point(road, sm + k * 0.5 * length, sg * u)[0][1] for k in (-1, 1) for u in (ub0, ub1)]
    base = min(corners) - 0.05
    top = max(corners) + 3.6
    axes = [(road.TX[i], 0.0, road.TZ[i]), (0.0, 1.0, 0.0), (road.LX[i], 0.0, road.LZ[i])]
    depth = ub1 - ub0
    out.add_box(LAYERS_BOTH, "concrete_cast", (c_in[0], 0.5 * (base + top), c_in[2]), axes,
                (0.5 * length, 0.5 * (top - base), 0.5 * depth))
    # roof slab with a canopy over the walkway side
    cr = (c_in[0] - sg * road.LX[i] * 0.6, top + 0.15, c_in[2] - sg * road.LZ[i] * 0.6)
    out.add_box(("render",), "concrete_weathered", cr, axes, (0.5 * length + 0.4, 0.15, 0.5 * depth + 0.6))
    # lit glass front facing the apron (0.4 m to 2.8 m above the floor), just proud of the wall
    face_u = ub0 - 0.02
    nrm = (-sg * road.LX[i], 0.0, -sg * road.LZ[i])
    for k in range(4):
        sa = sm - 0.5 * length + 1.0 + k * (length - 2.0) / 4.0
        sb = sa + (length - 2.0) / 4.0 - 0.6
        pa = lot_point(road, sa, sg * face_u)[0]
        pb = lot_point(road, sb, sg * face_u)[0]
        out.add_quad(("render",), "pa_glow", (pa[0], pa[1] + 0.4, pa[2]), (pb[0], pb[1] + 0.4, pb[2]),
                     (pb[0], pb[1] + 2.8, pb[2]), (pa[0], pa[1] + 2.8, pa[2]), (0, 0), (1, 0), (1, 1), (0, 1), nrm)
    # vending machines on the walkway, against the building front near its end
    for k in range(5):
        sv = sm + 0.5 * length - 1.0 - k * 1.05
        pv = lot_point(road, sv, sg * (ub0 - 0.45))[0]
        out.add_box(LAYERS_BOTH, "galvanized", (pv[0], pv[1] + 0.12 + 0.9, pv[2]), axes, (0.45, 0.9, 0.35))
        pf = lot_point(road, sv, sg * (ub0 - 0.81))[0]
        a = (pf[0] - 0.4 * road.TX[i], pf[1] + 0.5, pf[2] - 0.4 * road.TZ[i])
        b = (pf[0] + 0.4 * road.TX[i], pf[1] + 0.5, pf[2] + 0.4 * road.TZ[i])
        out.add_quad(("render",), "pa_glow", a, b, (b[0], b[1] + 1.3, b[2]), (a[0], a[1] + 1.3, a[2]),
                     (0, 0), (1, 0), (1, 1), (0, 1), nrm)
    # canopy downlights over the walkway
    for k in range(3):
        sl = sm - 0.33 * length + k * 0.33 * length
        pl = lot_point(road, sl, sg * (ub0 - 0.8))[0]
        lamps.append({"carriageway": road.id, "kind": "pa", "pos": [round(pl[0], 3), round(top - 0.1, 3), round(pl[2], 3)],
                      "dir": [0.0, -1.0, 0.0], "color": [1.0, 0.92, 0.8], "intensity": 6.0, "range": 12.0})
    log("parking apron %s on %s: s %.0f-%.0f, building %.0f m" % (lot["facility"], road.id, s0, s1, length))


def lot_place(road):
    """Centre of the parking bays (for the PA place / meet spot)."""
    lot = road.lot
    sg = 1.0 if lot["side"] == "L" else -1.0
    half = road.half_l if sg > 0 else road.half_r
    p = lot_point(road, 0.5 * (float(lot["s0"]) + float(lot["s1"])), sg * (half + 0.5 * (LOT_BAY[0] + LOT_BAY[1])))[0]
    return [round(v, 2) for v in p]


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
        u0 = [road.top(i, max(road.half_l, road.HL[i])), road.top(i, -max(road.half_r, road.HR[i]))]
        u1 = [road.top(i + 1, max(road.half_l, road.HL[i + 1])), road.top(i + 1, -max(road.half_r, road.HR[i + 1]))]
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


def emit_fill(out, road, ctx):
    """1:1 embankment fill from the deck edge down to the ground plane (ground class). Where another
    carriageway runs alongside inside the fill footprint, a vertical retaining wall replaces the slope
    (a fill would bury the neighbouring deck)."""
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
            if fill_blocked(ctx, road, i, ue, sgn, max(h0, h1)):
                b0 = (e0[0], ground_plane, e0[2])
                b1 = (e1[0], ground_plane, e1[2])
                out.add_quad(LAYERS_BOTH, "concrete_cast", e0, b0, b1, e1,
                             (road.S[i] / 3.0, 0.0), (road.S[i] / 3.0, h0 / 3.0), (road.S[i + 1] / 3.0, h1 / 3.0),
                             (road.S[i + 1] / 3.0, 0.0), (sgn * road.LX[i], 0.0, sgn * road.LZ[i]))
                continue
            want = (sgn * road.LX[i], 1.0, sgn * road.LZ[i])
            out.add_quad(LAYERS_BOTH, "ground_gravel", e0, t0, t1, e1,
                         (ue / tile, road.S[i] / tile), (ut0 / tile, road.S[i] / tile),
                         (ut1 / tile, road.S[i + 1] / tile), (ue / tile, road.S[i + 1] / tile), want)


def fill_blocked(ctx, road, i, ue, sgn, h):
    """Another deck below this edge inside the 1:1 fill footprint (within h of the edge)."""
    for f in (0.25, 0.5, 0.75, 1.0):
        u = ue + sgn * (0.3 + h * f)
        x, y, z = road.top(i, u)
        for r, si, lat, ytop in ctx.footprints(x, z, 0.2, exclude=road):
            if ytop < road.top_y(i, ue) - 0.2 and ytop > GROUND_Y - 12.0:
                return True
    return False


# ----------------------------------------------------------------------------------------------
# Landmark suspension bridges (superstructure around generated decks)
# ----------------------------------------------------------------------------------------------
# Rainbow Bridge (Tokyo, opened 1993): main span 570 m, side spans 114 m (total 798 m), towers 126 m
# above sea level, white steel towers, two-level stiffening truss (upper deck: Shuto Route 11; lower
# deck: Yurikamome and Route 482). Sources: Metropolitan Expressway Co. (Shutoko) bridge data as cited
# by Wikipedia "Rainbow Bridge (Tokyo)" (infobox: longest span 570 m, total 798 m, tower 126 m, roadway
# 52 m); the network's solved deck height on the bridge is 52 m. See docs/shuto/README.md.
SUSPENSION_BRIDGES = [
    {"name": "Rainbow Bridge", "carriageways": ["11.IN.004", "11.OUT.001"],
     "centre_xz": [-1006.5, 5309.0],       # mid-span (middle of the straight 52 m deck between the loops)
     "main_span": 570.0, "side_span": 114.0, "tower_top_y": 126.0, "sea_y": 0.0,
     "plane": 22.5,                         # cable / truss planes and tower legs, metres from the axis
     "lower_deck_y": 41.0, "cable_low_y": 55.5, "anchor_y": 47.0},
]


def bridge_frame(roads, spec):
    """Axis (centre point, unit direction along the bridge, lateral unit) fitted to the decks of the
    bridge's carriageways within the main + side spans."""
    by_id = {r.id: r for r in roads}
    cx, cz = spec["centre_xz"]
    half = 0.5 * spec["main_span"] + spec["side_span"]
    pts = []
    for cid in spec["carriageways"]:
        r = by_id.get(cid)
        if r is None:
            return None
        for i in range(r.N):
            if math.hypot(r.X[i] - cx, r.Z[i] - cz) < half - 20.0:
                pts.append((r.X[i], r.Z[i], r.TX[i], r.TZ[i]))
    if len(pts) < 50:
        return None
    tx = sum(p[2] if p[2] * pts[0][2] + p[3] * pts[0][3] > 0 else -p[2] for p in pts)
    tz = sum(p[3] if p[2] * pts[0][2] + p[3] * pts[0][3] > 0 else -p[3] for p in pts)
    ln = math.hypot(tx, tz)
    dx, dz = tx / ln, tz / ln
    lx, lz = dz, -dx
    # lateral centre: mean lateral offset of the deck samples (midway between the two carriageways)
    lat = sum((p[0] - cx) * lx + (p[1] - cz) * lz for p in pts) / len(pts)
    return (cx + lx * lat, cz + lz * lat), (dx, dz), (lx, lz)


def in_bridge(spec, frame, x, z, margin=0.0):
    """Inside the bridge's plan envelope (between the anchorages, within the truss width)."""
    (cx, cz), (dx, dz), (lx, lz) = frame
    a = (x - cx) * dx + (z - cz) * dz
    b = (x - cx) * lx + (z - cz) * lz
    return abs(a) <= 0.5 * spec["main_span"] + spec["side_span"] + margin and abs(b) <= spec["plane"] + 6.0 + margin


def emit_suspension_bridge(out, spec, frame, lamps):
    (cx, cz), (dx, dz), (lx, lz) = frame
    L, S = spec["main_span"], spec["side_span"]
    top, sea = spec["tower_top_y"], spec["sea_y"]
    pl = spec["plane"]
    deck_chord = 50.6            # truss top chord, just under the upper road slab (52 m - 1.2 m)
    low = spec["lower_deck_y"]
    axes = [(dx, 0.0, dz), (0.0, 1.0, 0.0), (lx, 0.0, lz)]

    def P(a, b, y):
        return (cx + dx * a + lx * b, y, cz + dz * a + lz * b)

    def cable_y(a):
        h = 0.5 * L
        saddle = top - 3.0
        if abs(a) <= h:
            return spec["cable_low_y"] + (saddle - spec["cable_low_y"]) * (a / h) ** 2
        t = (abs(a) - h) / S
        return saddle + (spec["anchor_y"] - saddle) * t - 4.0 * 3.0 * t * (1.0 - t)

    # Towers: two tapered legs per tower (white), cross beams (under the decks, mid height, top portal).
    for a0 in (-0.5 * L, 0.5 * L):
        out.add_box(LAYERS_BOTH, "concrete_weathered", P(a0, 0.0, sea + 1.0), axes, (12.0, 3.0, pl + 8.0))   # caisson
        segs = [(sea + 4.0, 40.0, 4.2, 3.0), (40.0, 80.0, 3.8, 2.7), (80.0, 110.0, 3.4, 2.45), (110.0, top, 3.2, 2.3)]
        for b0 in (-pl, pl):
            for y0, y1, ha, hb in segs:
                out.add_box(LAYERS_BOTH, "bridge_white", P(a0, b0, 0.5 * (y0 + y1)), axes, (ha, 0.5 * (y1 - y0), hb))
            # recessed vertical slot on the outer face (reads as the tower's paired-blade shape)
            out.add_box(("render",), "galvanized", P(a0, b0 + (1.0 if b0 > 0 else -1.0) * 2.36, 0.5 * (44.0 + top - 6.0)),
                        axes, (0.6, 0.5 * (top - 6.0 - 44.0), 0.05))
            # aviation / illumination lamp on each leg top
            out.add_box(("render",), "pa_glow", P(a0, b0, top + 0.35), axes, (0.5, 0.35, 0.5))
        for yb, hh, hw in ((37.0, 2.5, 2.2), (84.0, 2.0, 1.8), (top - 4.5, 2.5, 2.0)):
            out.add_box(LAYERS_BOTH, "bridge_white", P(a0, 0.0, yb), axes, (hw, hh, pl - 2.2))
        # top portal: a second, thinner beam just under the cap -> the open frame of the tower head
        out.add_box(("render",), "bridge_white", P(a0, 0.0, top - 11.0), axes, (1.6, 1.2, pl - 2.2))
        lamps.append({"carriageway": "landmark", "kind": "landmark", "pos": [round(v, 2) for v in P(a0, 0.0, top - 13.0)],
                      "dir": [0.0, -1.0, 0.0], "color": [1.0, 0.97, 0.92], "intensity": 4.0, "range": 40.0})
    # Main cables (with a string of small lamps: the bridge's night illumination) and suspenders.
    step = 12.0
    a = -0.5 * L - S
    xs = []
    while a <= 0.5 * L + S + 1e-6:
        xs.append(a)
        a += step / 2.0
    for b0 in (-pl, pl):
        for a1, a2 in zip(xs[:-1], xs[1:]):
            add_prism(out, "bridge_white", P(a1, b0, cable_y(a1)), P(a2, b0, cable_y(a2)), 0.45, 0.45, sides=8)
        for k, a1 in enumerate(xs):
            if k % 2 == 0:
                out.add_box(("render",), "pa_glow", P(a1, b0, cable_y(a1) + 0.55), axes, (0.18, 0.12, 0.18))
        a1 = -0.5 * L - S + step
        while a1 < 0.5 * L + S - step * 0.5:
            if abs(abs(a1) - 0.5 * L) > 5.0:
                y1 = cable_y(a1)
                if y1 - deck_chord > 1.0:
                    add_prism(out, "bridge_white", P(a1, b0, deck_chord), P(a1, b0, y1), 0.09, 0.09, sides=6)
            a1 += step
    # Two-level stiffening truss: chords, verticals and diagonals in both planes, floor beams under
    # both decks, and the lower deck slab (rail + general road level).
    a_end = 0.5 * L + S
    na = int(round(2.0 * a_end / step))
    for b0 in (-pl, pl):
        for k in range(na):
            a1 = -a_end + k * step
            a2 = a1 + step
            for y in (deck_chord, low):
                add_prism(out, "bridge_white", P(a1, b0, y), P(a2, b0, y), 0.45, 0.45, sides=6)
            add_prism(out, "bridge_white", P(a1, b0, low), P(a1, b0, deck_chord), 0.3, 0.3, sides=6)
            ya, yb = (low, deck_chord) if k % 2 == 0 else (deck_chord, low)
            add_prism(out, "bridge_white", P(a1, b0, ya), P(a2, b0, yb), 0.25, 0.25, sides=6)
    for k in range(na + 1):
        a1 = -a_end + k * step
        out.add_box(("render",), "bridge_white", P(a1, 0.0, low - 0.6), axes, (0.4, 0.6, pl))
        out.add_box(("render",), "bridge_white", P(a1, 0.0, deck_chord - 0.5), axes, (0.35, 0.5, pl))
    for k in range(na):
        a1 = -a_end + (k + 0.5) * step
        out.add_box(("render",), "concrete_cast", P(a1, 0.0, low + 0.2), axes, (0.5 * step, 0.2, pl - 0.6))
    # Anchorages at the span ends: massive blocks below the deck.
    for sg in (-1.0, 1.0):
        out.add_box(LAYERS_BOTH, "concrete_weathered", P(sg * (a_end + 18.0), 0.0, 0.5 * (sea + spec["anchor_y"] - 2.0)),
                    axes, (20.0, 0.5 * (spec["anchor_y"] - 2.0 - sea), pl + 6.0))
    log("suspension bridge %s: axis (%.1f, %.1f) dir (%.3f, %.3f), towers at +-%.0f m, %.0f m high"
        % (spec["name"], cx, cz, dx, dz, 0.5 * L, top))


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
        if any(in_bridge(sp, fr, road.X[i0], road.Z[i0], 20.0) for sp, fr in getattr(ctx, "bridges", ())):
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


def joint_segments(road):
    """Segments carrying an expansion joint (every 40 m on elevated ranges; t in [0, 0.25])."""
    out = set()
    k = 1
    while JOINT_SPACING * k < road.S[-1]:
        i = int(round(JOINT_SPACING * k))
        k += 1
        if i + 1 < road.N and road.ELEV[i]:
            out.add(i)
    return out


def emit_joints(out, road, ctx):
    """Galvanized expansion joints across the deck every 40 m on elevated ranges (clipped by deck owners)."""
    for i in sorted(joint_segments(road)):
        # 0.25 m wide strip (samples are 1 m apart), across the effective deck width.
        def at(t, u):
            p0, p1 = road.top(i, u), road.top(i + 1, u)
            return tuple(p0[k] + (p1[k] - p0[k]) * t for k in range(3))
        a = at(0.0, road.HL[i])
        b = at(0.0, -road.HR[i])
        c = at(0.25, -road.HR[i])
        d = at(0.25, road.HL[i])
        lift = (0.0, 0.008, 0.0)
        emit_decal_strip(out, road, ctx, "joint", Strip(b, c, a, d), lift, i)


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


APPROACH_GRADE = 0.06


def street_approaches(net, roads, bbox):
    """Ramps that end at a street-network boundary node above or below street level (the solved
    profile stops where the OSM expressway ways stop) get a generated approach: the deck continues
    straight on at APPROACH_GRADE down (or up) to street level and 15 m beyond, as embankment / trench
    with parapets, so the ramp meets the street instead of ending in mid-air. Approaches are geometry
    only (not in the road graph). An approach that would run into another deck or leave the section
    is not built; the ramp end is closed by a wall instead (emit_ramp_end_walls)."""
    nodes = {n["id"]: n for n in net["nodes"]}
    out_roads, closed = [], []
    for r in roads:
        for at_start in (True, False):
            nid = r.frm if at_start else r.to
            node = nodes.get(nid)
            if node is None or node.get("kind") != "boundary":
                continue
            exit_ = node.get("exit") or {}
            i = 0 if at_start else r.N - 1
            if bbox is not None:
                x0, z0, x1, z1 = bbox
                if not (x0 + 30 <= r.X[i] <= x1 - 30 and z0 + 30 <= r.Z[i] <= z1 - 30):
                    continue
            dy = GROUND_Y - (r.Y[i] + r.eps)
            if abs(dy) < 0.8:
                continue
            if exit_.get("to") != "street network":
                closed.append((r, i, at_start))
                continue
            sg = -1.0 if at_start else 1.0          # direction of the extension along the tangent
            tx, tz = sg * r.TX[i], sg * r.TZ[i]
            run = abs(dy) / APPROACH_GRADE
            length = run + 15.0
            n = int(length / 5.0) + 1
            pts = []
            ok = True
            for k in range(n + 1):
                d = length * k / n
                y = r.Y[i] + (dy if d >= run else dy * d / run)
                x, z = r.X[i] + tx * d, r.Z[i] + tz * d
                if bbox is not None and not (bbox[0] + 5 <= x <= bbox[2] - 5 and bbox[1] + 5 <= z <= bbox[3] - 5):
                    ok = False
                    break
                if k > 1:
                    for o in roads:
                        if o is r:
                            continue
                        for j in range(0, o.N, 2):
                            if abs(o.X[j] - x) < 14.0 and abs(o.Z[j] - z) < 14.0 and \
                                    math.hypot(o.X[j] - x, o.Z[j] - z) < r.half_l + o.half_l + 1.0 and \
                                    abs(o.Y[j] - y) < 7.0:
                                ok = False
                                break
                        if not ok:
                            break
                if not ok:
                    break
                pts.append([x, y, z])
            if not ok:
                closed.append((r, i, at_start))
                continue
            if at_start:
                pts = pts[::-1]
            cw = {"id": r.id + (".approach_in" if at_start else ".approach_out"), "kind": r.kind, "lanes": r.lanes,
                  "lane_width": r.lw, "shoulder_left": r.shl, "shoulder_right": r.shr,
                  "from": None if at_start else nid, "to": nid if at_start else None,
                  "points": pts, "structure": [{"s0": 0.0, "s1": length, "type": "at_grade"}]}
            ap = Road(cw)
            ap.parent = (r, i, at_start)
            out_roads.append(ap)
    log("street approaches: %d built, %d ramp ends closed" % (len(out_roads), len(closed)))
    return out_roads, closed


def attach_approaches(approaches, own):
    """After profile conformance moved a parent ramp's end, re-join its approach: height and cross
    slope are matched at the shared end and blended out over 20 m."""
    for ap in approaches:
        r, i, at_start = ap.parent
        k0 = ap.N - 1 if at_start else 0
        dy = r.Y[i] - ap.Y[k0]
        dsb = r.SB[i] - ap.SB[k0]
        for k in range(ap.N):
            d = abs(ap.S[k] - ap.S[k0])
            w = max(0.0, 1.0 - d / 20.0)
            ap.Y[k] += dy * (1.0 if d < 1e-6 else w)
            ap.SB[k] += dsb * w
            ap.BANK[k] = math.asin(clamp(ap.SB[k], -0.2, 0.2))
        own.forget(ap)


def emit_ramp_end_walls(out, closed):
    """Wall across a ramp that ends above / below street level without an approach (unbuilt link)."""
    for r, i, at_start in closed:
        l = r.top(i, r.HL[i] + 0.3)
        rr = r.top(i, -r.HR[i] - 0.3)
        h = 3.0
        fwd = (-r.TX[i], 0.0, -r.TZ[i]) if at_start else (r.TX[i], 0.0, r.TZ[i])
        n = (-fwd[0], 0.0, -fwd[2])
        out.add_quad(LAYERS_BOTH, "concrete_cast", rr, l, (l[0], l[1] + h, l[2]), (rr[0], rr[1] + h, rr[2]),
                     (0, 0), (1, 0), (1, 1), (0, 1), n)
        out.add_quad(LAYERS_BOTH, "concrete_cast", l, rr, (rr[0], rr[1] + h, rr[2]), (l[0], l[1] + h, l[2]),
                     (0, 0), (1, 0), (1, 1), (0, 1), fwd)


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
    edge = road.HL[i]
    if bbox is not None:
        x0, z0, x1, z1 = bbox
        if not (x0 + 15.0 <= road.X[i] <= x1 - 15.0 and z0 + 15.0 <= road.Z[i] <= z1 - 15.0):
            return None
    gy = GROUND_Y - 0.05
    if road.ELEV[i]:
        if barrier_omitted(road, ctx, i, 1.0):
            return None
        u = edge + 0.1
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
        u_end = max(road.half_l - 1.2, u_pole - 3.5)   # arm over the lanes (at most 3.5 m on a wide apron)
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
        if all(c is None for c in samples) and size > MIN_SIZE + 1e-6 and ctx.cover_near(ix, iz, size):
            samples.append("?")   # a narrow trench / at-grade deck may pass between the samples: refine
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
    mats["bridge_white"] = make_material("bridge_white", None, 0.45, 0.1, color=(0.86, 0.87, 0.88, 1.0))
    mats["pa_glow"] = make_material("pa_glow", None, 0.3, 0.0, color=(0.9, 0.9, 0.85, 1.0),
                                    emit=(1.0, 0.94, 0.84, 1.0), emit_strength=5.0)
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
    approaches, closed_ends = street_approaches(net, roads, bbox)
    roads += approaches
    mitre_nodes(roads)
    ctx = Context(roads)
    ctx.compute_edges()
    apply_lots(net, roads)
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

    ctx.bridges = []
    for spec in SUSPENSION_BRIDGES:
        fr = bridge_frame(roads, spec)
        if fr is not None and (bbox is None or (bbox[0] <= fr[0][0] <= bbox[2] and bbox[1] <= fr[0][1] <= bbox[3])):
            ctx.bridges.append((spec, fr))
    log("emitting %d roads" % len(roads))
    ctx.own = DeckOwnership(roads)
    ctx.deck_tris = {}
    deck_offsets = conform_profiles(roads, ctx.own)
    attach_approaches(approaches, ctx.own)
    log("profile conformance: %d roads adjusted (max %d mm)" % (len(deck_offsets), ctx.own.stats["conformed_max_mm"]))
    # Decks first, lowest priority first: a clipped deck inserts its seam points into the owners'
    # edges, which are emitted afterwards (watertight seams, no T-junctions).
    for r in sorted(roads, key=marking_priority):
        emit_deck(out, r, ctx)
    log("deck ownership: %s, seam points inserted: %d" % (
        dict(ctx.own.stats), sum(len(v) for v in ctx.own.inserts.values())))
    lots = []
    for r in roads:
        emit_markings(out, r, ctx)
        if r.lot is not None:
            emit_lot_markings(out, r, ctx)
            emit_lot(out, r, ctx, lamps)
            lots.append({"facility": r.lot["facility"], "carriageway": r.id, "name": r.lot["name"], "pos": lot_place(r),
                         "s0": r.lot["s0"], "s1": r.lot["s1"], "side": r.lot["side"], "width": r.lot["width"],
                         "taper": r.lot.get("taper", 10.0)})
        emit_barriers(out, r, ctx)
        emit_tunnel(out, r, ctx)
        emit_slabs(out, r)
        emit_fill(out, r, ctx)
        emit_piers(out, r, ctx)
        emit_joints(out, r, ctx)
        emit_portals(out, r)
        emit_poles(out, r, ctx, lamps, bbox)
        emit_tunnel_fixtures(out, r, lamps)
        emit_section_ends(out, r, bbox)
        if args.noise_barriers:
            emit_noise(out, r)
    for spec, fr in ctx.bridges:
        emit_suspension_bridge(out, spec, fr, lamps)
    emit_ramp_end_walls(out, closed_ends)
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
    with open(os.path.join(out_dir, "deck_offsets.json"), "w", encoding="ascii", newline="\n") as f:
        json.dump({"format": "nox-shuto-deck-offsets", "version": 1,
                   "note": "per-sample (1 m) centreline height offsets applied over the network profile "
                           "where a lower-priority deck conforms to a same-level owner (conform_profiles)",
                   "offsets": deck_offsets, "lots": lots,
                   "closed_ends": [[r.id, 0 if at_start else r.N - 1] for r, i, at_start in closed_ends],
                   "approaches": [a.id for a in approaches]}, f)
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
