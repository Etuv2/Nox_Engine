"""Shuto world-map background bake (stylised satellite base, no roads).

Produces:
  textures/shuto/map/worldmap_base.png    RGB base map: dark land, faint district texture and
                                          building footprints (rep section only), darker teal
                                          Tokyo Bay / rivers, shoreline highlight, light grid and vignette.
  textures/shuto/map/worldmap_base.json   image size and world bounds (NOX frame, metres) used by
                                          the game's world map (WorldMapUI).
  data/shuto/map_labels.json              district labels with positions derived from the
                                          authoritative facility names in data/shuto/network.json
                                          (run with --labels; labels with no matching data are dropped).

Roads are never baked: the game draws carriageways live (sharp at any zoom).

Data: OpenStreetMap (c) OpenStreetMap contributors, ODbL 1.0. Water/coastline tiles are fetched
from Overpass for the whole network bbox and cached under data/shuto/raw/overpass_worldmap_*.json;
building footprints come from the existing rep-section cache (overpass_scenery_buildings_*.json).

Frame: x east, z south, metres, origin Edobashi JCT (same as build_network.py). Image row 0 is the
northern edge (minimum z), so the map is north-up. Coastline ways have sea on their right-hand side
(OSM convention); each sea/land component is classified by a vote from those sides.

Run (stdlib + numpy + Pillow + scipy):
  python -I tools/shuto/make_worldmap.py              (fetch missing tiles, bake texture)
  python -I tools/shuto/make_worldmap.py --labels     (also rewrite data/shuto/map_labels.json)
  python -I tools/shuto/make_worldmap.py --refresh    (re-fetch Overpass tiles)
"""
import argparse
import json
import math
import sys
import time
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RAW = ROOT / "data" / "shuto" / "raw"
NET = ROOT / "data" / "shuto" / "network.json"
OUT_DIR = ROOT / "textures" / "shuto" / "map"
OUT_PNG = OUT_DIR / "worldmap_base.png"
OUT_JSON = OUT_DIR / "worldmap_base.json"
LABELS = ROOT / "data" / "shuto" / "map_labels.json"

UA = "NoxShutoWorldMap/1.0 (Nox_Engine Shuto world map bake)"
MIRRORS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
]

ORIGIN_LAT, ORIGIN_LON = 35.6843, 139.7745
PAD = 800.0          # metres of margin around the network extent
PX = 20.0            # metres per texel
TILES = (7, 5)       # Overpass tiles (lat rows, lon columns) over the bbox; small boxes keep Overpass happy
RAD_PER_M = 1.0 / 111200.0

# Colours (sRGB 0..255), tuned for the dark-night look.
LAND = np.array([22, 26, 32], np.float32)
URBAN = np.array([34, 39, 47], np.float32)
BUILDING = np.array([40, 45, 52], np.float32)
WATER_DEEP = np.array([5, 34, 40], np.float32)
WATER_NEAR = np.array([10, 56, 62], np.float32)
SHORE = np.array([74, 128, 132], np.float32)

# ------------------------------------------------------------------ projection (as build_network.py)

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
    """NOX frame (x east, z south, metres)."""
    e_, n_ = _tm_raw(lat, lon)
    return e_ - _E0, -(n_ - _N0)


def unproject_approx(x, z):
    """Good to a few metres over the network extent (only used to pick Overpass boxes)."""
    lat = ORIGIN_LAT - z * RAD_PER_M
    lon = ORIGIN_LON + x / (111320.0 * math.cos(math.radians(lat)))
    return lat, lon


# ------------------------------------------------------------------ bounds

def network_extent():
    net = json.loads(NET.read_text(encoding="utf-8"))
    xs, zs = [], []
    for cw in net["carriageways"]:
        for p in cw["points"]:
            xs.append(p[0])
            zs.append(p[2])
    return min(xs), min(zs), max(xs), max(zs)


