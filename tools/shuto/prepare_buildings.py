"""Prepare deterministic architecture textures and geographic placement records."""
import hashlib
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import scenery_plan as geo


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def main():
    out = ROOT / "models/shuto/buildings"
    tex = out / "textures"
    tex.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((ROOT / "textures/shuto/manifest.json").read_text(encoding="utf-8"))
    assets = {a["id"]: a for a in manifest["assets"]}
    records = []
    for name in ["concrete_cast", "concrete_weathered", "corrugated_metal", "galvanized_steel", "rooftop_membrane"]:
        a = assets[name]
        original = Image.open(ROOT / "textures/shuto" / a["file"]).convert("RGB")
        # Mirror four quadrants: exact edge continuity, no claim of measured PBR.
        patch = original.resize((256, 256), Image.Resampling.LANCZOS)
        tile = Image.new("RGB", (512, 512))
        tile.paste(patch, (0, 0))
        tile.paste(patch.transpose(Image.Transpose.FLIP_LEFT_RIGHT), (256, 0))
        tile.paste(tile.crop((0, 0, 512, 256)).transpose(Image.Transpose.FLIP_TOP_BOTTOM), (0, 256))
        tile.save(tex / (name + ".png"))
        pix = np.asarray(tile).astype(float)
        records.append({"file": name + ".png", "source": a["file"], "source_sha256": a["sha256"],
                        "processing": "mirror tile, 512 px; inspect mirrored repetition in close views",
                        "edge_mae": [float(np.abs(pix[:, 0] - pix[:, -1]).mean()),
                                     float(np.abs(pix[0] - pix[-1]).mean())],
                        "roughness": a["roughness"], "metallic": a["metallic"]})
    for family in ["apartment", "commercial", "industrial"]:
        a = assets["facades_" + family]
        original = Image.open(ROOT / "textures/shuto" / a["file"]).convert("RGB")
        for i, cell in enumerate(a["cells"]):
            rect = cell["pixel_rect"]
            crop = original.crop(tuple(rect)).resize((496, 496), Image.Resampling.LANCZOS)
            padded = np.pad(np.asarray(crop), ((8, 8), (8, 8), (0, 0)), mode="edge")
            filename = "patch_%s_%d.png" % (family, i)
            Image.fromarray(padded).save(tex / filename)
            records.append({"file": filename, "source": a["file"], "source_rect": rect,
                            "uv_blender_bottom_left": [8 / 512, 8 / 512, 504 / 512, 504 / 512],
                            "use": "single middle/far facade patch; never repeat whole source atlas"})
    fonts = ROOT / "fonts/noto"
    japanese = ImageFont.truetype(str(fonts / "NotoSansJP-Bold.otf"), 92)
    latin = ImageFont.truetype(str(ROOT / "fonts/BarlowCondensed-SemiBold.ttf"), 52)
    signs = [("minato_coffee", "\u55ab\u8336 \u6e2f", "MINATO COFFEE"),
             ("aoba_store", "\u9752\u8449\u5546\u5e97", "AOBA STORE"),
             ("shibaura_storage", "\u829d\u6d66\u5009\u5eab", "SHIBAURA STORAGE")]
    art = assets["billboards_art"]
    art_img = Image.open(ROOT / "textures/shuto" / art["file"]).convert("RGB")
    blanks = assets["sign_panel_blanks"]
    blank_img = Image.open(ROOT / "textures/shuto" / blanks["file"]).convert("RGB")
    for i, (name, jp, en) in enumerate(signs):
        image = blank_img.crop(tuple(blanks["cells"][1]["pixel_rect"])).resize((1024, 256))
        draw = ImageDraw.Draw(image)
        draw.rectangle((10, 10, 1013, 245), fill=(22, 33, 35), outline=(142, 138, 119), width=6)
        image.paste(art_img.crop(tuple(art["cells"][i]["pixel_rect"])).resize((210, 210)), (23, 23))
        draw = ImageDraw.Draw(image)
        draw.text((270, 26), jp, font=japanese, fill=(224, 219, 190))
        draw.text((276, 150), en, font=latin, fill=(175, 200, 192))
        image.save(tex / ("sign_" + name + ".png"))
        records.append({"file": "sign_" + name + ".png", "japanese": jp, "english": en,
                        "fonts": ["Noto Sans JP Bold", "Barlow Condensed SemiBold"], "font_license": "SIL-OFL-1.1",
                        "source": ["sign_panel_blanks.png", "billboards_art.png"], "fictional_scenery": True})
    atlas = Image.new("RGB", (1024, 768))
    for i, (name, _, _) in enumerate(signs):
        atlas.paste(Image.open(tex / ("sign_" + name + ".png")).convert("RGB"), (0, 256 * i))
    atlas.save(tex / "sign_atlas.png")
    records.append({"file": "sign_atlas.png", "rows_top_to_bottom": [s[0] for s in signs],
                    "use": "placed chunks: one sign material per chunk; same lettering as the single sign images"})
    write_json(ROOT / "data/shuto/buildings/material_sources.json", records)

    write_json(ROOT / "data/shuto/buildings/placements.json", plan_placements())


