"""Shuto rep-section scenery planner (pure Python, stdlib only).

Fetches OpenStreetMap data for the representative section bbox (cached under
data/shuto/raw/overpass_scenery.json), projects it into the NOX frame used by
data/shuto/network.json (x east, z south, metres, origin Edobashi JCT) and writes
models/shuto/rep_section/scenery_plan.json for tools/shuto/blender/build_scenery.py.

Run with:
  python -I tools/shuto/scenery_plan.py            (add --refresh to re-fetch)

Plan contents:
  water_rects   water surface rectangles at WATER_Y (4 m raster, merged), split per chunk.
                Sea = coastline-enclosed components on the side OSM marks as sea (right of
                the way), plus natural=water / riverbank polygons, plus river/canal lines.
                Water cells under at-grade road corridors (deck y < 4 m) are removed.
  buildings     OSM building footprints with centroid in the bbox, minus those whose footprint
                comes within the road corridor (carriageway half-width + 4 m) or stands in water.
                Each has height, facade family, OBB, distance to the expressway and an LOD tag
                ('full' textured per-storey/bay facades, or 'plain' untextured masses) chosen
                to fit the render triangle budget.
  landmarks     kit building placements (CC-BY asian_themed kit only) on tall footprints.
  containers    container stacks on industrial landuse, away from roads and water.
  trees         sparse foliage card positions in parks.
ODbL attribution: (c) OpenStreetMap contributors.
"""
import argparse
import json
import math
import random
import sys
import time
import urllib.request
from collections import defaultdict, deque
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RAW = ROOT / "data" / "shuto" / "raw"
NET = ROOT / "data" / "shuto" / "network.json"
KIT_INDEX = ROOT / "models" / "shuto" / "kit" / "kit_index.json"
PLAN = ROOT / "models" / "shuto" / "rep_section" / "scenery_plan.json"

UA = "NoxShutoBuilder/1.0 (Nox_Engine Shuto scenery build)"
MIRRORS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
]

ORIGIN_LAT, ORIGIN_LON = 35.6843, 139.7745
BBOX = (-2600.0, 1500.0, 0.0, 6000.0)   # x0, z0, x1, z1 (NOX frame)
CELL = 4.0            # water raster cell (m); divides 500 m chunk edges exactly
CHUNK = 500.0
GROUND_Y = 3.0
WATER_Y = 3.05
STOREY = 3.3
BAY = {"apartment": 3.5, "commercial": 4.0, "industrial": 6.0}
BAND_MARGIN = 4.0     # m beyond each carriageway band (lanes + both shoulders)
PLAN_SLACK = 1.5      # extra planning margin on top of the band (checker uses the band exactly)
LANDMARK_CAP = 120.0 # m, kit landmark height cap outside the Shiodome cluster
SHIODOME_C = (-1130.0, 2425.0)  # NOX centre of the Shiodome tower cluster
SHIODOME_R = 350.0
SHIODOME_CAP = 250.0 # m, cap inside the Shiodome cluster (OSM heights used up to this)
NEAR_M = 350.0        # full facades within this distance of an expressway
RENDER_BUDGET = 1300000  # render triangles reserved for buildings/water/props (bbox total <= 1.5M)
KIT_MAX = 120
# District anchors (NOX frame). Ginza = sprint start (-425, 1657); Shiodome = tower cluster;
# Daiba = Rainbow Bridge landing (checkpoint near (-527, 5477)); Hamamatsucho/Shibaura is the rest.
# Approximate regions for scenery character only, not administrative boundaries.
GINZA_C, GINZA_R = (-425.0, 1657.0), 700.0
DAIBA_C, DAIBA_R = (-700.0, 5300.0), 900.0
ZONE_STOREYS = {"ginza": (8, 12), "shiodome": (14, 26), "hamamatsucho": (6, 13),
                "waterfront": (2, 4), "daiba": (6, 16)}
STREET_W = {"primary": 14.0, "secondary": 11.0, "tertiary": 9.0, "residential": 7.0}
SIDEWALK_W = {"primary": 3.0, "secondary": 2.5, "tertiary": 2.2, "residential": 1.8}
STREET_LAMP_STEP = 30.0
TREE_STEP = 14.0


def log(msg):
    print("[scenery_plan] " + msg, flush=True)


# ---------------------------------------------------------------- projection (copy of build_network.project)

_A = 6378137.0
_F = 1 / 298.257223563
_N = _F / (2 - _F)
_E = math.sqrt(_F * (2 - _F))
_ABIG = _A / (1 + _N) * (1 + _N ** 2 / 4 + _N ** 4 / 64)
_ALPHA = [_N / 2 - 2 * _N ** 2 / 3 + 5 * _N ** 3 / 16,
          13 * _N ** 2 / 48 - 3 * _N ** 3 / 5,
          61 * _N ** 3 / 240]


def _tm_raw(lat, lon):
    phi = math.radians(lat)
    lam = math.radians(lon - ORIGIN_LON)
    tau = math.tan(phi)
    sig = math.sinh(_E * math.atanh(_E * tau / math.sqrt(1 + tau * tau)))
    taup = tau * math.sqrt(1 + sig * sig) - sig * math.sqrt(1 + tau * tau)
    xi = math.atan2(taup, math.cos(lam))
    eta = math.asinh(math.sin(lam) / math.hypot(taup, math.cos(lam)))
    e_, n_ = eta, xi
    for j, al in enumerate(_ALPHA, 1):
        e_ += al * math.cos(2 * j * xi) * math.sinh(2 * j * eta)
        n_ += al * math.sin(2 * j * xi) * math.cosh(2 * j * eta)
    return _ABIG * e_, _ABIG * n_


_E0, _N0 = _tm_raw(ORIGIN_LAT, ORIGIN_LON)


def project(lat, lon):
    e_, n_ = _tm_raw(lat, lon)
    return e_ - _E0, -(n_ - _N0)