def map_bounds():
    x0, z0, x1, z1 = network_extent()
    # Snap outwards to whole 500 m so the bounds are stable across rebuilds.
    snap = lambda v, f: math.floor(v / 500.0) * 500.0 if f < 0 else math.ceil(v / 500.0) * 500.0
    return (snap(x0 - PAD, -1), snap(z0 - PAD, -1), snap(x1 + PAD, 1), snap(z1 + PAD, 1))


# ------------------------------------------------------------------ Overpass

def log(msg):
    print(msg, flush=True)


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
                return body
            except Exception as ex:  # next mirror / attempt
                last = ex
                log("  %s failed: %s" % (url, ex))
        time.sleep(8 * (attempt + 1))
    raise SystemExit("Overpass fetch failed: %s" % last)


# Coastline only over the whole bbox (small, fast). Rivers, lakes and riverbanks come from the
# rep-section cache (overpass_scenery_water_*), which is the only area with them fetched.
COAST_QUERY = ['way["natural"="coastline"]{b};']
# Rivers and canals as centrelines (whole bbox): drawn as water strips, widths from the class.
RIVER_QUERY = ['way["waterway"~"^(river|canal)$"]{b};']
QUERIES = [("coast", COAST_QUERY), ("river", RIVER_QUERY)]


def fetch_water_tiles(refresh):
    b = map_bounds()
    x0, z0, x1, z1 = b
    lat_top, lon_left = unproject_approx(x0, z0)
    lat_bot, lon_right = unproject_approx(x1, z1)
    RAW.mkdir(parents=True, exist_ok=True)
    elements = {}
    rows, cols = TILES
    for i in range(rows):
        for j in range(cols):
            s = lat_bot + (lat_top - lat_bot) * i / rows
            n = lat_bot + (lat_top - lat_bot) * (i + 1) / rows
            w = lon_left + (lon_right - lon_left) * j / cols
            e = lon_left + (lon_right - lon_left) * (j + 1) / cols
            # Small overlap so ways crossing tile edges are complete in at least one tile.
            box = (s - 0.002, w - 0.002, n + 0.002, e + 0.002)
            for kind, qs in QUERIES:
                path = RAW / ("overpass_worldmap_%s_%dx%d_r%dc%d.json" % (kind, rows, cols, i, j))
                if path.exists() and not refresh:
                    body = path.read_text(encoding="utf-8")
                    log("cache: %s" % path.name)
                else:
                    query = ("[out:json][timeout:600][maxsize:536870912];\n(\n" +
                             "\n".join(q.format(b="(%.5f,%.5f,%.5f,%.5f)" % box) for q in qs) +
                             "\n);\nout body geom;\n")
                    time.sleep(6)  # Overpass slot limit: keep requests spaced out
                    body = post_overpass(query)
                    path.write_text(body, encoding="utf-8")
                    log("fetched %s" % path.name)
                for el in json.loads(body)["elements"]:
                    elements[(el["type"], el["id"])] = el
    return list(elements.values())


def load_rep_water():
    """Rep-section water cache (coastline, riverbanks, natural=water, rivers and canals)."""
    out = []
    for path in sorted(RAW.glob("overpass_scenery_water_t*.json")):
        out.extend(json.loads(path.read_text(encoding="utf-8")).get("elements", []))
    return out


def load_rep_buildings():
    out = []
    for path in sorted(RAW.glob("overpass_scenery_buildings_t*.json")):
        for el in json.loads(path.read_text(encoding="utf-8")).get("elements", []):
            if el.get("type") == "way" and "geometry" in el:
                out.append([project(p["lat"], p["lon"]) for p in el["geometry"]])
    return out


# ------------------------------------------------------------------ rasterisation

def to_px(pts, b, w, h):
    x0, z0, _, _ = b
    return [((x - x0) / PX, (z - z0) / PX) for x, z in pts]