# ---------------------------------------------------------------- placement plan
# Ground section exported by the road chunks (ground_gravel at y 2.95). Outside it NOX shows the sky.
GROUND_BOX = (-2600.0, 1500.0, 0.0, 6000.0)
# Post-2002 completions recognised by name (reviewer knowledge; OSM has no start_date for them).
POST2002_NAMES = {
    "ザ・東京タワーズ": "The Tokyo Towers (2008)",
    "The Tokyo Towers": "The Tokyo Towers (2008)",
    "アクティ汐留": "Acty Shiodome (2004)",
    "ワールドタワーレジデンス": "World Tower Residence (2024)",
    "田町ステーションタワー": "msb Tamachi Station Tower (2018-2020)",
    "HARUMI FLAG": "Harumi Flag (2024)",
    "Kachidoki The Tower": "Kachidoki The Tower (2023)",
    "Air Tower": "Shibaura Island Air Tower (2007)",
    "キャピタルマークタワー": "Capital Mark Tower (2004)",
    "田町タワー": "Tamachi Tower (2018)",
    "クレアタワー": "Nippon Life Hamamatsucho Clair Tower (2018)",
    "BLUE FRONT": "Blue Front Shibaura (2025)",
    "パークコート浜離宮": "Park Court Hamarikyu The Tower (2016)",
    "アーバンネット内幸町": "Urbannet Uchisaiwaicho (2021)",
    "ベイクレストタワー": "Bay Crest Tower (2005)",
    "汐留ビルディング": "Shiodome Building / Tokyo Shiodome Building (2005-2007)",
    "住友不動産御成門タワー": "Sumitomo Fudosan Onarimon Tower (2013)",
    "プラウド": "Proud brand residence (brand launched 2003)",
    "東京虎ノ門グローバルスクエア": "Tokyo Toranomon Global Square (2020)",
    "グローバルフロントタワー": "Global Front Tower (2015)",
    "汐留メディアタワー": "Shiodome Media Tower and annex (2003)",
    "ブリリア": "Brillia brand residence (brand launched 2003)",
    "ザ・プリンス パークタワー東京": "The Prince Park Tower Tokyo (2005)",
    "汐留住友ビル": "Shiodome Sumitomo Building (2004)",
    "住友不動産汐留浜離宮ビル": "Sumitomo Real Estate Shiodome Hamarikyu Building (2008)",
    "新研究棟": "National Cancer Center new research building (post-2002)",
    "アロフト東京銀座": "Aloft Tokyo Ginza (2020)",
    "ACホテル": "AC Hotel Tokyo Ginza (2020)",
    "ホテル京阪 築地銀座": "Hotel Keihan Tsukiji Ginza Grande (2019)",
    "Mitsui Garden Hotel Shiodome": "Mitsui Garden Hotel Shiodome Italia-gai (2006)",
}
# Small untagged structures: OSM name parts / building tags that cannot be multi-storey blocks.
SMALL_NAMES = ("茶屋", "売札所", "棚", "便所", "トイレ", "休憩",
               "売店", "珈琲", "亭", "門", "鐘楼", "祠")
SMALL_TAGS = {"kiosk", "toilets", "gatehouse", "shed", "hut", "pavilion", "garage", "garages", "carport"}
TEMPLE_NAMES = ("殿", "堂", "寺", "院", "霊廟", "菩薩", "観音", "神社")
TEMPLE_TAGS = {"temple", "shrine", "church"}
# District palettes: coherent per district, varied between neighbours.
PALETTES = {
    ("ginza", "commercial"): ["office_ribbon", "spandrel_midrise", "tile_older", "dark_office", "ivory_modern"],
    ("ginza", "apartment"): ["tile_older", "apartment_concrete", "apartment_balcony"],
    # Curtain-wall towers in Shiodome are post-2002; the 2002 palette stays with older office styles.
    ("shiodome", "commercial"): ["office_ribbon", "ivory_modern", "spandrel_midrise", "dark_office"],
    ("shiodome", "apartment"): ["apartment_concrete", "ivory_modern", "apartment_balcony"],
    ("hamamatsucho", "apartment"): ["apartment_balcony", "apartment_concrete", "tile_older"],
    ("hamamatsucho", "commercial"): ["office_ribbon", "dark_office", "spandrel_midrise", "tile_older"],
    ("daiba", "commercial"): ["ivory_modern", "spandrel_midrise", "office_ribbon"],
    ("daiba", "apartment"): ["apartment_balcony", "ivory_modern", "apartment_concrete"],
}
INDUSTRIAL = ["warehouse_clerestory", "warehouse_roller"]
# Close concrete wall palette per facade style: indices of wall_0..3 in build_architecture.
WALLS = {"tile_older": [2, 3, 0], "apartment_concrete": [0, 1, 3], "apartment_balcony": [3, 0, 1],
         "ivory_modern": [3, 0], "office_ribbon": [1, 0, 3], "spandrel_midrise": [0, 3, 1],
         "dark_office": [1, 0], "curtain_mullion": [1, 3], "curtain_bronze": [2, 1],
         "warehouse_clerestory": [1, 2], "warehouse_roller": [2, 1]}
# Close facade layouts: bay width (m), panes per bay (1.6 x 2.0 m panes are fixed by the room contract).
LAYOUTS = {"tile_older": (3.0, 1), "apartment_concrete": (3.4, 1), "apartment_balcony": (3.2, 1),
           "ivory_modern": (4.0, 1), "office_ribbon": (3.8, 2), "spandrel_midrise": (4.4, 2),
           "dark_office": (3.7, 2), "curtain_mullion": (3.7, 2), "curtain_bronze": (3.7, 2),
           "warehouse_clerestory": (6.0, 2), "warehouse_roller": (6.0, 1)}