def latlon_bbox(margin_deg=0.01):
    x0, z0, x1, z1 = BBOX
    lat_s = ORIGIN_LAT - z1 / 111200.0
    lat_n = ORIGIN_LAT - z0 / 111200.0
    cos0 = math.cos(math.radians(ORIGIN_LAT))
    lon_w = ORIGIN_LON + x0 / (111320.0 * cos0)
    lon_e = ORIGIN_LON + x1 / (111320.0 * cos0)
    return (lat_s - margin_deg, lon_w - margin_deg, lat_n + margin_deg, lon_e + margin_deg)


# ---------------------------------------------------------------- fetch

KINDS = {
    "buildings": ['way["building"]{b};', 'relation["building"]["type"="multipolygon"]{b};'],
    "water": ['way["natural"="water"]{b};', 'relation["natural"="water"]["type"="multipolygon"]{b};',
              'way["waterway"="riverbank"]{b};', 'relation["waterway"="riverbank"]{b};',
              'way["natural"="coastline"]{b};', 'way["waterway"~"^(river|canal)$"]{b};'],
    "streets": ['way["highway"~"^(primary|secondary|tertiary|residential)$"]{b};'],
    "landuse": ['way["leisure"="park"]{b};', 'relation["leisure"="park"]{b};',
                'way["landuse"~"^(industrial|recreation_ground|port)$"]{b};',
                'relation["landuse"~"^(industrial|recreation_ground|port)$"]{b};'],
}
TILES = 3  # tiles per axis; keeps each Overpass request small


def tile_boxes():
    s, w, n, e = latlon_bbox()
    out = []
    for i in range(TILES):
        for j in range(TILES):
            s0 = s + (n - s) * i / TILES
            s1 = s + (n - s) * (i + 1) / TILES
            w0 = w + (e - w) * j / TILES
            w1 = w + (e - w) * (j + 1) / TILES
            out.append(("t%d%d" % (i, j), (s0, w0, s1, w1)))
    return out


def build_query(kind, box):
    b = "(%.5f,%.5f,%.5f,%.5f)" % box
    body = "\n".join(x.format(b=b) for x in KINDS[kind])
    return "[out:json][timeout:600][maxsize:536870912];\n(\n" + body + "\n);\nout body geom;\n"


def post_overpass(query):
    last = None
    for attempt in range(6):
        for url in MIRRORS:
            try:
                req = urllib.request.Request(url, data=query.encode("utf-8"), headers={"User-Agent": UA})
                with urllib.request.urlopen(req, timeout=700) as r:
                    body = r.read().decode("utf-8")
                data = json.loads(body)
                if "elements" not in data:
                    raise ValueError("no elements key")
                return body, data
            except Exception as ex:  # try the next mirror / attempt
                last = ex
                log("  %s failed: %s" % (url, ex))
        time.sleep(20 * (attempt + 1))
    raise SystemExit("Overpass fetch failed: %s" % last)


def fetch_overpass(refresh):
    """Fetch (or load cached) tiles x kinds. Returns deduplicated elements list."""
    RAW.mkdir(parents=True, exist_ok=True)
    seen = {}
    for tname, box in tile_boxes():
        for kind in KINDS:
            path = RAW / ("overpass_scenery_%s_%s.json" % (kind, tname))
            if path.exists() and not refresh:
                data = json.loads(path.read_text(encoding="utf-8"))
                log("cache: %s (%d)" % (path.name, len(data["elements"])))
            else:
                body, data = post_overpass(build_query(kind, box))
                path.write_text(body, encoding="utf-8")
                log("fetched %s %s (%d elements)" % (kind, tname, len(data["elements"])))
            for el in data["elements"]:
                seen[(el["type"], el["id"])] = el
    return list(seen.values())


# ---------------------------------------------------------------- geometry helpers

def in_bbox(x, z, pad=0.0):
    x0, z0, x1, z1 = BBOX
    return x0 - pad <= x <= x1 + pad and z0 - pad <= z <= z1 + pad


def area2(ring):
    a = 0.0
    n = len(ring)
    for i in range(n):
        x0, z0 = ring[i]
        x1, z1 = ring[(i + 1) % n]
        a += x0 * z1 - x1 * z0
    return 0.5 * a


def clean_ring(ring):
    out = []
    for p in ring:
        if not out or (abs(p[0] - out[-1][0]) > 1e-6 or abs(p[1] - out[-1][1]) > 1e-6):
            out.append(p)
    if len(out) > 1 and abs(out[0][0] - out[-1][0]) < 1e-6 and abs(out[0][1] - out[-1][1]) < 1e-6:
        out.pop()
    return out


def point_in_ring(x, z, ring):
    inside = False
    n = len(ring)
    j = n - 1
    for i in range(n):
        xi, zi = ring[i]
        xj, zj = ring[j]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi + 1e-12) + xi:
            inside = not inside
        j = i
    return inside


def obb(ring):
    """Oriented box from the longest edge. Returns centre, unit axis u, width along u, depth."""
    n = len(ring)
    best = None
    for i in range(n):
        xa, za = ring[i]
        xb, zb = ring[(i + 1) % n]
        L = math.hypot(xb - xa, zb - za)
        if best is None or L > best[0]:
            best = (L, (xb - xa) / max(L, 1e-9), (zb - za) / max(L, 1e-9))
    _, ux, uz = best
    vx, vz = -uz, ux
    pu = [p[0] * ux + p[1] * uz for p in ring]
    pv = [p[0] * vx + p[1] * vz for p in ring]
    u0, u1, v0, v1 = min(pu), max(pu), min(pv), max(pv)
    cu, cv = (u0 + u1) / 2, (v0 + v1) / 2
    cx, cz = cu * ux + cv * vx, cu * uz + cv * vz
    return {"c": [round(cx, 2), round(cz, 2)], "u": [round(ux, 5), round(uz, 5)],
            "w": round(u1 - u0, 2), "d": round(v1 - v0, 2)}


def parse_height(tags):
    h = tags.get("height")
    if h:
        try:
            return float(h.replace("m", "").replace(",", ".").strip())
        except ValueError:
            pass
    lv = tags.get("building:levels")
    if lv:
        try:
            return float(lv) * STOREY
        except ValueError:
            pass
    return None