def bake(refresh):
    b = map_bounds()
    x0, z0, x1, z1 = b
    W = int(math.ceil((x1 - x0) / PX))
    H = int(math.ceil((z1 - z0) / PX))
    log("bounds x[%.0f,%.0f] z[%.0f,%.0f] -> %dx%d px at %.0f m" % (x0, x1, z0, z1, W, H, PX))

    elements = {(el["type"], el["id"]): el for el in fetch_water_tiles(refresh)}
    for el in load_rep_water():
        elements.setdefault((el["type"], el["id"]), el)
    elements = list(elements.values())
    ways, rels = [], []
    for el in elements:
        if el["type"] == "way" and "geometry" in el:
            ways.append(el)
        elif el["type"] == "relation":
            rels.append(el)

    # Projected lines/rings in NOX metres (for coastline votes and polygons).
    def proj_geom(geom):
        return [project(p["lat"], p["lon"]) for p in geom]

    coast = []   # coastline polylines (sea on the right)
    water_rings = []  # (ring, is_hole) for filled water areas
    rivers = []  # waterway centrelines
    for el in ways:
        tags = el.get("tags", {})
        pts = proj_geom(el["geometry"])
        if tags.get("natural") == "coastline":
            coast.append(pts)
        elif tags.get("natural") == "water" or tags.get("waterway") == "riverbank":
            if len(pts) >= 3:
                water_rings.append((pts, False))
        elif tags.get("waterway") in ("river", "canal"):
            rivers.append((pts, 30.0 if tags.get("waterway") == "river" else 18.0))
    for el in rels:
        tags = el.get("tags", {})
        if tags.get("natural") != "water" and tags.get("waterway") != "riverbank":
            continue
        for m in el.get("members", []):
            if m.get("type") != "way" or "geometry" not in m:
                continue
            pts = proj_geom(m["geometry"])
            if m.get("role") == "inner":
                water_rings.append((pts, True))
            elif m.get("role") in ("outer", ""):
                water_rings.append((pts, False))
    log("water: %d coastline ways, %d water rings, %d rivers" % (len(coast), len(water_rings), len(rivers)))

    # Coastline edge raster and sea/land components.
    edge_img = Image.new("L", (W, H), 0)
    ed = ImageDraw.Draw(edge_img)
    for pts in coast:
        ed.line(to_px(pts, b, W, H), fill=255, width=3)
    edge = np.array(edge_img) > 0
    labels, n = ndimage.label(~edge)
    sea_votes = np.zeros(n + 1, np.int64)
    land_votes = np.zeros(n + 1, np.int64)
    for pts in coast:
        for (ax, az), (bx, bz) in zip(pts[:-1], pts[1:]):
            dx, dz = bx - ax, bz - az
            L = math.hypot(dx, dz)
            if L < 1e-3:
                continue
            # Right-hand normal in NOX (x east, z south) for a traveller along (dx, dz).
            rx, rz = -dz / L, dx / L
            mx, mz = (ax + bx) * 0.5, (az + bz) * 0.5
            for sign, votes in ((1.0, sea_votes), (-1.0, land_votes)):
                px_ = int((mx + sign * rx * 2.0 * PX - x0) / PX)
                pz_ = int((mz + sign * rz * 2.0 * PX - z0) / PX)
                if 0 <= px_ < W and 0 <= pz_ < H and labels[pz_, px_] > 0:
                    votes[labels[pz_, px_]] += 1
    is_sea = np.zeros(n + 1, bool)
    is_sea[1:] = sea_votes[1:] > land_votes[1:]
    sea = is_sea[labels]
    log("sea components: %d of %d" % (int(is_sea.sum()), n))

    # Inland water areas and rivers.
    inland = Image.new("L", (W, H), 0)
    idraw = ImageDraw.Draw(inland)
    for pts, hole in water_rings:
        if len(pts) >= 3:
            idraw.polygon(to_px(pts, b, W, H), fill=0 if hole else 255)
    for pts, width in rivers:
        if len(pts) >= 2:
            idraw.line(to_px(pts, b, W, H), fill=255, width=max(2, int(width / PX)))
    inland_mask = np.array(inland) > 0
    water = sea | inland_mask

    # Land shading: slow district variation, fine grain, faint building footprints (rep section).
    rng = np.random.default_rng(20261010)
    coarse = ndimage.gaussian_filter(rng.standard_normal((H, W)).astype(np.float32), 9.0)
    coarse /= max(1e-6, float(np.abs(coarse).max()))
    fine = ndimage.gaussian_filter(rng.standard_normal((H, W)).astype(np.float32), 0.8)
    fine /= max(1e-6, float(np.abs(fine).max()))
    # Urban density proxy: nearer the bay and the centre reads denser.
    dist = ndimage.distance_transform_edt(~water).astype(np.float32) * PX
    urban = np.clip(1.0 - dist / 9000.0, 0.0, 1.0)
    urban = urban * (0.65 + 0.35 * (coarse * 0.5 + 0.5))
    img = np.zeros((H, W, 3), np.float32)
    land_col = LAND[None, None, :] + (URBAN - LAND)[None, None, :] * urban[..., None]
    land_col += (fine * 2.2)[..., None] + (coarse * 1.5)[..., None]
    # District texture everywhere on land: ~110 m city blocks with per-block tone, darker street
    # gaps, stronger where the urban proxy is high. Procedural, so it covers the whole bbox.
    blk = 110.0 / PX
    nby, nbx = int(np.ceil(H / blk)) + 2, int(np.ceil(W / blk)) + 2
    tone = rng.random((nby, nbx)).astype(np.float32) - 0.5
    bi = np.minimum((np.arange(H) / blk).astype(int), nby - 1)
    bj = np.minimum((np.arange(W) / blk).astype(int), nbx - 1)
    block = tone[np.ix_(bi, bj)]
    fy = (np.arange(H) / blk) % 1.0
    fx = (np.arange(W) / blk) % 1.0
    street = ((fy < 0.07)[:, None] | (fx < 0.07)[None, :]).astype(np.float32)
    amp = (0.25 + 0.75 * urban)
    land_col += (block * 9.0 * amp)[..., None] - (street * 4.0 * amp)[..., None]
    depth = ndimage.distance_transform_edt(water).astype(np.float32) * PX
    depth01 = np.clip(depth / 2500.0, 0.0, 1.0)
    water_col = WATER_NEAR[None, None, :] + (WATER_DEEP - WATER_NEAR)[None, None, :] * depth01[..., None]
    water_col += (fine * 1.2)[..., None]
    img[:] = np.where(water[..., None], water_col, land_col)

    # Building footprints (only where the rep-section OSM cache exists).
    bld = Image.new("L", (W, H), 0)
    bd = ImageDraw.Draw(bld)
    for ring in load_rep_buildings():
        if len(ring) >= 3:
            bd.polygon(to_px(ring, b, W, H), fill=255)
    bmask = np.array(bld) > 0
    img[bmask & ~water] = img[bmask & ~water] * 0.4 + BUILDING * 0.6

    # Shoreline highlight, light grid, vignette.
    shore = ndimage.binary_dilation(edge, iterations=1) & ~ndimage.binary_erosion(water, iterations=1) & ~water
    shore2 = ndimage.binary_dilation(water, iterations=2) & ~water
    img[shore2] = img[shore2] * 0.55 + SHORE * 0.45
    grid_px = max(1, int(round(1000.0 / PX)))
    gx = (np.arange(W) % grid_px == 0)
    gz = (np.arange(H) % grid_px == 0)
    gridmask = gz[:, None] | gx[None, :]
    img[gridmask] = img[gridmask] * 0.9 + 255 * 0.1 * 0.25
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    r = np.sqrt(((xx / W) - 0.5) ** 2 * 1.2 + ((yy / H) - 0.5) ** 2) * 1.6
    img *= np.clip(1.0 - 0.45 * r * r, 0.4, 1.0)[..., None]

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").save(OUT_PNG, optimize=True)
    # Tokyo Bay label anchor: centroid of open sea (>1.2 km from any land) inside the bay box.
    bx0, bz0, bx1, bz1 = 1000.0, 6000.0, 9000.0, 16000.0
    ix0, iz0 = max(0, int((bx0 - x0) / PX)), max(0, int((bz0 - z0) / PX))
    ix1, iz1 = min(W, int((bx1 - x0) / PX)), min(H, int((bz1 - z0) / PX))
    to_land = ndimage.distance_transform_edt(sea)[iz0:iz1, ix0:ix1] * PX
    py, px = np.nonzero(to_land > 1200.0)
    if len(px):
        bay_pos = [round(x0 + (ix0 + px.mean() + 0.5) * PX, 1), round(z0 + (iz0 + py.mean() + 0.5) * PX, 1)]
    else:
        bay_pos = None
    meta = {
        "image": "textures/shuto/map/worldmap_base.png",
        "tokyo_bay": bay_pos,
        "size": [W, H],
        "bounds": [round(x0, 1), round(z0, 1), round(x1, 1), round(z1, 1)],
        "metres_per_texel": PX,
        "frame": "NOX: x east, z south, metres; row 0 = north (min z)",
        "attribution": "Map data (c) OpenStreetMap contributors (ODbL)",
    }
    OUT_JSON.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    log("wrote %s (%dx%d) and %s" % (OUT_PNG.name, W, H, OUT_JSON.name))