STOREYS = {"apartment": (3.0, 3.15, 3.3), "commercial": (3.6, 3.8, 4.0), "industrial": (5.0, 5.5)}
# Daiba waterfront context (all completed before 2003). Heights without OSM tags are estimates.
DAIBA = [
    {"osm": 328162159, "name": "Aqua City Odaiba", "opened": "2000-04", "h": 30.0, "style": "ivory_modern",
     "family": "commercial", "source": "OSM footprint; opened April 2000; height estimated (about 7 storeys)"},
    {"osm": 167055431, "name": "Decks Tokyo Beach", "opened": "1996-07-12", "h": 30.0, "style": "spandrel_midrise",
     "family": "commercial", "source": "OSM footprint and start_date; height estimated (about 7 storeys)"},
    {"osm": 5804890, "name": "Hotel Nikko Tokyo (Hilton Tokyo Odaiba since 2015)", "opened": "1996", "h": 60.0,
     "style": "ivory_modern", "family": "commercial",
     "source": "OSM multipolygon outer hull; opened 1996 as Hotel Nikko Tokyo; height estimated (16 storeys)"},
    {"osm": 149124428, "name": "Tradepia Odaiba", "opened": "1996", "h": 113.0, "style": "curtain_mullion",
     "family": "commercial", "source": "OSM footprint and height tag; 1996 completion from general knowledge"},
]


def ring_area(ring):
    return abs(sum(ring[i][0] * ring[i - 1][1] - ring[i - 1][0] * ring[i][1] for i in range(len(ring)))) / 2


def convex_hull(points):
    pts = sorted(set((round(p[0], 3), round(p[1], 3)) for p in points))
    if len(pts) < 3:
        return [list(p) for p in pts]

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lower, upper = [], []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return [list(p) for p in lower[:-1] + upper[:-1]]


def interior_samples(ring, limit=200):
    xs, zs = [p[0] for p in ring], [p[1] for p in ring]
    step = max(1.0, math.sqrt(max(ring_area(ring), 1.0) / limit))
    out = []
    x = min(xs) + step / 2
    while x < max(xs):
        z = min(zs) + step / 2
        while z < max(zs):
            if geo.point_in_ring(x, z, ring):
                out.append((x, z))
            z += step
        x += step
    return out


def rect(c, u, w, d):
    ux, uz = u
    return [[c[0] + ux * x + uz * z, c[1] + uz * x - ux * z]
            for x, z in [(-w / 2, -d / 2), (w / 2, -d / 2), (w / 2, d / 2), (-w / 2, d / 2)]]


def edges_of(ring):
    """Edges in build_scenery.wall_edges order (edges of 5 cm or less are skipped there)."""
    out = []
    for i in range(len(ring)):
        a, b = ring[i], ring[(i + 1) % len(ring)]
        if math.hypot(b[0] - a[0], b[1] - a[1]) > 0.05:
            out.append((a, b))
    return out


class Grid:
    def __init__(self, cell):
        self.cell = cell
        self.items = defaultdict(list)

    def keys(self, x0, z0, x1, z1):
        for i in range(math.floor(x0 / self.cell), math.floor(x1 / self.cell) + 1):
            for k in range(math.floor(z0 / self.cell), math.floor(z1 / self.cell) + 1):
                yield (i, k)

    def add_ring(self, ring, item):
        xs, zs = [p[0] for p in ring], [p[1] for p in ring]
        for key in self.keys(min(xs), min(zs), max(xs), max(zs)):
            self.items[key].append(item)

    def query(self, x0, z0, x1, z1):
        seen = {}
        for key in self.keys(x0, z0, x1, z1):
            for item in self.items.get(key, ()):
                seen[id(item)] = item
        return list(seen.values())


def road_samples(net, step=5.0):
    grid = Grid(60.0)
    for cw in net["carriageways"]:
        pts = cw["points"]
        for a, b in zip(pts[:-1], pts[1:]):
            n = max(1, int(math.dist((a[0], a[2]), (b[0], b[2])) / step))
            for k in range(n):
                t = k / n
                p = (a[0] + (b[0] - a[0]) * t, a[2] + (b[2] - a[2]) * t)
                grid.items[(math.floor(p[0] / 60), math.floor(p[1] / 60))].append(p)
    return grid


def road_edges(ring, samples, reach=140.0):
    """Indices of wall edges whose outward side faces a carriageway within `reach` metres."""
    area = sum(ring[i][0] * ring[(i + 1) % len(ring)][1] - ring[(i + 1) % len(ring)][0] * ring[i][1]
               for i in range(len(ring)))
    sign = 1 if area > 0 else -1
    out = []
    for index, (a, b) in enumerate(edges_of(ring)):
        length = math.dist(a, b)
        # Same outward normal convention as build_architecture.normal().
        n = ((b[1] - a[1]) / length * sign, -(b[0] - a[0]) / length * sign)
        m = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
        for p in samples.query(m[0] - reach, m[1] - reach, m[0] + reach, m[1] + reach):
            dx, dz = p[0] - m[0], p[1] - m[1]
            d = math.hypot(dx, dz)
            if 0.5 < d < reach and (dx * n[0] + dz * n[1]) / d > 0.3:
                out.append(index)
                break
    return out