def family_of(tags, h, z):
    b = tags.get("building", "yes")
    if b in ("apartments", "residential", "house", "detached", "dormitory", "terrace", "semidetached_house"):
        return "apartment"
    if b in ("commercial", "office", "retail", "hotel", "mixed_use", "supermarket", "civic", "public"):
        return "commercial"
    if b in ("industrial", "warehouse", "manufacture", "factory", "hangar", "storage_tank", "shed", "transformer_tower"):
        return "industrial"
    if h is not None and h >= 25:
        return "commercial"
    return "industrial" if z > 3000 else "apartment"


def default_height(family):
    return {"commercial": 30.0, "apartment": 16.0, "industrial": 10.0}[family]


# ---------------------------------------------------------------- road corridor

def seg_dist2(px, pz, ax, az, bx, bz):
    dx, dz = bx - ax, bz - az
    L2 = dx * dx + dz * dz
    if L2 <= 1e-12:
        return (px - ax) ** 2 + (pz - az) ** 2
    t = ((px - ax) * dx + (pz - az) * dz) / L2
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    qx, qz = ax + dx * t, az + dz * t
    return (px - qx) ** 2 + (pz - qz) ** 2


def band_halfwidth(cw):
    """Carriageway band half-width: lanes*lane_width/2 + both shoulders + BAND_MARGIN (m)."""
    return (cw["lanes"] * cw["lane_width"] / 2.0 + cw.get("shoulder_left", 0.0)
            + cw.get("shoulder_right", 0.0) + BAND_MARGIN)


class RoadIndex:
    """Carriageway bands as centreline segments with per-carriageway half-width.

    near(x, z, extra) is True when (x, z) is within band + extra of a centreline segment.
    footprint_conflict(ring) is True when a footprint (any height) touches a band: its boundary
    comes within band + PLAN_SLACK of a segment, or a centreline point lies inside it.
    """
    CELL = 40.0

    def __init__(self, net):
        self.seg_cells = defaultdict(list)
        self.pt_cells = defaultdict(list)
        self.samples = []  # 1 m samples (x, y, z, hw) for the water cut and expressway distance
        self.hw_max = 0.0
        for cw in net["carriageways"]:
            pts = cw["points"]
            if not any(in_bbox(p[0], p[2], 60.0) for p in pts):
                continue
            hw = band_halfwidth(cw)
            self.hw_max = max(self.hw_max, hw)
            for p in pts:
                self.pt_cells[self._cell(p[0], p[2])].append((p[0], p[2]))
            for a, b in zip(pts[:-1], pts[1:]):
                self._add_seg(a[0], a[2], b[0], b[2], hw)
                L = math.dist((a[0], a[2]), (b[0], b[2]))
                n = max(1, int(math.ceil(L / 1.0)))
                for k in range(n):
                    t = k / n
                    self.samples.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                                         a[2] + (b[2] - a[2]) * t, hw))
            self.samples.append((pts[-1][0], pts[-1][1], pts[-1][2], hw))
        assert self.hw_max + PLAN_SLACK < self.CELL, "band half-width exceeds hash cell"

    def _cell(self, x, z):
        return (math.floor(x / self.CELL), math.floor(z / self.CELL))

    def _add_seg(self, ax, az, bx, bz, hw):
        seg = (ax, az, bx, bz, hw)
        i0, k0 = self._cell(min(ax, bx), min(az, bz))
        i1, k1 = self._cell(max(ax, bx), max(az, bz))
        for i in range(i0, i1 + 1):
            for k in range(k0, k1 + 1):
                self.seg_cells[(i, k)].append(seg)

    def near(self, x, z, extra):
        i, k = self._cell(x, z)
        for di in (-1, 0, 1):
            for dk in (-1, 0, 1):
                for (ax, az, bx, bz, hw) in self.seg_cells.get((i + di, k + dk), ()):
                    r = hw + extra
                    if seg_dist2(x, z, ax, az, bx, bz) <= r * r:
                        return True
        return False

    def footprint_conflict(self, ring, extra=None):
        extra = PLAN_SLACK if extra is None else extra
        for x, z in densify(ring, 1.5):
            if self.near(x, z, extra):
                return True
        xs = [p[0] for p in ring]
        zs = [p[1] for p in ring]
        x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
        i0, k0 = self._cell(x0, z0)
        i1, k1 = self._cell(x1, z1)
        for i in range(i0, i1 + 1):
            for k in range(k0, k1 + 1):
                for (px, pz) in self.pt_cells.get((i, k), ()):
                    if x0 <= px <= x1 and z0 <= pz <= z1 and point_in_ring(px, pz, ring):
                        return True
        return False


def rect_ring(o):
    """Corners of an OBB dict {c, u, w, d} (NOX x, z)."""
    cx, cz = o["c"]
    ux, uz = o["u"]
    vx, vz = -uz, ux
    hw_, hd = o["w"] / 2.0, o["d"] / 2.0
    out = []
    for a, b in ((-hw_, -hd), (hw_, -hd), (hw_, hd), (-hw_, hd)):
        out.append((cx + ux * a + vx * b, cz + uz * a + vz * b))
    return out


def densify(ring, step=1.5):
    out = []
    n = len(ring)
    for i in range(n):
        xa, za = ring[i]
        xb, zb = ring[(i + 1) % n]
        L = math.hypot(xb - xa, zb - za)
        k = max(1, int(math.ceil(L / step)))
        for j in range(k):
            t = j / k
            out.append((xa + (xb - xa) * t, za + (zb - za) * t))
    return out


# ---------------------------------------------------------------- water raster