# ------------------------------------------------------------------ labels

LABEL_SPECS = [
    # text, kind, facility matchers (name_en substring or name_ja substring)
    ("GINZA", "district", ["Ginza"], ["銀座"]),
    ("SHIBAURA", "district", [], ["芝浦"]),
    ("DAIBA", "district", [], ["台場"]),
    ("SHINJUKU", "district", ["Shinjuku"], []),
    ("SHIBUYA", "district", ["Shibuya"], []),
    ("IKEBUKURO", "district", ["Higashi ikebukuro"], []),
    ("HANEDA", "district", ["Haneda Airport"], []),
    ("KASUMIGASEKI", "district", ["Kasumigaseki"], ["霞が関"]),
    ("YOKOHAMA", "district", ["Yokohama West"], []),
]


def write_labels():
    net = json.loads(NET.read_text(encoding="utf-8"))
    facilities = net.get("facilities", [])
    out = []
    for text, kind, en, ja in LABEL_SPECS:
        hits = [f for f in facilities
                if any(s.lower() in (f.get("name_en") or "").lower() for s in en)
                or any(s in (f.get("name_ja") or "") for s in ja)]
        if not hits:
            log("label %s: no facility match, skipped" % text)
            continue
        xs = [f["pos"][0] for f in hits]
        zs = [f["pos"][2] for f in hits]
        pos = [round(sum(xs) / len(xs), 1), round(sum(zs) / len(zs), 1)]
        out.append({"text": text, "kind": kind, "pos": pos, "source": "%d facilities" % len(hits)})
    # Water label: midpoint of the Shibaura parking area and the Daiba interchange.
    # Tokyo Bay: pole of inaccessibility of the sea mask, written by the bake (worldmap_base.json).
    if OUT_JSON.exists():
        bay = json.loads(OUT_JSON.read_text(encoding="utf-8")).get("tokyo_bay")
        if bay:
            out.append({"text": "TOKYO BAY", "kind": "water", "pos": bay,
                        "source": "pole of inaccessibility of the bay sea mask (worldmap_base.json)"})
    LABELS.write_text(json.dumps({"format": "nox-shuto-map-labels", "version": 1, "labels": out},
                                 indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    log("wrote %s (%d labels)" % (LABELS, len(out)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true", help="re-fetch Overpass tiles")
    ap.add_argument("--labels", action="store_true", help="rewrite data/shuto/map_labels.json")
    ap.add_argument("--labels-only", action="store_true")
    args = ap.parse_args()
    if args.labels or args.labels_only:
        write_labels()
    if not args.labels_only:
        bake(args.refresh)


if __name__ == "__main__":
    sys.exit(main())