def load_parks():
    grid = Grid(100.0)
    for path in sorted((ROOT / "data/shuto/raw").glob("overpass_scenery_landuse_*.json")):
        for e in json.loads(path.read_text(encoding="utf-8"))["elements"]:
            if e.get("tags", {}).get("leisure") not in ("park", "garden"):
                continue
            members = [e["geometry"]] if e.get("geometry") else [m["geometry"] for m in e.get("members", [])
                                                                   if m.get("role") == "outer" and m.get("geometry")]
            for g in members:
                ring = [list(geo.project(p["lat"], p["lon"])) for p in g]
                if len(ring) >= 4 and ring[0] == ring[-1]:
                    grid.add_ring(ring[:-1], ring[:-1])
    return grid


def in_park(parks, x, z):
    return any(geo.point_in_ring(x, z, r) for r in parks.query(x, z, x, z))


def plausible_height(b, tags, parks, notes):
    """Cap inferred (untagged) heights by structure type, park setting and footprint size."""
    if b["h_tag"] is not None:
        return b["h"]
    h = b["h"]
    name = tags.get("name", "") + " " + tags.get("name:en", "")
    kind = tags.get("building", "yes")
    o = b["obb"]
    reasons = []
    if any(part in name for part in SMALL_NAMES) or kind in SMALL_TAGS:
        h, reasons = min(h, 5.0), ["small structure by name/tag"]
    elif any(part in name for part in TEMPLE_NAMES) or kind in TEMPLE_TAGS:
        h, reasons = min(h, 18.0), ["temple/shrine hall by name/tag"]
    c = o["c"]
    if in_park(parks, c[0], c[1]):
        h = min(h, 6.6)
        reasons.append("inside a park/garden polygon")
    storey = 3.3
    cap = max(storey, math.floor(6.0 * min(o["w"], o["d"]) / storey) * storey)
    if ring_area(b["ring"]) < 40:
        cap = min(cap, 2 * storey)
    if b.get("zone") == "shiodome":
        # The 2002 Shiodome redevelopment was mostly under construction; untagged towers are not assumed.
        cap = min(cap, 13 * storey)
    if h > cap:
        h = cap
        reasons.append("footprint-limited inferred height")
    if h != b["h"]:
        notes.append({"id": b["id"], "from": b["h"], "to": round(h, 2), "reasons": reasons})
    return round(h, 2)


def streets_grid(source_plan):
    grid = Grid(50.0)
    for s in source_plan["streets"]:
        pts = s["pts"]
        for a, b in zip(pts[:-1], pts[1:]):
            grid.add_ring([a, b], (a, b, s["w"] / 2))
    return grid


def street_fraction(ring, streets):
    samples = interior_samples(ring, 120)
    if not samples:
        return 0.0
    hits = 0
    for x, z in samples:
        for a, b, half in streets.query(x, z, x, z):
            if geo.seg_dist2(x, z, a[0], a[1], b[0], b[1]) <= half * half:
                hits += 1
                break
    return hits / len(samples)


def geo_scale(ring, k):
    cx = sum(p[0] for p in ring) / len(ring)
    cz = sum(p[1] for p in ring) / len(ring)
    return [(cx + (p[0] - cx) * k, cz + (p[1] - cz) * k) for p in ring]


def resolve_overlaps(kept, excluded):
    grid = Grid(50.0)
    for b in kept:
        grid.add_ring(b["ring"], b)
    drop, inset = set(), set()
    samples = {}
    for a in sorted(kept, key=lambda b: b["id"]):
        if a["id"] in drop:
            continue
        xs, zs = [p[0] for p in a["ring"]], [p[1] for p in a["ring"]]
        for b in sorted(grid.query(min(xs), min(zs), max(xs), max(zs)), key=lambda b: b["id"]):
            if b is a or b["id"] in drop or b.get("seed_basis") == a.get("seed_basis"):
                continue
            small, large = (a, b) if ring_area(a["ring"]) <= ring_area(b["ring"]) else (b, a)
            pts = samples.setdefault(small["id"], interior_samples(small["ring"], 300))
            if not pts:
                continue
            frac = sum(geo.point_in_ring(x, z, large["ring"]) for x, z in pts) / len(pts)
            if frac <= 0.05:
                continue
            if small["h"] > large["h"] + 3 and frac > 0.9:
                inset.add(small["id"])     # tower standing on a podium footprint
                continue
            drop.add(small["id"])
            excluded.append({"id": small["id"], "reason": "footprint overlaps %s (%.0f %%); duplicate or inner part"
                             % (large["id"], frac * 100)})
            if small is a:
                break
    for b in kept:
        if b["id"] in inset:
            # Pull tower walls 0.4 m inside the podium walls; avoids coplanar facades.
            k = 1 - 0.8 / max(4.0, min(b["obb"]["w"], b["obb"]["d"]))
            b["ring"] = [[round(v, 3) for v in p] for p in geo_scale(b["ring"], k)]
            b["podium_inset"] = True
    return [b for b in kept if b["id"] not in drop]