class Grid:
    def __init__(self):
        x0, z0, x1, z1 = BBOX
        self.x0, self.z0 = x0, z0
        self.nx = int(math.ceil((x1 - x0) / CELL))
        self.nz = int(math.ceil((z1 - z0) / CELL))
        self.n = self.nx * self.nz

    def idx(self, x, z):
        i = math.floor((x - self.x0) / CELL)
        j = math.floor((z - self.z0) / CELL)
        if 0 <= i < self.nx and 0 <= j < self.nz:
            return j * self.nx + i
        return -1

    def centre(self, c):
        j, i = divmod(c, self.nx)
        return self.x0 + (i + 0.5) * CELL, self.z0 + (j + 0.5) * CELL

    def fill_ring(self, ring, mark):
        zs = [p[1] for p in ring]
        j0 = max(0, int(math.floor((min(zs) - self.z0) / CELL - 0.5)))
        j1 = min(self.nz - 1, int(math.ceil((max(zs) - self.z0) / CELL - 0.5)))
        rows = defaultdict(list)
        n = len(ring)
        for k in range(n):
            xa, za = ring[k]
            xb, zb = ring[(k + 1) % n]
            if za == zb:
                continue
            lo, hi = min(za, zb), max(za, zb)
            for j in range(j0, j1 + 1):
                zc = self.z0 + (j + 0.5) * CELL
                if lo <= zc < hi:
                    rows[j].append(xa + (zc - za) * (xb - xa) / (zb - za))
        for j, xs in rows.items():
            xs.sort()
            for a in range(0, len(xs) - 1, 2):
                i0 = max(0, int(math.ceil((xs[a] - self.x0) / CELL - 0.5)))
                i1 = min(self.nx - 1, int(math.floor((xs[a + 1] - self.x0) / CELL - 0.5)))
                for i in range(i0, i1 + 1):
                    mark[j * self.nx + i] = 1

    def mark_line(self, pts, half, mark):
        for (xa, za), (xb, zb) in zip(pts[:-1], pts[1:]):
            L = math.hypot(xb - xa, zb - za)
            k = max(1, int(math.ceil(L / 1.0)))
            for s in range(k + 1):
                t = s / k
                x = xa + (xb - xa) * t
                z = za + (zb - za) * t
                i0 = max(0, int(math.floor((x - half - self.x0) / CELL)))
                i1 = min(self.nx - 1, int(math.floor((x + half - self.x0) / CELL)))
                j0 = max(0, int(math.floor((z - half - self.z0) / CELL)))
                j1 = min(self.nz - 1, int(math.floor((z + half - self.z0) / CELL)))
                for j in range(j0, j1 + 1):
                    zc = self.z0 + (j + 0.5) * CELL
                    for i in range(i0, i1 + 1):
                        xc = self.x0 + (i + 0.5) * CELL
                        if (xc - x) ** 2 + (zc - z) ** 2 <= half * half:
                            mark[j * self.nx + i] = 1


def build_water(grid, coast_lines, water_polys, canal_lines, roads):
    N = grid.n
    barrier = bytearray(N)
    sea_pts, land_pts = [], []
    for pts in coast_lines:
        for (xa, za), (xb, zb) in zip(pts[:-1], pts[1:]):
            L = math.hypot(xb - xa, zb - za)
            if L < 1e-6:
                continue
            dx, dz = (xb - xa) / L, (zb - za) / L
            rx, rz = -dz, dx  # right of travel = sea side (OSM convention)
            k = max(1, int(math.ceil(L / 2.0)))
            for s in range(k + 1):
                t = s / k
                x = xa + (xb - xa) * t
                z = za + (zb - za) * t
                c = grid.idx(x, z)
                if c >= 0:
                    barrier[c] = 1
                cs = grid.idx(x + 3.0 * rx, z + 3.0 * rz)
                cl = grid.idx(x - 3.0 * rx, z - 3.0 * rz)
                if cs >= 0:
                    sea_pts.append(cs)
                if cl >= 0:
                    land_pts.append(cl)
    log("coastline barrier cells: %d" % sum(barrier))

    comp = [-1] * N
    ncomp = 0
    for start in range(N):
        if barrier[start] or comp[start] >= 0:
            continue
        comp[start] = ncomp
        q = deque([start])
        while q:
            c = q.popleft()
            j, i = divmod(c, grid.nx)
            if i > 0 and not barrier[c - 1] and comp[c - 1] < 0:
                comp[c - 1] = ncomp
                q.append(c - 1)
            if i < grid.nx - 1 and not barrier[c + 1] and comp[c + 1] < 0:
                comp[c + 1] = ncomp
                q.append(c + 1)
            if j > 0 and not barrier[c - grid.nx] and comp[c - grid.nx] < 0:
                comp[c - grid.nx] = ncomp
                q.append(c - grid.nx)
            if j < grid.nz - 1 and not barrier[c + grid.nx] and comp[c + grid.nx] < 0:
                comp[c + grid.nx] = ncomp
                q.append(c + grid.nx)
        ncomp += 1
    log("water flood components: %d" % ncomp)
    sv = [0] * ncomp
    lv = [0] * ncomp
    for c in sea_pts:
        if comp[c] >= 0:
            sv[comp[c]] += 1
    for c in land_pts:
        if comp[c] >= 0:
            lv[comp[c]] += 1
    is_sea = [sv[k] > lv[k] for k in range(ncomp)]
    water = bytearray(N)
    for c in range(N):
        if comp[c] >= 0 and is_sea[comp[c]]:
            water[c] = 1
    log("sea components: %d of %d, cells %d" % (sum(is_sea), ncomp, sum(water)))

    for ring in water_polys:
        grid.fill_ring(ring, water)
    for pts, half in canal_lines:
        grid.mark_line(pts, half, water)

    # At-grade roads are not water: remove water under decks lower than 4 m.
    cut = bytearray(N)
    for x, y, z, hw in roads.samples:
        if y >= 4.0:
            continue
        i0 = max(0, int(math.floor((x - hw - grid.x0) / CELL)))
        i1 = min(grid.nx - 1, int(math.floor((x + hw - grid.x0) / CELL)))
        j0 = max(0, int(math.floor((z - hw - grid.z0) / CELL)))
        j1 = min(grid.nz - 1, int(math.floor((z + hw - grid.z0) / CELL)))
        for j in range(j0, j1 + 1):
            for i in range(i0, i1 + 1):
                xc, zc = grid.x0 + (i + 0.5) * CELL, grid.z0 + (j + 0.5) * CELL
                if (xc - x) ** 2 + (zc - z) ** 2 <= hw * hw:
                    cut[j * grid.nx + i] = 1
    for c in range(N):
        if cut[c]:
            water[c] = 0
    log("water cells after road cut: %d" % sum(water))
    return water