def assign_variety(kept):
    """Neighbour-aware facade style, close wall, storey pitch and roof variant within district palettes."""
    grid = Grid(60.0)
    order = sorted(kept, key=lambda b: (b["obb"]["c"][1], b["obb"]["c"][0], b["id"]))
    for b in order:
        if b.get("landmark"):
            grid.add_ring(b["ring"], b)
            continue
        zone = b.get("zone") or "hamamatsucho"
        fam = b["family"]
        palette = INDUSTRIAL if fam == "industrial" else PALETTES.get((zone, fam)) or PALETTES[("hamamatsucho", fam)]
        if b.get("fixed_style"):
            palette = [b["fixed_style"]]
        c, r = b["obb"]["c"], 0.5 * math.hypot(b["obb"]["w"], b["obb"]["d"])
        near = []
        for o in grid.query(c[0] - r - 40, c[1] - r - 40, c[0] + r + 40, c[1] + r + 40):
            oc, orr = o["obb"]["c"], 0.5 * math.hypot(o["obb"]["w"], o["obb"]["d"])
            if math.dist(c, oc) - r - orr < 14:
                near.append(o)
        used = {o.get("style") for o in near}
        start = b["seed"] % len(palette)
        rotated = palette[start:] + palette[:start]
        free = [s for s in rotated if s not in used]
        b["style"] = free[0] if free else min(rotated, key=lambda s: sum(o.get("style") == s for o in near))
        walls = WALLS.get(b["style"], [0, 1, 2, 3])
        k = (b["seed"] >> 4) % len(walls)
        wall_order = walls[k:] + walls[:k]
        used_walls = {o.get("wall") for o in near}
        b["wall"] = next((w for w in wall_order if w not in used_walls), wall_order[0])
        choices = STOREYS[fam]
        used_pitch = {o.get("storey_m") for o in near}
        first = choices[(b["seed"] >> 8) % len(choices)]
        b["storey_m"] = next((p for p in [first] + list(choices) if p not in used_pitch), first)
        bay, panes = LAYOUTS.get(b["style"], (3.2, 1))
        bay += ((b["seed"] >> 12) % 3 - 1) * 0.2
        b["facade"] = {"bay": round(bay, 2), "panes": panes,
                       "sill": [0.75, 0.85, 0.9][(b["seed"] >> 14) % 3],
                       "balconies": b["style"] == "apartment_balcony",
                       "parapet": [0.45, 0.75, 1.1][(b["seed"] >> 16) % 3]}
        grid.add_ring(b["ring"], b)


def daiba_ring(e):
    if e.get("geometry"):
        ring = [list(geo.project(p["lat"], p["lon"])) for p in e["geometry"]]
        return ring[:-1] if ring[0] == ring[-1] else ring
    points = [geo.project(p["lat"], p["lon"]) for m in e.get("members", [])
              if m.get("role") == "outer" for p in (m.get("geometry") or [])]
    return convex_hull(points)


def site_pad(b, margin, bands):
    """Raised paving block under a building beyond the exported ground (quay-like edge to y -1.5)."""
    o = b["obb"]
    for m in (margin, margin * 0.66, margin * 0.33, 1.0):
        if b.get("collision_ring"):
            cr = b["collision_ring"]
            cc = [sum(p[0] for p in cr) / 4, sum(p[1] for p in cr) / 4]
            ring = rect(cc, o["u"], math.dist(cr[0], cr[1]) + 2 * m, math.dist(cr[1], cr[2]) + 2 * m)
        else:
            ring = rect(o["c"], o["u"], o["w"] + 2 * m, o["d"] + 2 * m)
        if not bands.footprint_conflict(ring, extra=0.5):
            return {"id": "pad_" + b["id"], "building": b["id"], "ring": [[round(v, 3) for v in p] for p in ring],
                    "top_y": 3.0, "bottom_y": -1.5, "cx": b["cx"], "cz": b["cz"], "margin_m": round(m, 2)}
    return None