def water_rects(grid, water):
    """Merge water cells into rectangles (greedy vertical merge of identical row runs)."""
    rects = []
    active = {}
    for j in range(grid.nz + 1):
        runs = []
        if j < grid.nz:
            base = j * grid.nx
            i = 0
            while i < grid.nx:
                if water[base + i]:
                    i0 = i
                    while i < grid.nx and water[base + i]:
                        i += 1
                    runs.append((i0, i))
                else:
                    i += 1
        new_active = {}
        for run in runs:
            new_active[run] = active.pop(run, j)
        for (i0, i1), start in active.items():
            rects.append((i0, start, i1, j))
        active = new_active
    out = []
    for i0, j0, i1, j1 in rects:
        out.append((grid.x0 + i0 * CELL, grid.z0 + j0 * CELL, grid.x0 + i1 * CELL, grid.z0 + j1 * CELL))
    return out


def split_chunks(x0, z0, x1, z1):
    """Split a world rectangle at 500 m chunk boundaries. Yields (cx, cz, x0, z0, x1, z1)."""
    out = []
    cx0, cx1 = math.floor(x0 / CHUNK), math.floor((x1 - 1e-6) / CHUNK)
    cz0, cz1 = math.floor(z0 / CHUNK), math.floor((z1 - 1e-6) / CHUNK)
    for cx in range(cx0, cx1 + 1):
        for cz in range(cz0, cz1 + 1):
            ax, bx = max(x0, cx * CHUNK), min(x1, (cx + 1) * CHUNK)
            az, bz = max(z0, cz * CHUNK), min(z1, (cz + 1) * CHUNK)
            if bx > ax and bz > az:
                out.append((cx, cz, ax, az, bx, bz))
    return out


def chunk_of(x, z):
    return math.floor(x / CHUNK), math.floor(z / CHUNK)


# ---------------------------------------------------------------- expressway distance field

def expressway_distance(roads, step=20.0):
    x0, z0, x1, z1 = BBOX
    nx = int(math.ceil((x1 - x0) / step)) + 1
    nz = int(math.ceil((z1 - z0) / step)) + 1
    dist = [-1] * (nx * nz)
    q = deque()
    for x, y, z, hw in roads.samples:
        i = int(round((x - x0) / step))
        j = int(round((z - z0) / step))
        if 0 <= i < nx and 0 <= j < nz and dist[j * nx + i] < 0:
            dist[j * nx + i] = 0
            q.append((i, j))
    while q:
        i, j = q.popleft()
        d = dist[j * nx + i]
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ii, jj = i + di, j + dj
            if 0 <= ii < nx and 0 <= jj < nz and dist[jj * nx + ii] < 0:
                dist[jj * nx + ii] = d + 1
                q.append((ii, jj))

    def at(x, z):
        i = int(round((x - x0) / step))
        j = int(round((z - z0) / step))
        i = min(max(i, 0), nx - 1)
        j = min(max(j, 0), nz - 1)
        d = dist[j * nx + i]
        return (d if d >= 0 else 9999) * step

    return at


# ---------------------------------------------------------------- main

def tri_counts(ring, h, family, lod):
    n = len(ring)
    roof = 2 * max(0, n - 2)
    if lod == "plain":
        return 2 * n + roof
    storeys = max(1, int(math.ceil(h / STOREY)))
    bay = BAY[family]
    walls = 0
    for i in range(n):
        xa, za = ring[i]
        xb, zb = ring[(i + 1) % n]
        L = math.hypot(xb - xa, zb - za)
        walls += 2 * max(1, int(round(L / bay))) * storeys
    return walls + roof


def pick_style(zone, fam, h, r):
    """Facade style (names in textures/shuto/derived/facade_styles) by district, family and height."""
    if zone == "ginza":
        if fam == "commercial":
            return r.choice(["spandrel_midrise", "office_ribbon", "tile_older"])
        return r.choice(["tile_older", "apartment_concrete"])
    if zone == "shiodome":
        if h >= 60.0:
            return r.choice(["curtain_mullion", "curtain_bronze"])
        return r.choice(["office_ribbon", "ivory_modern", "spandrel_midrise"])
    if zone == "waterfront":
        return r.choice(["warehouse_clerestory", "warehouse_roller"])
    if zone == "daiba":
        return r.choice(["apartment_balcony", "ivory_modern", "spandrel_midrise"])
    if fam == "apartment":
        return r.choice(["apartment_balcony", "apartment_concrete", "tile_older"])
    return r.choice(["office_ribbon", "dark_office", "spandrel_midrise"])


def zone_of(c, near_water):
    if math.dist(c, SHIODOME_C) < SHIODOME_R:
        return "shiodome"
    if near_water and c[1] > 3000:
        return "waterfront"
    if math.dist(c, GINZA_C) < GINZA_R and c[1] < 2300:
        return "ginza"
    if c[1] > 4800 and math.dist(c, DAIBA_C) < DAIBA_R:
        return "daiba"
    return "hamamatsucho"


def seg_street_hit(x, z, segs, cells, cell_m=50.0):
    """True if (x, z) lies within the clearance of a street segment (bucketed lookup)."""
    for sid in cells.get((int(math.floor(x / cell_m)), int(math.floor(z / cell_m))), ()):
        ax, az, bx, bz, clear = segs[sid]
        if seg_dist2(x, z, ax, az, bx, bz) < clear * clear:
            return True
    return False


def add_seg_cells(segs, cells, cell_m=50.0):
    for sid, (ax, az, bx, bz, _c) in enumerate(segs):
        L = math.dist((ax, az), (bx, bz))
        n = max(1, int(L / 20.0))
        for k in range(n + 1):
            t = k / n
            x = ax + (bx - ax) * t
            z = az + (bz - az) * t
            key = (int(math.floor(x / cell_m)), int(math.floor(z / cell_m)))
            cells.setdefault(key, set()).add(sid)