def plan_infill(net, bands, kept, streets, water, parks):
    """Low near-road buildings and fenced yards on bare ground beside the expressways."""
    occupied = Grid(40.0)
    for b in kept:
        occupied.add_ring(b["ring"], b)
    zone_grid = defaultdict(list)
    for b in kept:
        if b.get("zone"):
            zone_grid[(math.floor(b["obb"]["c"][0] / 150), math.floor(b["obb"]["c"][1] / 150))].append(b)
    x0, z0, x1, z1 = GROUND_BOX
    out = []
    for cw in sorted(net["carriageways"], key=lambda c: c["id"]):
        pts = cw["points"]
        if not any(x0 < p[0] < x1 and z0 < p[2] < z1 for p in pts):
            continue
        hw = geo.band_halfwidth(cw) + geo.PLAN_SLACK
        tunnels = [(s["s0"], s["s1"]) for s in cw.get("structure", []) if s.get("type") == "tunnel"]
        s_acc, next_s = 0.0, 8.0
        for a, b in zip(pts[:-1], pts[1:]):
            seg = math.dist((a[0], a[2]), (b[0], b[2]))
            while seg > 1e-6 and next_s <= s_acc + seg:
                t = (next_s - s_acc) / seg
                p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)
                station = next_s
                next_s += 18.0
                if p[1] > 20 or any(s0 <= station <= s1 for s0, s1 in tunnels):
                    continue
                tx, tz = (b[0] - a[0]) / seg, (b[2] - a[2]) / seg
                for side, sgn, row in [(side, sgn, row) for side, sgn in (("L", 1), ("R", -1)) for row in range(3)]:
                    # Row 0 lines the clearance edge; rows 1-2 fill open lots up to about 45 m from the road.
                    key = "infill_%s_%s_%04d" % (cw["id"], side, int(station)) + ("_r%d" % row if row else "")
                    h32 = int(hashlib.sha256(key.encode("ascii")).hexdigest()[:8], 16)
                    nx, nz = -tz * sgn, tx * sgn
                    w = 10.0 + (h32 % 6)
                    d = 8.0 + (h32 >> 3) % 4
                    off = hw + 0.8 + row * 16.0 + d / 2
                    c = [p[0] + nx * off, p[2] + nz * off]
                    if row and any(geo.point_in_ring(p[0] + nx * t, p[2] + nz * t, o["ring"])
                                   for t in range(int(hw), int(off - d / 2), 3)
                                   for o in occupied.query(p[0] + nx * t, p[2] + nz * t, p[0] + nx * t, p[2] + nz * t)
                                   if not o.get("kind")):
                        continue    # an existing building already fronts the road here
                    # Edge 0 of rect() lies at c - (uz, -ux) * d / 2; orient it toward the road.
                    u = (-nz, nx)
                    ring = rect(c, u, w, d)
                    if not all(x0 + 5 < q[0] < x1 - 5 and z0 + 5 < q[1] < z1 - 5 for q in ring):
                        continue
                    big = rect(c, u, w + 6, d + 6)
                    xs, zs = [q[0] for q in big], [q[1] for q in big]
                    clash = False
                    for o in occupied.query(min(xs), min(zs), max(xs), max(zs)):
                        if any(geo.point_in_ring(q[0], q[1], o["ring"]) for q in big + [c]) or \
                                any(geo.point_in_ring(q[0], q[1], big) for q in o["ring"]):
                            clash = True
                            break
                    if clash or bands.footprint_conflict(ring):
                        continue
                    probe = [tuple(q) for q in ring] + [tuple(c)] + interior_samples(ring, 30)
                    if any((math.floor(x / 4) * 4.0, math.floor(z / 4) * 4.0) in water for x, z in probe):
                        continue
                    if any(geo.seg_dist2(x, z, sa[0], sa[1], sb[0], sb[1]) <= (half + 1.5) ** 2
                           for x, z in probe for sa, sb, half in streets.query(x - 20, z - 20, x + 20, z + 20)):
                        continue
                    if in_park(parks, c[0], c[1]):
                        continue
                    neighbours = zone_grid.get((math.floor(c[0] / 150), math.floor(c[1] / 150)), [])
                    zone = min(neighbours, key=lambda o: math.dist(o["obb"]["c"], c))["zone"] if neighbours else "hamamatsucho"
                    roll = h32 % 100
                    if zone == "waterfront":
                        kind, fam, floors = ("yard", "industrial", 0) if roll < 45 else ("building", "industrial", 1 + roll % 2)
                    elif zone in ("ginza", "shiodome"):
                        kind, fam, floors = ("yard", "commercial", 0) if roll < 20 else ("building", "commercial", 2 + roll % 3)
                    else:
                        kind, fam, floors = ("yard", "apartment", 0) if roll < 30 else \
                            ("building", "apartment" if roll % 2 else "commercial", 2 + roll % 3)
                    storey = {"apartment": 3.0, "commercial": 3.6, "industrial": 5.0}[fam]
                    h = 1.8 if kind == "yard" else floors * storey
                    obb = {"c": [round(c[0], 2), round(c[1], 2)], "u": [round(u[0], 5), round(u[1], 5)], "w": w, "d": d}
                    rec = {"id": key, "osm": 0, "ring": [[round(v, 3) for v in q] for q in ring], "obb": obb,
                           "h": round(h, 2), "h_tag": None, "family": fam, "zone": zone, "kind": "infill_" + kind,
                           "cx": math.floor(c[0] / 500), "cz": math.floor(c[1] / 500),
                           "chunk_anchor": [round(c[0], 3), round(c[1], 3)],
                           "near_m": round(off, 2), "detail": "window_mid", "flags": {"ac": roll % 3}, "tint": 1,
                           "off": [0, 0], "style": "office_ribbon", "seed_basis": key, "seed": h32,
                           "era_status": "plausible_infill_not_surveyed", "name_en": "", "storey_m": storey,
                           "road_edges": [0], "source": "procedural near-road infill (project-authored, not OSM)"}
                    out.append(rec)
                    occupied.add_ring(ring, rec)
            s_acc += seg
    return out