def plan_streets(street_ways, roads, grid, water):
    """City street polylines, split where they meet an expressway band or water."""
    pieces = []
    for tags, pts in street_ways:
        cls = tags["highway"]
        w = STREET_W[cls]
        sw = SIDEWALK_W[cls]
        run = []
        for i in range(len(pts) - 1):
            a, b = pts[i], pts[i + 1]
            L = math.dist(a, b)
            if L < 1.0:
                continue
            mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
            ok = in_bbox(mid[0], mid[1])
            if ok:
                gi = grid.idx(mid[0], mid[1])
                ok = not (gi >= 0 and water[gi])
            if ok:
                ux, uz = (b[0] - a[0]) / L, (b[1] - a[1]) / L
                nx, nz = -uz, ux
                e = w / 2 + sw + 0.8
                ring = [(a[0] - ux + nx * e, a[1] - uz + nz * e), (b[0] + ux + nx * e, b[1] + uz + nz * e),
                        (b[0] + ux - nx * e, b[1] + uz - nz * e), (a[0] - ux - nx * e, a[1] - uz - nz * e)]
                ok = not roads.footprint_conflict(ring)
            if ok:
                if not run:
                    run = [a]
                run.append(b)
            else:
                if len(run) >= 2:
                    pieces.append({"cls": cls, "w": w, "sw": sw,
                                   "pts": [[round(p[0], 2), round(p[1], 2)] for p in run]})
                run = []
        if len(run) >= 2:
            pieces.append({"cls": cls, "w": w, "sw": sw, "pts": [[round(p[0], 2), round(p[1], 2)] for p in run]})
    return pieces


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true")
    args = ap.parse_args()
    random.seed(20021231)
    t0 = time.time()

    elements = fetch_overpass(args.refresh)
    net = json.loads(NET.read_text(encoding="utf-8"))
    roads = RoadIndex(net)
    log("road samples: %d (band hw max %.2f)" % (len(roads.samples), roads.hw_max))

    buildings, coast_lines, water_polys, canal_lines, parks, industrial = [], [], [], [], [], []
    street_ways = []
    for el in elements:
        tags = el.get("tags", {})
        etype = el["type"]
        rings = []
        if etype == "way":
            g = el.get("geometry")
            if g:
                rings = [[project(p["lat"], p["lon"]) for p in g]]
                if tags.get("natural") == "coastline" and "building" not in tags:
                    coast_lines.append(rings[0])
                    continue
        elif etype == "relation":
            for m in el.get("members", []):
                if m.get("role") in ("outer", "") and m.get("geometry") and m.get("type", "way") == "way":
                    rings.append([project(p["lat"], p["lon"]) for p in m["geometry"]])
        if not rings:
            continue
        if etype == "way" and tags.get("highway") in STREET_W:
            street_ways.append((tags, rings[0]))
            continue
        if "building" in tags:
            for r in rings:
                buildings.append((el["id"], tags, r))
        elif tags.get("natural") == "water" or tags.get("waterway") == "riverbank":
            water_polys.extend(rings)
        elif tags.get("waterway") in ("river", "canal") and etype == "way":
            w = tags.get("width")
            try:
                half = float(w) / 2.0
            except (TypeError, ValueError):
                half = 6.0 if tags["waterway"] == "canal" else 20.0
            canal_lines.append((rings[0], half))
        elif tags.get("leisure") == "park" or tags.get("landuse") == "recreation_ground":
            parks.extend(rings)
        elif tags.get("landuse") in ("industrial", "port"):
            industrial.extend(rings)
    log("buildings %d, water polys %d, canals %d, coast lines %d, parks %d, industrial %d"
        % (len(buildings), len(water_polys), len(canal_lines), len(coast_lines), len(parks), len(industrial)))

    grid = Grid()
    water = build_water(grid, coast_lines, water_polys, canal_lines, roads)
    rects = water_rects(grid, water)
    water_out = []
    for x0, z0, x1, z1 in rects:
        for cx, cz, ax, az, bx, bz in split_chunks(x0, z0, x1, z1):
            water_out.append({"cx": cx, "cz": cz, "x0": round(ax, 2), "z0": round(az, 2),
                              "x1": round(bx, 2), "z1": round(bz, 2)})
    log("water rectangles: %d (after chunk split)" % len(water_out))

    dist_at = expressway_distance(roads)
    streets = plan_streets(street_ways, roads, grid, water)
    seg_list, seg_cells = [], {}
    for pc in streets:
        cl = pc["w"] / 2 + pc["sw"] + 0.3
        for a, b in zip(pc["pts"], pc["pts"][1:]):
            seg_list.append((a[0], a[1], b[0], b[1], cl))
    add_seg_cells(seg_list, seg_cells)
    log("street pieces %d, segments %d" % (len(streets), len(seg_list)))

    # Buildings: keep centroid in bbox, drop road-corridor and water conflicts.
    kept = []
    dropped = defaultdict(int)
    for oid, tags, ring in buildings:
        ring = clean_ring(ring)
        if len(ring) < 3:
            dropped["degenerate"] += 1
            continue
        if abs(area2(ring)) < 8.0:
            dropped["tiny"] += 1
            continue
        cx = sum(p[0] for p in ring) / len(ring)
        cz = sum(p[1] for p in ring) / len(ring)
        if not in_bbox(cx, cz):
            dropped["outside"] += 1
            continue
        if grid.idx(cx, cz) >= 0 and water[grid.idx(cx, cz)]:
            dropped["water"] += 1
            continue
        if any(water[grid.idx(p[0], p[1])] for p in ring if grid.idx(p[0], p[1]) >= 0):
            dropped["water_edge"] += 1
            continue
        if roads.footprint_conflict(ring):
            dropped["road_band"] += 1
            continue
        if any(seg_street_hit(p[0], p[1], seg_list, seg_cells) for p in ring):
            dropped["street"] += 1
            continue
        h_tag = parse_height(tags)
        fam = family_of(tags, h_tag, cz)
        near_water = False
        for dx, dz in ((60, 0), (-60, 0), (0, 60), (0, -60)):
            gi = grid.idx(cx + dx, cz + dz)
            if gi >= 0 and water[gi]:
                near_water = True
        zone = zone_of((cx, cz), near_water)
        area = abs(area2(ring)) / 2.0
        rng_b = random.Random(oid)
        untagged = tags.get("building", "yes") in ("yes", "")
        if untagged and h_tag is None:
            if zone == "waterfront":
                fam = "industrial"
            elif zone in ("ginza", "shiodome"):
                fam = "commercial"
            elif zone == "daiba":
                fam = "commercial" if area > 400 else "apartment"
            else:
                fam = "apartment" if area < 350 else "commercial"
        if h_tag is None and zone == "waterfront" and fam == "industrial":
            h = rng_b.uniform(8.0, 13.0) if area > 500 else rng_b.uniform(5.0, 9.0)
        elif h_tag is None and zone in ZONE_STOREYS:
            lo, hi = ZONE_STOREYS[zone]
            h = rng_b.randint(lo, hi) * STOREY
        else:
            h = h_tag if h_tag is not None else default_height(fam)
        h = max(3.0, min(h, 300.0))
        flags = {
            "sign": zone == "ginza" and fam == "commercial" and rng_b.random() < 0.5,
            "tank": h >= 12.0 and rng_b.random() < 0.35,
            "stair": h >= 18.0 and rng_b.random() < 0.25,
            "ac": rng_b.randint(0, 4),
            "setback": h >= 60.0,
            "corrugated": fam == "industrial" and zone == "waterfront",
        }
        style = pick_style(zone, fam, h, rng_b)
        style_tint = rng_b.randrange(3)
        style_off = [rng_b.randrange(16), rng_b.randrange(12)]
        kept.append({"osm": oid, "ring": ring, "h": h, "h_tag": h_tag, "family": fam,
                     "style": style, "tint": style_tint, "off": style_off,
                     "tags": tags.get("building", "yes"), "c": (cx, cz), "zone": zone, "flags": flags,
                     "near_m": round(dist_at(cx, cz), 1)})
    log("buildings kept %d, dropped %s" % (len(kept), dict(dropped)))

    # Landmarks: tallest footprints get a CC-BY kit building (asian_themed) fitted to the footprint.
    kit = json.loads(KIT_INDEX.read_text(encoding="utf-8"))
    kit_list = [b for b in kit["buildings"] if b["source"] == "asian_themed" and b.get("shippable")]
    landmarks = []
    dropped_landmark = defaultdict(int)
    cand = [b for b in kept if b["h"] >= 40.0 and abs(area2(b["ring"])) >= 180.0]
    cand.sort(key=lambda b: -b["h"])
    used = set()
    for b in cand:
        if len(landmarks) >= KIT_MAX:
            break
        o = obb(b["ring"])
        if o["w"] <= 0 or o["d"] <= 0:
            continue
        aspect = max(o["w"], o["d"]) / max(1e-6, min(o["w"], o["d"]))
        if aspect > 2.5 or min(o["w"], o["d"]) < 8.0:
            continue
        if roads.footprint_conflict(rect_ring(o)):
            dropped_landmark["road_band"] += 1
            continue
        near_shio = math.dist(b["c"], SHIODOME_C) < SHIODOME_R
        h = min(b["h"], SHIODOME_CAP if near_shio else LANDMARK_CAP)
        pool = [k for k in kit_list if k["name"] not in used] or kit_list
        best = min(pool, key=lambda k: abs(k["height_m"] - h))
        # True scale only: one uniform factor from the footprint fit. Reject if the footprint
        # ratios disagree (the kit would be stretched) or the resulting height is too short.
        kx, ky = best["footprint_m"]
        r = [o["w"] / max(kx, 1e-3), o["d"] / max(ky, 1e-3)]
        if max(r) / max(min(r), 1e-3) > 1.35:
            dropped_landmark["aspect_mismatch"] += 1
            continue
        scale = min(r)
        h = best["height_m"] * scale
        if h < 0.6 * b["h"] or scale < 0.3:
            dropped_landmark["height_or_scale"] += 1
            continue
        used.add(best["name"])
        landmarks.append({"kit": best["name"], "source": best["source"], "license": best["license"],
                          "author": best["author"], "kit_h": best["height_m"],
                          "kit_footprint": best["footprint_m"], "osm": b["osm"], "obb": o,
                          "h": round(h, 2), "h_osm": round(b["h"], 2), "scale": round(scale, 5),
                          "c": [round(b["c"][0], 2), round(b["c"][1], 2)]})
        b["landmark"] = True
    kept = [b for b in kept if not b.get("landmark")]
    log("landmarks %d, candidates dropped %s" % (len(landmarks), dict(dropped_landmark)))

    # Level-of-detail: full textured facades for the nearest buildings within budget.
    base = 0
    for b in kept:
        b["lod"] = "far"
        base += tri_counts(b["ring"], b["h"], b["family"], "plain")
    base += 2 * len(water_out) + 3000 * 4  # water + trees (estimate)
    base += sum(4 * 12 * 2 for _ in landmarks)  # landmarks estimate
    spare = RENDER_BUDGET - base
    full_n = 0
    for b in sorted(kept, key=lambda b: b["near_m"]):
        if b["near_m"] > NEAR_M:
            break
        extra = tri_counts(b["ring"], b["h"], b["family"], "full") - tri_counts(b["ring"], b["h"], b["family"], "plain")
        if extra <= spare:
            b["lod"] = "full"
            spare -= extra
            full_n += 1
    log("LOD full %d of %d (spare budget left %d)" % (full_n, len(kept), spare))

    # Containers: stacks on industrial landuse (sparse, away from roads, water and buildings).
    containers = []
    for ring in industrial:
        ring = clean_ring(ring)
        if len(ring) < 3 or abs(area2(ring)) < 2500.0:
            continue
        xs = [p[0] for p in ring]
        zs = [p[1] for p in ring]
        o = obb(ring)
        yaw_u = o["u"]
        ux_c, uz_c = yaw_u
        x, z = min(xs), min(zs)
        while x < max(xs):
            zz = z
            while zz < max(zs):
                gi = grid.idx(x, zz)
                if (random.random() < 0.12 and gi >= 0 and point_in_ring(x, zz, ring)
                        and not water[gi]
                        and not roads.footprint_conflict(rect_ring({
                            "c": [x + ux_c * 6.095, zz + uz_c * 6.095], "u": yaw_u, "w": 12.19, "d": 2.44}))):
                    containers.append({"pos": [round(x, 2), round(zz, 2)], "u": yaw_u,
                                       "stack": 2 if random.random() < 0.3 else 1})
                zz += 26.0 + random.random() * 6.0
            x += 26.0 + random.random() * 6.0
    log("containers %d" % len(containers))

    # Trees: sparse cards in parks.
    trees = []
    for ring in parks:
        ring = clean_ring(ring)
        if len(ring) < 3:
            continue
        xs = [p[0] for p in ring]
        zs = [p[1] for p in ring]
        x = min(xs)
        while x < max(xs):
            z = min(zs)
            while z < max(zs):
                x_j = x + random.uniform(-3, 3)
                z_j = z + random.uniform(-3, 3)
                gi = grid.idx(x_j, z_j)
                if (random.random() < 0.22 and gi >= 0 and point_in_ring(x_j, z_j, ring)
                        and not water[gi]):
                    if not roads.near(x_j, z_j, 3.5):
                        trees.append({"pos": [round(x_j, 2), round(z_j, 2)],
                                      "size": round(3.5 + random.random() * 2.5, 2),
                                      "cell": random.choice(["shrub", "hedge", "grass", "weed"]),
                                      "yaw": round(random.uniform(0, 180), 1)})
                z += 12.0
            x += 12.0
    log("trees %d" % len(trees))

    # Plan output.
    street_trees, street_lamps = [], []
    for pc in streets:
        if pc["cls"] == "residential":
            continue
        sw_off = pc["w"] / 2 + pc["sw"] * 0.5
        acc_t, acc_l, side = 0.0, 0.0, 1
        for a, b in zip(pc["pts"], pc["pts"][1:]):
            L = math.dist(a, b)
            if L < 1e-6:
                continue
            ux, uz = (b[0] - a[0]) / L, (b[1] - a[1]) / L
            nx, nz = -uz, ux
            t = 0.0
            while t < L:
                x = a[0] + ux * t
                z = a[1] + uz * t
                if acc_t <= 0.0:
                    side = -side
                    px, pz = x + nx * sw_off * side, z + nz * sw_off * side
                    street_trees.append({"pos": [round(px, 2), round(pz, 2)],
                                         "size": round(4.0 + random.random() * 2.5, 2),
                                         "cell": random.choice(["shrub", "hedge", "grass"]),
                                         "yaw": round(random.uniform(0, 180), 1), "street": True})
                    acc_t = TREE_STEP
                if acc_l <= 0.0 and pc["cls"] in ("primary", "secondary"):
                    lx = x + nx * (pc["w"] / 2 + 0.6)
                    lz = z + nz * (pc["w"] / 2 + 0.6)
                    street_lamps.append({"pos": [round(lx, 2), round(lz, 2)],
                                         "u": [round(ux, 4), round(uz, 4)]})
                    acc_l = STREET_LAMP_STEP
                step = min(acc_t, acc_l if acc_l > 0 else acc_t, L - t)
                step = max(step, 0.5)
                t += step
                acc_t -= step
                acc_l -= step
    log("street trees %d, street lamps %d" % (len(street_trees), len(street_lamps)))
    trees = trees + street_trees

    out_b = []
    for b in kept:
        cx, cz = chunk_of(b["c"][0], b["c"][1])
        ring = [[round(p[0], 2), round(p[1], 2)] for p in b["ring"]]
        out_b.append({"id": "osm_%d" % b["osm"], "osm": b["osm"], "cx": cx, "cz": cz,
                      "ring": ring, "h": round(b["h"], 2), "h_tag": b["h_tag"],
                      "family": b["family"], "lod": b["lod"], "near_m": b["near_m"],
                      "zone": b["zone"], "flags": b["flags"],
                      "style": b["style"], "tint": b["tint"], "off": b["off"],
                      "obb": obb(b["ring"]), "pick": random.randrange(4),
                      "lit_seed": random.randrange(1 << 30)})
    out_k = []
    for i, k in enumerate(landmarks):
        cx, cz = chunk_of(k["c"][0], k["c"][1])
        out_k.append(dict(k, id="kit_%03d" % (i + 1), cx=cx, cz=cz))
    out_c = []
    for i, c in enumerate(containers):
        cx, cz = chunk_of(c["pos"][0], c["pos"][1])
        out_c.append(dict(c, id="ctr_%05d" % i, cx=cx, cz=cz))
    out_t = []
    for t in trees:
        cx, cz = chunk_of(t["pos"][0], t["pos"][1])
        out_t.append(dict(t, cx=cx, cz=cz))
    plan = {
        "format": "nox-shuto-scenery-plan",
        "version": 1,
        "bbox": list(BBOX),
        "chunk_m": CHUNK,
        "ground_y": GROUND_Y,
        "water_y": WATER_Y,
        "storey_m": STOREY,
        "sources": {
            "osm": {"license": "ODbL-1.0", "attribution": "(c) OpenStreetMap contributors",
                    "extract": "Overpass API, bbox %s" % (latlon_bbox(),), "raw": "data/shuto/raw/overpass_scenery_*.json"},
        },
        "counts": {"buildings": len(out_b), "full": full_n, "landmarks": len(out_k),
                   "containers": len(out_c), "trees": len(out_t), "water_rects": len(water_out),
                   "streets": len(streets), "street_lamps": len(street_lamps)},
        "dropped": dict(dropped),
        "water_rects": water_out,
        "buildings": out_b,
        "landmarks": out_k,
        "containers": out_c,
        "trees": out_t,
        "streets": streets,
        "street_lamps": street_lamps,
    }
    PLAN.parent.mkdir(parents=True, exist_ok=True)
    PLAN.write_text(json.dumps(plan, separators=(",", ":")), encoding="utf-8")
    log("wrote %s in %.1f s" % (PLAN, time.time() - t0))


if __name__ == "__main__":
    main()