def plan_placements():
    source_plan = json.loads((ROOT / "models/shuto/rep_section/scenery_plan.json").read_text(encoding="utf-8"))
    elements = {}
    for path in sorted((ROOT / "data/shuto/raw").glob("overpass_scenery_buildings_*.json")):
        for e in json.loads(path.read_text(encoding="utf-8"))["elements"]:
            elements[e["id"]] = e
    buildings = list(source_plan["buildings"])
    # Recover the original footprint, rather than keep oversized kit landmark substitutions.
    for landmark in source_plan["landmarks"]:
        e = elements.get(landmark["osm"], {})
        ring = [list(geo.project(p["lat"], p["lon"])) for p in e.get("geometry", [])]
        if len(ring) > 1 and ring[0] == ring[-1]:
            ring.pop()
        if len(ring) < 3:
            continue
        obb = geo.obb(ring)
        buildings.append({"id": "osm_%d" % landmark["osm"], "osm": landmark["osm"],
                          "ring": ring, "obb": obb, "h": landmark["h_osm"], "h_tag": landmark["h_osm"],
                          "cx": math.floor(obb["c"][0] / 500), "cz": math.floor(obb["c"][1] / 500),
                          "family": "commercial", "style": "office_ribbon", "tint": 0,
                          "off": [0, 0], "flags": {}, "near_m": 999, "lod": "far"})
    # Fuji TV is just beyond the road-section box and visible from the Daiba approach.
    fuji = elements[287770734]
    ring = [list(geo.project(p["lat"], p["lon"])) for p in fuji["geometry"][:-1]]
    obb = geo.obb(ring)
    buildings.append({"id": "osm_287770734", "osm": 287770734, "ring": ring, "obb": obb,
                      "h": 123.45, "h_tag": 123.45, "cx": math.floor(obb["c"][0] / 500),
                      "cz": math.floor(obb["c"][1] / 500), "family": "commercial", "style": "office_ribbon",
                      "tint": 0, "off": [0, 0], "flags": {}, "near_m": 999, "lod": "far", "landmark": "fuji"})
    # Daiba waterfront context completed before 2003 (stand-alone silhouettes beyond the road box).
    for spec in DAIBA:
        ring = daiba_ring(elements[spec["osm"]])
        obb = geo.obb(ring)
        buildings.append({"id": "osm_%d" % spec["osm"], "osm": spec["osm"], "ring": ring, "obb": obb,
                          "h": spec["h"], "h_tag": spec["h"], "cx": math.floor(obb["c"][0] / 500),
                          "cz": math.floor(obb["c"][1] / 500), "family": spec["family"], "style": spec["style"],
                          "fixed_style": spec["style"], "tint": 1, "off": [0, 0], "flags": {"ac": 2}, "near_m": 999,
                          "lod": "far", "zone": "daiba", "daiba_context": spec["name"], "opened": spec["opened"],
                          "source": spec["source"]})
    era_exclusions = {819117400: "Toranomon Hills Mori Tower (2014)",
                      151481757: "Shiodome City Center completed 2003; finished tower excluded",
                      151481850: "Nippon Television Tower completed 2003; finished tower excluded",
                      155509991: "World City Towers (post-2002)", 155509996: "World City Towers (post-2002)",
                      155510017: "World City Towers (post-2002)", 155509993: "Shibaura Island Cape Tower (post-2002)",
                      155510052: "Shibaura Island Grove Tower (post-2002)", 157573072: "Shibaura Island Bloom Tower (post-2002)",
                      155012544: "City Tower Shinagawa (post-2002)"}
    net = json.loads((ROOT / "data/shuto/network.json").read_text(encoding="utf-8"))
    # Include the additional Daiba landmark's neighboring graph roads in the clearance index.
    geo.BBOX = (-2600.0, 1500.0, 300.0, 6800.0)
    bands = geo.RoadIndex(net)
    samples = road_samples(net)
    parks = load_parks()
    streets = streets_grid(source_plan)
    water = set()
    for r in source_plan["water_rects"]:
        for i in range(int(round((r["x1"] - r["x0"]) / 4))):
            for k in range(int(round((r["z1"] - r["z0"]) / 4))):
                water.add((r["x0"] + i * 4.0, r["z0"] + k * 4.0))
    occurrences = Counter(b["id"] for b in buildings)
    part_number = defaultdict(int)
    for b in buildings:
        original_id = b["id"]
        b["seed_basis"] = original_id
        if occurrences[original_id] > 1:
            points = [tuple(round(v, 3) for v in p) for p in b["ring"]]
            orders = [points[i:] + points[:i] for i in range(len(points))]
            reverse = points[::-1]
            orders.extend(reverse[i:] + reverse[:i] for i in range(len(reverse)))
            signature = hashlib.sha256(json.dumps(min(orders), separators=(",", ":")).encode("ascii")).hexdigest()[:10]
            b["legacy_id"] = original_id
            b["legacy_part_index"] = part_number[original_id]
            part_number[original_id] += 1
            b["id"] = original_id + "_part_" + signature
    kept, excluded, height_notes = [], [], []
    for b in sorted(buildings, key=lambda b: b["id"]):
        tags = elements.get(b["osm"], {}).get("tags", {})
        date = tags.get("start_date", tags.get("building:year_built", ""))
        future = date[:4].isdigit() and int(date[:4]) > 2002
        names = tags.get("name", "") + " | " + tags.get("name:en", "")
        by_name = next((reason for part, reason in POST2002_NAMES.items() if part in names), None)
        if b["osm"] in era_exclusions or future or by_name:
            excluded.append({"id": b["id"], "reason": era_exclusions.get(b["osm"]) or by_name or "start_date=" + date})
            continue
        if tags.get("building") == "roof" and not b.get("landmark"):
            excluded.append({"id": b["id"], "reason": "building=roof canopy, not an enclosed building mass"})
            continue
        if bands.footprint_conflict(b["ring"]):
            excluded.append({"id": b["id"], "reason": "road corridor including planning slack"})
            continue
        if not b.get("landmark") and not b.get("daiba_context") and street_fraction(b["ring"], streets) > 0.05:
            excluded.append({"id": b["id"], "reason": "footprint lies on an exported local street"})
            continue
        c = b["obb"]["c"]
        centroid = [sum(p[i] for p in b["ring"]) / len(b["ring"]) for i in range(2)]
        b["chunk_anchor"] = centroid if [math.floor(v / 500) for v in centroid] == [b["cx"], b["cz"]] else c
        assert [math.floor(v / 500) for v in b["chunk_anchor"]] == [b["cx"], b["cz"]]
        # Cached near_m is retained except for formerly substituted landmarks.
        if b["near_m"] == 999:
            b["near_m"] = round(min(math.hypot(c[0] - p[0], c[1] - p[2])
                                        for cw in net["carriageways"] for p in cw["points"]), 2)
        b["seed"] = int(hashlib.sha256(b["seed_basis"].encode("ascii")).hexdigest()[:8], 16)
        b["detail"] = "near" if b["near_m"] <= 50 else "window_mid" if b["near_m"] <= 110 else "mid" if b["near_m"] <= 350 else "far"
        b["era_status"] = "date_tag_verified" if date else "undated_current_OSM_approximation"
        b["name_en"] = tags.get("name:en", "")
        b["storey_m"] = 3.3 if b["family"] == "apartment" else 3.6 if b["family"] == "commercial" else 5.0
        b["h"] = plausible_height(b, tags, parks, height_notes)
        if b.get("daiba_context"):
            b["detail"] = "mid"
            b["era_status"] = "pre2003_named_daiba_context"
            b["name_en"] = b["daiba_context"]
        if b["osm"] == 151481755:
            b["landmark"] = "dentsu"
            b["detail"] = "mid"
            b["storey_m"] = b["h"] / 48
        if b.get("landmark") == "fuji":
            b["detail"] = "mid"
            b["storey_m"] = 4.1
            ux, uz = b["obb"]["u"]
            cx, cz = b["obb"]["c"]
            b["collision_ring"] = [[cx + ux * x + uz * z, cz + uz * x - ux * z]
                                   for x, z in [(-152, -36), (152, -36), (152, 68), (-152, 68)]]
            b["model_offset_m"] = 16.0
            b["model_anchor"] = [cx + uz * 16, cz - ux * 16]
            b["model_site_extent_m"] = [304, 104]
            b["model_reference"] = "Supplied waterfront photo; estimated extended lower complex beyond OSM main footprint"
            assert not bands.footprint_conflict(b["collision_ring"]), "Fuji campus reaches a road corridor"
        kept.append(b)
    # Daiba context must not intersect the estimated Fuji TV campus envelope.
    fuji_rec = next(b for b in kept if b.get("landmark") == "fuji")
    for b in list(kept):
        if b.get("daiba_context") and any(geo.point_in_ring(p[0], p[1], fuji_rec["collision_ring"]) for p in b["ring"]):
            kept.remove(b)
            excluded.append({"id": b["id"], "reason": "intersects the estimated Fuji TV campus envelope"})
    kept = resolve_overlaps(kept, excluded)
    for b in kept:
        if any(not (GROUND_BOX[0] <= p[0] <= GROUND_BOX[2] and GROUND_BOX[1] <= p[1] <= GROUND_BOX[3])
               for p in b.get("collision_ring", b["ring"])):
            b["skirt"] = True     # base walls continue to y -1.5 where no exported ground exists
        b["road_edges"] = road_edges(b["ring"], samples)
    infill = plan_infill(net, bands, kept, streets, water, parks)
    kept.extend(infill)
    kept.sort(key=lambda b: b["id"])
    assign_variety(kept)
    pads = []
    for b in kept:
        if b.get("landmark") == "fuji" or b.get("daiba_context"):
            pad = site_pad(b, 14.0 if b.get("landmark") else 8.0, bands)
            if pad:
                pads.append(pad)
    plan = {"format": "nox-shuto-buildings", "version": 2, "reference_date": "2002-12-31",
            "units": "metres", "axes": "+X east, +Y up, +Z south", "chunk_m": 500,
            "bbox": source_plan["bbox"], "buildings": kept, "excluded": excluded, "site_pads": pads,
            "height_adjustments": height_notes,
            "road_source_sha256": hashlib.sha256((ROOT / "data/shuto/network.json").read_bytes()).hexdigest(),
            "geography": "Current OSM footprints; untagged era/height is approximate, not historical survey",
            "chunk_ownership": "Existing footprint-vertex centroid ownership; recovered landmarks use their recorded OBB anchor",
            "detail_distances_m": {"full_geometry": 50, "lightweight_parallax": 110, "textured_mid": 350},
            "infill": "Project-authored near-road low buildings/yards on bare ground; ids infill_<carriageway>_<side>_<station m>",
            "attribution": "(c) OpenStreetMap contributors, ODbL-1.0"}
    print("buildings", len(kept), "excluded", len(excluded), "near", sum(b["detail"] == "near" for b in kept),
          "infill", len(infill), "pads", len(pads), "height adjustments", len(height_notes))
    return plan


if __name__ == "__main__":
    main()
