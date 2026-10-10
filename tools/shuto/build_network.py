#!/usr/bin/env python3
"""Build the Shuto Expressway road dataset (contract: docs/shuto/network_format.md).

Pipeline
  1. Fetch OpenStreetMap motorway ways and facilities from Overpass (cached in
     data/shuto/raw/, delete the cache or pass --refresh to re-download).
  2. Select Shuto ways (operator / ref pattern), drop post-2002 sections (era rules),
     and keep foreign expressway/street connections as boundary nodes.
  3. Build directed carriageways between topological nodes, assign route / dir / ids.
  4. Project to a local transverse Mercator frame (Kruger series, WGS84, origin
     Edobashi JCT), resample to <=10 m, light smoothing (|displacement| <= 0.3 m).
  5. Elevation: tag-based targets, gradient-limited profiles (6 % main, 8 % ramps),
     node continuity, and crossing clearance (deck to deck >= 7 m) by lifting the upper
     carriageway locally. Unresolved crossings are logged in build_report.json.
  6. Write network.json, nox_road_network.json and build_report.json.

Run:  python tools/shuto/build_network.py [--refresh]
Python 3.11, standard library only.
"""

import argparse
import bisect
import collections
import datetime
import json
import math
import re
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / "data" / "shuto" / "raw"
OUT = ROOT / "data" / "shuto"
MANIFEST = ROOT / "docs" / "shuto" / "route_manifest.json"

UA = "NoxShutoBuilder/1.0 (Nox_Engine Shuto road dataset build)"
MIRRORS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.private.coffee/api/interpreter",
]

# South, West, North, East. Wide enough for Yokohama Yokosuka and Chiba ends.
BBOX = (35.30, 139.50, 35.85, 140.05)
BB = ",".join(str(v) for v in BBOX)
Q_MOTORWAY = f"""[out:json][timeout:300];
(way["highway"~"^(motorway|motorway_link)$"]({BB});
 way["operator"~"\u9996\u90fd\u9ad8"]({BB}););
out body geom;"""
Q_FACILITIES = f"""[out:json][timeout:180];
(node["highway"~"^(services|rest_area)$"]({BB});
 way["highway"~"^(services|rest_area)$"]({BB});
 node["amenity"="parking"]["name"~"PA|\u30d1\u30fc\u30ad\u30f3\u30b0"]({BB});
 way["amenity"="parking"]["name"~"PA|\u30d1\u30fc\u30ad\u30f3\u30b0"]({BB});
 node["barrier"="toll_booth"]({BB});
 node["highway"="motorway_junction"]({BB});
 way["highway"="motorway_junction"]({BB}););
out body geom;"""

ORIGIN_LAT, ORIGIN_LON = 35.6843, 139.7745
GROUND_Y = 3.0
REF_DATE = datetime.date(2002, 12, 31)
SPACING = 10.0          # max centreline spacing (m)
SMOOTH_MAX = 0.3        # max smoothing displacement (m)
GRADE_MAIN, GRADE_RAMP = 0.06, 0.08
CLEAR = 7.0             # deck-to-deck clearance at crossings (m)
ELEV_CROSS_RANGE = 120.0 # half-length of local lift around a crossing (m)

# Shuto refs (OSM) that belong to the manifest route list. C3, C4, CA, E*, 14, 16, 17, 357,
# 298 and D8 are other roads and are deliberately absent.
SHUTO_REFS = {"C1", "C2", "B", "1", "2", "3", "4", "5", "6", "7", "9", "10", "11",
              "K1", "K2", "K3", "K5", "K6", "K7", "S1", "S2", "S5"}
# Name keywords decide ambiguous refs (1 and 6 are shared by two routes each).
NAME_ROUTES = [("\u4e0a\u91ce\u7dda", "1-Ueno"), ("\u7fbd\u7530\u7dda", "1-Haneda"), ("\u5411\u5cf6\u7dda", "6-Mukojima"),
               ("\u4e09\u90f7\u7dda", "6-Misato"), ("\u516b\u91cd\u6d32\u7dda", "Y"), ("\u5ddd\u53e3\u7dda", "S1"), ("\u5927\u5bae\u7dda", "S5"),
               ("\u65b0\u90fd\u5fc3\u7dda", "S2"), ("\u6a2a\u6d5c\u5317\u7dda", "K7"), ("\u6a2a\u6d5c\u5317\u897f\u7dda", "K7"), ("\u6674\u6d77\u7dda", "10")]
# Routes wholly opened after the reference date (manifest: 10, K7, S2 main sections).
ERA_ROUTE_DROP = {"10": "10 Harumi Route, opened 2009/2018",
                  "K7": "K7 Yokohama North Route, opened 2017/2020",
                  "S2": "S2 Saitama Shintoshin Route main sections, opened 2004/2006"}
# C2 sections open at 2002-12-31 (manifest C2-1..4, C2-6): Itabashi JCT - Kohoku JCT -
# Senju-shinbashi - Horikiri JCT - Yotsugi - Kasai JCT. Everything else on C2 (Yamate Tunnel,
# Kumanocho, Ohashi, Komatsugawa JCT, ...) opened later and is dropped. Coordinates are OSM
# junction nodes; a way is kept only if its midpoint is within C2_CORRIDOR_HALF of this line.
C2_OPEN_2002 = [(35.74742, 139.70827),   # Itabashi JCT
                (35.7455, 139.7267), (35.7491, 139.7347), (35.7556, 139.7424),  # real arc bows south
                (35.7594, 139.7495), (35.76518, 139.75994), (35.76212, 139.79700),
                (35.74675, 139.82249), (35.72886, 139.83687), (35.70187, 139.85932),
                (35.64641, 139.85639)]   # Komatsugawa JCT is on the real Yotsugi-Kasai alignment
C2_CORRIDOR_HALF = 700.0    # the real arc curves up to ~600 m from the chord between junctions
C2_KUMANOCHO = (35.74265, 139.70790)   # Kumanocho JCT (opened 2007): excluded within 350 m

# Deck heights above ground for elevated layers (approx. contract values).
DECK_REL = {1: 9.0, 2: 17.0, 3: 25.0}
# Long-span crowns: absolute TP heights (approximate, public sources noted in report).
LONG_SPAN = [
    (("\u30ec\u30a4\u30f3\u30dc\u30fc", "Rainbow"), 52.0, "Rainbow Bridge upper deck ~ +52 m (approx.)"),
    (("\u6a2a\u6d5c\u30d9\u30a4\u30d6\u30ea\u30c3\u30b8", "Bay Bridge"), 55.0, "Yokohama Bay Bridge upper deck B ~ +55 m (approx.)"),
    (("\u3064\u3070\u3055", "Tsubasa"), 40.0, "Tsurumi Tsubasa Bridge ~ +40 m (approx.)"),
]

# Era rules: sections opened after the reference date (by name). Dates are approximate
# until route_manifest.json is available; OSM start_date tags are also honoured.
ERA_NAME_RULES = [
    ("\u5c71\u624b\u30c8\u30f3\u30cd\u30eb", "2007-12-22", "C2 Yamate Tunnel (approx. opening date)"),
    ("\u6771\u4eac\u30b2\u30fc\u30c8\u30d6\u30ea\u30c3\u30b8", "2012-03-17", "Tokyo Gate Bridge (approx. opening date)"),
    ("\u4e2d\u592e\u74b0\u72b6\u65b0\u5bbf\u7dda", "2015-03-14", "C4 Shinjuku Line (approx. opening date)"),
    ("\u6a2a\u6d5c\u5317\u7dda", "2009-03-01", "K7 Yokohama Kita Line (approx. opening date)"),
]

# Common foreign expressway refs -> ASCII names for boundary exit info.
FOREIGN_NAMES = {
    "E1": "Tomei Expressway", "E1A": "Shin-Tomei Expressway", "E2": "Meishin Expressway",
    "E3": "Chuo Expressway", "E4": "Tohoku Expressway", "E6": "Joban Expressway",
    "E14": "Higashi-Kanto Expressway", "E16": "Keiyo Expressway", "E17": "Kan-Etsu Expressway",
    "E20": "Chuo Expressway", "E51": "Tokyo Gaikan Expressway", "E83": "Kawasaki/Yokohane route",
    "CA": "Tokyo-Aqua Line", "E466": "Yokohama-Yokosuka Road", "E409": "Yokohama-Yokosuka Road",
}

LOG = []


def log(msg):
    print(msg, flush=True)
    LOG.append(msg)


# ---------------------------------------------------------------- geometry

R_EARTH = 6371008.8


def haversine(a, b):
    """Great-circle distance in metres between (lat, lon) pairs."""
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2
    return 2 * R_EARTH * math.asin(math.sqrt(h))


# WGS84 transverse Mercator (Kruger series, Karney 2011), k0 = 1, central meridian = origin.
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
    """NOX frame: x east, z south, metres, origin at Edobashi JCT."""
    e_, n_ = _tm_raw(lat, lon)
    return e_ - _E0, -(n_ - _N0)


def unproject_check():
    """Self-check: 1 km east of origin should be ~1000 m in x (scale 1:1).
    Longitude step uses the WGS84 prime-vertical radius N (the TM is on the ellipsoid)."""
    phi = math.radians(ORIGIN_LAT)
    n_pv = _A / math.sqrt(1 - (_E ** 2) * math.sin(phi) ** 2)
    lon2 = ORIGIN_LON + math.degrees(1000.0 / (n_pv * math.cos(phi)))
    x, z = project(ORIGIN_LAT, lon2)
    return x, z, 1000.0


# ---------------------------------------------------------------- data fetch

def fetch_overpass(name, query, refresh):
    """Return parsed Overpass JSON, cached as data/shuto/raw/<name>.json."""
    path = RAW / f"{name}.json"
    if path.exists() and not refresh:
        log(f"cache: {path.name}")
        return json.loads(path.read_text(encoding="utf-8"))
    last = None
    for attempt in range(5):
        for url in MIRRORS:
            try:
                req = urllib.request.Request(url, data=query.encode("utf-8"),
                                             headers={"User-Agent": UA})
                with urllib.request.urlopen(req, timeout=400) as r:
                    body = r.read().decode("utf-8")
                data = json.loads(body)
                if "elements" not in data:
                    raise ValueError("no elements key")
                path.write_text(body, encoding="utf-8")
                log(f"fetched {name} from {url} ({len(data['elements'])} elements)")
                return data
            except Exception as ex:  # keep trying mirrors and attempts
                last = ex
                log(f"  {url} failed: {ex}")
        time.sleep(30 * (attempt + 1))
    raise SystemExit(f"Overpass fetch failed for {name}: {last}")


# ---------------------------------------------------------------- tag logic

def parse_int(v, default=0):
    try:
        return int(str(v).split(";")[0].strip())
    except (TypeError, ValueError):
        return default


def refs_of(tags):
    return [r.strip() for r in tags.get("ref", "").split(";") if r.strip()]


def route_of(tags):
    """Manifest route id for a way from its name or ref, or None if not a Shuto route."""
    name = tags.get("name", "")
    if "\u90fd\u5fc3\u74b0\u72b6\u7dda" in name:   # Inner Circular (C1) by exact name
        return "C1"
    if "\u4e2d\u592e\u74b0\u72b6\u7dda" in name:   # Central Circular (C2) by name
        return "C2"
    for key, rid in NAME_ROUTES:
        if key in name:
            return rid
    refs = [r for r in refs_of(tags) if r in SHUTO_REFS]
    if not refs:
        return None
    r = refs[0]
    if r == "1":
        return "1-Ueno" if "\u4e0a\u91ce" in name else "1-Haneda"
    if r == "6":
        return "6-Misato" if "\u4e09\u90f7" in name else "6-Mukojima"
    return r


def is_shuto(tags):
    if route_of(tags):
        return True
    text = tags.get("operator", "") + tags.get("name", "") + tags.get("operator:ja", "")
    return "\u9996\u90fd\u9ad8" in text


def dist_to_polyline_xz(pt_xz, poly_xz):
    """Distance (m) from a point to a polyline in the NOX frame."""
    best = 1e18
    px, pz = pt_xz
    for (ax, az), (bx, bz) in zip(poly_xz, poly_xz[1:]):
        dx, dz = bx - ax, bz - az
        L2 = dx * dx + dz * dz or 1e-9
        t = min(max(((px - ax) * dx + (pz - az) * dz) / L2, 0.0), 1.0)
        best = min(best, math.hypot(px - (ax + t * dx), pz - (az + t * dz)))
    return best


def era_reason(tags):
    """Return a reason string if the way was not open at REF_DATE, else None."""
    for key in ("start_date", "opening_date"):
        v = tags.get(key, "")
        m = re.match(r"(\d{4})(?:-(\d{2})-(\d{2}))?", v)
        if m:
            d = datetime.date(int(m.group(1)), int(m.group(2) or 1), int(m.group(3) or 1))
            if d > REF_DATE:
                return f"{key}={v}"
    name = tags.get("name", "")
    for key, date, label in ERA_NAME_RULES:
        if key in name and datetime.date.fromisoformat(date) > REF_DATE:
            return f"{label}, {date}"
    return None


def long_span_y(tags):
    names = tags.get("name", "") + tags.get("name:en", "")
    for keys, y, _ in LONG_SPAN:
        if any(k in names for k in keys):
            return y
    return None


def classify(tags, is_link):
    """Return (structure_type, target_y) for a piece of road, with y in TP metres."""
    layer = parse_int(tags.get("layer"), 0)
    tunnel = tags.get("tunnel", "no") not in ("no", "") or tags.get("location") == "underground"
    bridge = tags.get("bridge", "no") not in ("no", "")
    ly = long_span_y(tags)
    if ly is not None and (bridge or layer >= 1):
        return "elevated", ly
    if tunnel:
        name = tags.get("name", "")
        if "\u7fbd\u7530" in name or "\u6e2f" in name:          # river / bay tunnels, deeper
            return "tunnel", GROUND_Y - 15.0
        return "tunnel", GROUND_Y - 8.0 * max(1, -layer)
    if bridge or layer >= 1:
        lv = min(max(1, layer), 3)
        return "elevated", GROUND_Y + DECK_REL[lv]
    if layer < 0 or tags.get("cutting") not in (None, "no", ""):
        return "cut", GROUND_Y - 6.0
    return "at_grade", GROUND_Y


def default_lanes(tags, is_link):
    if "lanes" in tags:
        return max(1, parse_int(tags["lanes"], 2))
    return 1 if is_link else 2


def lane_width(tags, route, is_link):
    if is_link:
        return 3.0
    return 3.5 if route == "B" else 3.25


def speed_kmh(tags, is_link):
    m = re.match(r"(\d+)", tags.get("maxspeed", ""))
    if m:
        return int(m.group(1))
    return 40 if is_link else 60


# ---------------------------------------------------------------- foreign names

def foreign_info(tags):
    refs = refs_of(tags)
    ref = refs[0] if refs else ""
    name = FOREIGN_NAMES.get(ref)
    if name is None:
        en = tags.get("name:en", "")
        name = en if en and all(ord(c) < 128 for c in en) else (f"Expressway {ref}".strip() or "unnamed road")
    op = tags.get("operator", "")
    if op and all(ord(c) < 128 for c in op):
        operator = op
    else:
        operator = "NEXCO" if ref.startswith("E") else "other operator"
    return name, operator


def ascii_or_empty(s):
    return s if s and all(ord(c) < 128 for c in s) else ""


# ---------------------------------------------------------------- graph building

def load_ways(motor):
    ways = {}
    for el in motor.get("elements", []):
        if el.get("type") != "way" or "geometry" not in el or "nodes" not in el:
            continue
        pts = [(g["lat"], g["lon"]) for g in el["geometry"]]
        ways[el["id"]] = {"id": el["id"], "nodes": el["nodes"], "pts": pts,
                          "tags": el.get("tags", {}), "shuto": False, "drop": False,
                          "route": None, "inferred": False}
    return ways


def build_graph(ways, node_bbox_margin=0.003):
    """Select Shuto ways, drop post-2002 ways, build directed segments and chains."""
    excluded = []
    c2_poly = [project(la, lo) for la, lo in C2_OPEN_2002]
    kum = project(*C2_KUMANOCHO)
    # Select Shuto ways and drop post-reference-date sections.
    for w in ways.values():
        t = w["tags"]
        if is_shuto(t):
            w["shuto"] = True
            w["route"] = route_of(t)
            reason = era_reason(t)
            if w["route"] in ERA_ROUTE_DROP:
                reason = ERA_ROUTE_DROP[w["route"]]
            if w["route"] == "C2" and w["pts"]:
                mid = w["pts"][len(w["pts"]) // 2]
                mid_xz = project(*mid)
                if (dist_to_polyline_xz(mid_xz, c2_poly) > C2_CORRIDOR_HALF
                        or math.hypot(mid_xz[0] - kum[0], mid_xz[1] - kum[1]) < 180.0):
                    reason = "C2 section not open at 2002-12-31 (outside 2002 Itabashi-Kasai arc; manifest C2-7..11)"
            if reason:
                w["drop"] = True
                excluded.append({"osm_ways": [w["id"]], "route": w["route"] or "",
                                 "reason": f"opened after 2002-12-31 ({reason})"})
    # Motorway links without Shuto ref that touch a Shuto way are assumed Shuto.
    for _ in range(3):
        touch = collections.defaultdict(int)
        for w in ways.values():
            if w["shuto"] and not w["drop"]:
                for n in w["nodes"]:
                    touch[n] += 1
        for w in ways.values():
            t = w["tags"]
            if w["shuto"] or w["drop"] or t.get("highway") != "motorway_link":
                continue
            if any(r.startswith("E") for r in refs_of(t)):
                continue
            if any(touch.get(n) for n in (w["nodes"][0], w["nodes"][-1])):
                w["shuto"] = True
                w["inferred"] = True
    shuto_ways = {k: w for k, w in ways.items() if w["shuto"] and not w["drop"]}
    foreign_ways = {k: w for k, w in ways.items()
                    if not w["shuto"] and not w["drop"] and w["tags"].get("highway", "").startswith("motorway")}
    # Routes: first Shuto ref; otherwise inherit from adjacent Shuto way.
    node_ways = collections.defaultdict(list)
    for w in shuto_ways.values():
        for n in w["nodes"]:
            node_ways[n].append(w["id"])
    queue = collections.deque(w["id"] for w in shuto_ways.values() if w["route"])
    while queue:
        cur = shuto_ways[queue.popleft()]
        for n in cur["nodes"]:
            for oid in node_ways[n]:
                o = shuto_ways[oid]
                if o["route"] is None:
                    o["route"] = cur["route"]
                    queue.append(oid)
    for w in shuto_ways.values():
        if w["route"] is None:
            w["route"] = "UNASSIGNED"
    # Dropped (post-2002) nodes: where a Shuto section ends at a closed-at-reference gap.
    dropped_nodes = set()
    for w in ways.values():
        if w["drop"]:
            dropped_nodes.update(w["nodes"])
    # Split ways where another way's endpoint meets them at an interior node (ramps joining a
    # mainline way mid-span), so that every junction is a graph node.
    endpoint_nodes = set()
    interior_nodes = set()
    for w in shuto_ways.values():
        endpoint_nodes.update((w["nodes"][0], w["nodes"][-1]))
        interior_nodes.update(w["nodes"][1:-1])
    split_nodes = endpoint_nodes & interior_nodes
    # Directed segments.
    segs = []
    for w in sorted(shuto_ways.values(), key=lambda x: x["id"]):
        ow = w["tags"].get("oneway", "")
        if ow in ("no", "0"):
            dirs = [1, -1]
        elif ow == "-1":
            dirs = [-1]
        else:
            dirs = [1]
        n_all = len(w["nodes"])
        cuts = [0] + [i for i in range(1, n_all - 1) if w["nodes"][i] in split_nodes] + [n_all - 1]
        for a, b in zip(cuts, cuts[1:]):
            for d in dirs:
                nodes = w["nodes"][a:b + 1]
                pts = w["pts"][a:b + 1]
                if d < 0:
                    nodes, pts = nodes[::-1], pts[::-1]
                segs.append({"way": w["id"], "nodes": nodes, "pts": pts, "route": w["route"],
                             "tags": w["tags"], "link": w["tags"].get("highway") == "motorway_link"})
    ins = collections.defaultdict(list)
    outs = collections.defaultdict(list)
    for i, s in enumerate(segs):
        outs[s["nodes"][0]].append(i)
        ins[s["nodes"][-1]].append(i)

    topo = set()
    for n in set(list(ins) + list(outs)):
        if len(ins[n]) != 1 or len(outs[n]) != 1:
            topo.add(n)
        elif segs[ins[n][0]]["route"] != segs[outs[n][0]]["route"]:
            topo.add(n)

    visited = set()
    raw_chains = []

    def walk(i0):
        seq = [i0]
        visited.add(i0)
        cur = segs[i0]["nodes"][-1]
        while cur not in topo:
            nxt = outs[cur][0]
            if nxt in visited:
                break
            seq.append(nxt)
            visited.add(nxt)
            cur = segs[nxt]["nodes"][-1]
        return seq

    for i in range(len(segs)):
        if i not in visited and segs[i]["nodes"][0] in topo:
            raw_chains.append(walk(i))
    for i in range(len(segs)):          # closed loops without topological nodes
        if i not in visited:
            topo.add(segs[i]["nodes"][0])
            raw_chains.append(walk(i))
    return segs, ins, outs, topo, raw_chains, excluded, foreign_ways, dropped_nodes, node_ways, ways


def near_bbox_edge(pt):
    s, w, n, e = BBOX
    m = 0.003
    return pt[0] < s + m or pt[0] > n - m or pt[1] < w + m or pt[1] > e - m


def make_chains(segs, ins, outs, topo, raw_chains, foreign_ways, dropped_nodes, node_ways, ways):
    """Turn raw segment sequences into carriageway records with boundary handling."""
    chains = []
    for seq in raw_chains:
        first, last = segs[seq[0]], segs[seq[-1]]
        f_node, t_node = first["nodes"][0], last["nodes"][-1]
        ch = {"segs": seq, "from": f_node, "to": t_node, "ext_from": None, "ext_to": None,
              "from_kind": None, "to_kind": None, "pieces": []}
        pieces = []
        for si in seq:
            s = segs[si]
            pieces.append({"way": s["way"], "tags": s["tags"], "link": s["link"], "pts": s["pts"]})
        # Start side.
        if not ins[f_node]:
            ext, fid = _foreign_at(f_node, foreign_ways, outs, ins)
            if f_node in dropped_nodes:
                ch["from_kind"] = ("boundary", "closed section (opened after 2002-12-31)", "n/a")
            elif ext is not None:
                pieces.insert(0, {"way": fid, "tags": ways[fid]["tags"], "link": False,
                                  "pts": list(reversed(ext))[:-1]})
                ch["from_kind"] = ("boundary",) + foreign_info(ways[fid]["tags"])
                ch["ext_from"] = True
            elif near_bbox_edge(segs[seq[0]]["pts"][0]):
                ch["from_kind"] = ("boundary", "outside extract", "n/a")
            elif first["link"]:
                ch["from_kind"] = ("boundary", "street network", "city")
            else:
                ch["from_kind"] = ("end", "", "")
        # End side.
        if not outs[t_node]:
            ext, fid = _foreign_at(t_node, foreign_ways, ins, outs)
            if t_node in dropped_nodes:
                ch["to_kind"] = ("boundary", "closed section (opened after 2002-12-31)", "n/a")
            elif ext is not None:
                pieces.append({"way": fid, "tags": ways[fid]["tags"], "link": False, "pts": ext[1:]})
                ch["to_kind"] = ("boundary",) + foreign_info(ways[fid]["tags"])
                ch["ext_to"] = True
            elif near_bbox_edge(last["pts"][-1]):
                ch["to_kind"] = ("boundary", "outside extract", "n/a")
            elif last["link"]:
                ch["to_kind"] = ("boundary", "street network", "city")
            else:
                ch["to_kind"] = ("end", "", "")
        ch["pieces"] = pieces
        chains.append(ch)
    return chains


def _foreign_at(node, foreign_ways, same_side, other_side):
    """If a foreign expressway touches this Shuto dead end, return 200 m of its geometry."""
    if len(same_side[node]) != 1 or len(other_side[node]) != 0:
        return None, None
    for fid, fw in foreign_ways.items():
        if node in (fw["nodes"][0], fw["nodes"][-1]) or node in fw["nodes"]:
            return _walk_foreign(fw, node, 200.0), fid
    return None, None


def _walk_foreign(fw, node, length):
    """Points from `node` (index 0) for `length` metres along foreign way fw."""
    nodes, pts = fw["nodes"], fw["pts"]
    idx = nodes.index(node)
    if idx == len(nodes) - 1:
        seq = list(range(idx, -1, -1))
    else:
        seq = list(range(idx, len(nodes)))
    out = [pts[seq[0]]]
    acc = 0.0
    for a, b in zip(seq, seq[1:]):
        d = haversine(pts[a], pts[b])
        if d <= 0:
            continue
        if acc + d >= length:
            f = (length - acc) / d
            out.append((pts[a][0] + (pts[b][0] - pts[a][0]) * f,
                        pts[a][1] + (pts[b][1] - pts[a][1]) * f))
            return out
        acc += d
        out.append(pts[b])
    return out if len(out) > 1 else None


# ---------------------------------------------------------------- resampling

def resample(chain):
    """Project chain pieces, resample at <= SPACING and smooth. Returns arrays."""
    xz, piece_of = [], []
    lls = []
    for k, p in enumerate(chain["pieces"]):
        for pt in p["pts"]:
            if lls and pt == lls[-1]:
                continue
            lls.append(pt)
            piece_of.append(k)
    xz = [project(a, b) for a, b in lls]
    s = [0.0]
    for (x0, z0), (x1, z1) in zip(xz, xz[1:]):
        s.append(s[-1] + math.hypot(x1 - x0, z1 - z0))
    total = s[-1]
    if total < 1.0 or len(xz) < 2:
        return None
    n = int(math.ceil(total / SPACING)) + 1
    sn = [total * i / (n - 1) for i in range(n)]
    xs, zs, pidx = [], [], []
    j = 0
    for t in sn:
        while j < len(s) - 2 and s[j + 1] < t:
            j += 1
        seg = s[j + 1] - s[j] or 1.0
        f = min(max((t - s[j]) / seg, 0.0), 1.0)
        xs.append(xz[j][0] + (xz[j + 1][0] - xz[j][0]) * f)
        zs.append(xz[j][1] + (xz[j + 1][1] - xz[j][1]) * f)
        pidx.append(piece_of[j] if f < 0.5 else piece_of[min(j + 1, len(piece_of) - 1)])
    ox, oz = list(xs), list(zs)
    px, pz = list(xs), list(zs)
    for _ in range(6):                       # Laplacian smoothing with displacement clamp
        nx, nz = list(px), list(pz)
        for i in range(1, n - 1):
            nx[i] = 0.5 * px[i] + 0.25 * (px[i - 1] + px[i + 1])
            nz[i] = 0.5 * pz[i] + 0.25 * (pz[i - 1] + pz[i + 1])
        px, pz = nx, nz
        for i in range(1, n - 1):
            dx, dz = px[i] - ox[i], pz[i] - oz[i]
            d = math.hypot(dx, dz)
            if d > SMOOTH_MAX:
                px[i] = ox[i] + dx * SMOOTH_MAX / d
                pz[i] = oz[i] + dz * SMOOTH_MAX / d
    return {"x": px, "z": pz, "s": sn, "piece": pidx}


def solve_profile(t, ds, a, b, g, sweeps=10):
    """Gradient-limited profile closest to target t with pinned ends a, b."""
    n = len(t)
    y = list(t)
    y[0], y[-1] = a, b
    for _ in range(sweeps):
        for i in range(1, n):
            lim = g * ds[i - 1]
            if y[i] < y[i - 1] - lim:
                y[i] = y[i - 1] - lim
            elif y[i] > y[i - 1] + lim:
                y[i] = y[i - 1] + lim
        for i in range(n - 2, -1, -1):
            lim = g * ds[i]
            if y[i] < y[i + 1] - lim:
                y[i] = y[i + 1] - lim
            elif y[i] > y[i + 1] + lim:
                y[i] = y[i + 1] + lim
        y[0], y[-1] = a, b
    worst = 0.0
    for i in range(1, n):
        worst = max(worst, abs(y[i] - y[i - 1]) / max(ds[i - 1], 1e-6) - g)
    return y, worst


def moving_average(v, half=3):
    out = []
    n = len(v)
    for i in range(n):
        lo, hi = max(0, i - half), min(n, i + half + 1)
        out.append(sum(v[lo:hi]) / (hi - lo))
    return out


# ---------------------------------------------------------------- directions

def chain_dir(route, xs, zs):
    if route in ("C1", "C2", "C3", "C4"):
        sweep = 0.0
        for i in range(1, len(xs)):
            a0 = math.atan2(zs[i - 1], xs[i - 1])
            a1 = math.atan2(zs[i], xs[i])
            d = a1 - a0
            while d > math.pi:
                d -= 2 * math.pi
            while d < -math.pi:
                d += 2 * math.pi
            sweep += d
        return "CW" if sweep > 0 else "CCW"
    if route == "B":
        dx, dz = xs[-1] - xs[0], zs[-1] - zs[0]
        if abs(dx) >= abs(dz):
            return "E" if dx > 0 else "W"
        return "S" if dz > 0 else "N"
    d0 = math.hypot(xs[0], zs[0])
    d1 = math.hypot(xs[-1], zs[-1])
    return "IN" if d1 < d0 else "OUT"


def seq_key(route, xs, zs):
    if route in ("C1", "C2", "C3", "C4"):
        return math.atan2(zs[0], xs[0]) % (2 * math.pi)
    if route == "B":
        return xs[0]
    return math.hypot(xs[0], zs[0])


# ---------------------------------------------------------------- crossings

def seg_intersect(p1, p2, p3, p4):
    d = (p2[0] - p1[0]) * (p4[1] - p3[1]) - (p2[1] - p1[1]) * (p4[0] - p3[0])
    if abs(d) < 1e-9:
        return None
    u = ((p3[0] - p1[0]) * (p4[1] - p3[1]) - (p3[1] - p1[1]) * (p4[0] - p3[0])) / d
    v = ((p3[0] - p1[0]) * (p2[1] - p1[1]) - (p3[1] - p1[1]) * (p2[0] - p1[0])) / d
    if 0 <= u <= 1 and 0 <= v <= 1:
        return u, v
    return None


def find_crossings(cars, geo):
    """Plan-view crossings between different carriageways. Returns list of dicts."""
    cell = 50.0
    grid = collections.defaultdict(list)
    for ci, c in enumerate(cars):
        g = geo[ci]
        for j in range(len(g["x"]) - 1):
            x0, x1 = sorted((g["x"][j], g["x"][j + 1]))
            z0, z1 = sorted((g["z"][j], g["z"][j + 1]))
            for cx in range(int(math.floor(x0 / cell)), int(math.floor(x1 / cell)) + 1):
                for cz in range(int(math.floor(z0 / cell)), int(math.floor(z1 / cell)) + 1):
                    grid[(cx, cz)].append((ci, j))
    seen = set()
    out = []
    for items in grid.values():
        for a in range(len(items)):
            ca, ja = items[a]
            for b in range(a + 1, len(items)):
                cb, jb = items[b]
                if ca == cb:
                    continue
                key = (min(ca, cb), max(ca, cb), min(ja, jb), max(ja, jb))
                if key in seen:
                    continue
                seen.add(key)
                ga, gb = geo[ca], geo[cb]
                p1 = (ga["x"][ja], ga["z"][ja]); p2 = (ga["x"][ja + 1], ga["z"][ja + 1])
                p3 = (gb["x"][jb], gb["z"][jb]); p4 = (gb["x"][jb + 1], gb["z"][jb + 1])
                hit = seg_intersect(p1, p2, p3, p4)
                if not hit:
                    continue
                # Near-parallel pairs are coincident carriageways, not grade crossings.
                ax, az, bx, bz = p2[0] - p1[0], p2[1] - p1[1], p4[0] - p3[0], p4[1] - p3[1]
                cosang = abs(ax * bx + az * bz) / max(math.hypot(ax, az) * math.hypot(bx, bz), 1e-9)
                if cosang > math.cos(math.radians(15.0)):
                    continue
                u, v = hit
                px = p1[0] + (p2[0] - p1[0]) * u
                pz = p1[1] + (p2[1] - p1[1]) * u
                ya = ga["y"][ja] + (ga["y"][ja + 1] - ga["y"][ja]) * u
                yb = gb["y"][jb] + (gb["y"][jb + 1] - gb["y"][jb]) * v
                # Ignore junction contact at a shared node.
                shared = {cars[ca]["from"], cars[ca]["to"]} & {cars[cb]["from"], cars[cb]["to"]}
                if shared:
                    near = False
                    for n in shared:
                        nx, nz = geo_node_xz(cars, geo, n)
                        if nx is not None and math.hypot(px - nx, pz - nz) < 30.0:
                            near = True
                    if near:
                        continue
                out.append({"a": ca, "b": cb, "ja": ja, "jb": jb, "x": px, "z": pz,
                            "ya": ya, "yb": yb, "dy": ya - yb})
    return out


def geo_node_xz(cars, geo, node):
    for ci, c in enumerate(cars):
        if c["from"] == node:
            return geo[ci]["x"][0], geo[ci]["z"][0]
        if c["to"] == node:
            return geo[ci]["x"][-1], geo[ci]["z"][-1]
    return None, None


# ---------------------------------------------------------------- connectivity

def scc_kosaraju(nodes, adj, radj):
    order, seen = [], set()
    for s in nodes:
        if s in seen:
            continue
        stack = [(s, iter(adj.get(s, ())))]
        seen.add(s)
        while stack:
            v, it = stack[-1]
            nxt = next(it, None)
            if nxt is None:
                order.append(v)
                stack.pop()
            elif nxt not in seen:
                seen.add(nxt)
                stack.append((nxt, iter(adj.get(nxt, ()))))
    comp = {}
    cid = 0
    for s in reversed(order):
        if s in comp:
            continue
        stack = [s]
        comp[s] = cid
        while stack:
            v = stack.pop()
            for w in radj.get(v, ()):
                if w not in comp:
                    comp[w] = cid
                    stack.append(w)
        cid += 1
    return comp


def bfs(starts, adj):
    seen = set(starts)
    dq = collections.deque(starts)
    while dq:
        v = dq.popleft()
        for w in adj.get(v, ()):
            if w not in seen:
                seen.add(w)
                dq.append(w)
    return seen


# ---------------------------------------------------------------- main

# ---------------------------------------------------------------- v2 additions

PA_NAME_RE = re.compile(r"(PA|\u30d1\u30fc\u30ad\u30f3\u30b0\u30a8\u30ea\u30a2)$")
PRIORITY_XZ = (-2600.0, 1500.0, 0.0, 6000.0)   # x0, z0, x1, z1 (Ginza / Shibaura / Rainbow area)
SNAP_TOL = 8.0
APPROACH_M = 200.0       # pin cone reach along the network (direct approaches only)
ELEV_MIN_CLEAR = 5.5    # minimum elevated deck above ground (over streets)
# Height pins: deck heights (TP metres) for named bridge spans. bbox = (lat0, lon0, lat1, lon1).
# The solver climbs toward the pin at GRADE_MAIN from the nearest network distance.
HEIGHT_PINS = [
    {"name": "Rainbow Bridge upper deck", "routes": ["11"], "y": 52.0,
     "bbox": (35.6320, 139.7480, 35.6420, 139.7680),
     "source": "Wikipedia 'Rainbow Bridge (Tokyo)' infobox: roadway 52 m (accessed 2026-10-10). "
               "Navigation clearance not verified."},
    {"name": "Yokohama Bay Bridge upper deck", "routes": ["B"], "y": 55.0,
     "bbox": (35.4380, 139.6600, 35.4620, 139.6850),
     "source": "Wikipedia 'Yokohama Bay Bridge': clearance 55 m (accessed 2026-10-10); "
               "upper/lower deck attribution unverified. Bbox approximate."},
    {"name": "Tsurumi Tsubasa Bridge", "routes": ["B"], "y": 40.0,
     "bbox": (35.4620, 139.6900, 35.4800, 139.7200),
     "source": "UNVERIFIED: no height found in 2026-10-10 search; retained prior approximate value. "
               "Bbox approximate."},
]


def fetch_pa_service(fac, refresh):
    """Service roads / parking aisles within 90 m of each Shuto PA candidate (cached)."""
    centers = []
    for el in fac.get("elements", []):
        t = el.get("tags", {})
        name = t.get("name", "")
        if not (t.get("highway") in ("services", "rest_area")
                or (t.get("amenity") == "parking" and PA_NAME_RE.search(name))):
            continue
        if el["type"] == "node":
            centers.append((el["lat"], el["lon"]))
        elif el.get("geometry"):
            g = el["geometry"]
            centers.append((sum(p["lat"] for p in g) / len(g), sum(p["lon"] for p in g) / len(g)))
    q = "[out:json][timeout:300];\n("
    q += "".join('way["highway"~"^(service|parking_aisle)$"](around:90,%f,%f);' % (la, lo)
                 for la, lo in centers)
    q += ");\nout body geom;"
    return fetch_overpass("overpass_pa_service", q, refresh)


def snap_endpoints(cars, geo, tol=SNAP_TOL, dy_max=4.0):
    """Join dead-end endpoints within tol metres, head to tail, at compatible heights.
    Chain keys are renamed so both carriageways share one node; endpoints move to the midpoint."""
    cnt = collections.Counter()
    for c in cars:
        cnt[c["from_key"]] += 1
        cnt[c["to_key"]] += 1
    ends = []
    for ci, c in enumerate(cars):
        g = geo[ci]
        for side, idx in (("from", 0), ("to", -1)):
            kind = c[side + "_kind"]
            if kind and kind[0] == "boundary" and kind[2] == "city":
                continue                      # ramp ends at a street stay boundaries
            if cnt[c[side + "_key"]] != 1:
                continue
            j = 1 if idx == 0 else -2
            ux, uz = g["x"][idx] - g["x"][j], g["z"][idx] - g["z"][j]
            n = math.hypot(ux, uz) or 1.0
            ends.append({"ci": ci, "side": side, "idx": idx, "x": g["x"][idx], "z": g["z"][idx],
                         "ux": ux / n, "uz": uz / n, "y": g["target"][idx]})
    pairs = []
    for i in range(len(ends)):
        for j in range(i + 1, len(ends)):
            a, b = ends[i], ends[j]
            if a["ci"] == b["ci"]:
                continue
            d = math.hypot(a["x"] - b["x"], a["z"] - b["z"])
            if d < 3 * tol:
                pairs.append((d, i, j))
    pairs.sort()
    used, merged, near = set(), [], []
    for d, i, j in pairs:
        if i in used or j in used:
            continue
        a, b = ends[i], ends[j]
        if a["ux"] * b["ux"] + a["uz"] * b["uz"] > -0.5:   # must face each other head to tail
            continue
        label = "way %s / way %s" % (cars[a["ci"]]["pieces"][0]["way"], cars[b["ci"]]["pieces"][0]["way"])
        if d >= tol or abs(a["y"] - b["y"]) > dy_max:
            near.append({"ways": label, "dist_m": round(d, 1), "dy_m": round(a["y"] - b["y"], 2)})
            continue
        ka = cars[a["ci"]][a["side"] + "_key"]
        kb = cars[b["ci"]][b["side"] + "_key"]
        for c in cars:
            if c["from_key"] == kb:
                c["from_key"] = ka
            if c["to_key"] == kb:
                c["to_key"] = ka
        mx, mz = (a["x"] + b["x"]) / 2, (a["z"] + b["z"]) / 2
        for e in (a, b):
            cars[e["ci"]][e["side"] + "_kind"] = None
            geo[e["ci"]]["x"][e["idx"]] = mx
            geo[e["ci"]]["z"][e["idx"]] = mz
        used.update((i, j))
        merged.append({"ways": label, "dist_m": round(d, 2), "dy_m": round(a["y"] - b["y"], 2),
                       "xz": [round(mx, 1), round(mz, 1)]})
    log("endpoint snaps: %d merged, %d near misses within %.0f m" % (len(merged), len(near), 3 * tol))
    return {"merged": merged, "near_misses": near[:300]}


def apply_height_pins(cars, geo, segs):
    """Raise targets to each pin's deck height; approaches follow a cone at GRADE_MAIN."""
    import heapq
    records = []
    for pin in HEIGHT_PINS:
        la0, lo0, la1, lo1 = pin["bbox"]
        xa, za = project(la0, lo0)
        xb, zb = project(la1, lo1)
        x0, x1, z0, z1 = min(xa, xb), max(xa, xb), min(za, zb), max(za, zb)
        Y, G = pin["y"], GRADE_MAIN
        pinned = {}
        for ci, c in enumerate(cars):
            if segs[c["segs"][0]]["route"] not in pin["routes"]:
                continue
            g = geo[ci]
            inside = [i for i in range(len(g["x"]))
                      if x0 <= g["x"][i] <= x1 and z0 <= g["z"][i] <= z1 and g["type"][i] == "elevated"]
            if inside:
                pinned[ci] = (g["s"][inside[0]], g["s"][inside[-1]])
        for ci_p in pinned:
            geo[ci_p]["pinned"] = True
        if not pinned:
            records.append({"pin": pin["name"], "status": "no matching carriageway", "source": pin["source"]})
            continue
        adj = collections.defaultdict(list)
        for ci, c in enumerate(cars):
            L = geo[ci]["s"][-1]
            adj[c["from_key"]].append((c["to_key"], L))
            adj[c["to_key"]].append((c["from_key"], L))
        dist, pq, seq = {}, [], 0
        for ci, (s0, s1) in pinned.items():
            L = geo[ci]["s"][-1]
            for key, d in ((cars[ci]["from_key"], s0), (cars[ci]["to_key"], L - s1)):
                if d < dist.get(key, 1e18):
                    dist[key] = d
                    heapq.heappush(pq, (d, seq, key))
                    seq += 1
        cutoff = APPROACH_M
        while pq:
            d, _, key = heapq.heappop(pq)
            if d > dist.get(key, 1e18) or d > cutoff:
                continue
            for nxt, L in adj[key]:
                nd = d + L
                if nd < dist.get(nxt, 1e18):
                    dist[nxt] = nd
                    heapq.heappush(pq, (nd, seq, nxt))
                    seq += 1
        for ci, c in enumerate(cars):
            g = geo[ci]
            s, L = g["s"], g["s"][-1]
            da, db = dist.get(c["from_key"], 1e18), dist.get(c["to_key"], 1e18)
            for i in range(len(s)):
                if ci in pinned:
                    s0, s1 = pinned[ci]
                    if s0 <= s[i] <= s1 and g["type"][i] == "elevated":
                        g["target"][i] = Y
                        continue
                    dpt = s0 - s[i] if s[i] < s0 else (s[i] - s1 if s[i] > s1 else 0.0)
                else:
                    dpt = min(da + s[i], db + (L - s[i]))
                if dpt > cutoff:
                    continue
                g["target"][i] = max(g["target"][i], Y - G * dpt)
        records.append({"pin": pin["name"], "y_target": Y, "pinned_carriageways": len(pinned),
                        "cone_nodes": len(dist), "source": pin["source"]})
        log("height pin '%s': %d pinned carriageways, cone over %d nodes" % (pin["name"], len(pinned), len(dist)))
    return records


def point_rank(c, g, i):
    """Vertical rank of the road at point i: OSM layer, else bridge=1, tunnel=-1, else 0."""
    t = c["pieces"][g["piece"][i]]["tags"]
    layer = parse_int(t.get("layer"), 0)
    if layer:
        return layer
    if t.get("bridge", "no") not in ("no", ""):
        return 1
    if t.get("tunnel", "no") not in ("no", "") or t.get("location") == "underground":
        return -1
    return 0


def plan_bank(c, g, multi_keys):
    """Bank (rad) from plan curvature: e = clamp(v^2/(127 R) - 0.1, 0, 0.06), bank = atan(e),
    positive = left side up (left-hand curve). Zero within 30 m of merge/diverge ends."""
    x, z, s = g["x"], g["z"], g["s"]
    n = len(x)
    if n < 3:
        return [0.0] * n
    hd = []
    for i in range(n - 1):
        dx, dz = x[i + 1] - x[i], z[i + 1] - z[i]
        L = math.hypot(dx, dz) or 1e-9
        hd.append((dx / L, dz / L))
    kap = [0.0] * n
    for i in range(1, n - 1):
        ax, az = hd[i - 1]
        bx, bz = hd[i]
        ang = math.atan2(abs(ax * bz - az * bx), ax * bx + az * bz)
        sign = 1.0 if (bx - ax) * az + (bz - az) * (-ax) > 0 else -1.0   # left = (az, -ax)
        ds = (s[i + 1] - s[i - 1]) / 2.0 or 1.0
        kap[i] = sign * ang / ds
    kap = moving_average(kap, 2)
    v = c["speed_kmh"]
    bank = []
    for i in range(n):
        k = abs(kap[i])
        if k < 1e-6:
            bank.append(0.0)
            continue
        R = 1.0 / k
        e = min(max(v * v / (127.0 * R) - 0.1, 0.0), 0.06)
        bank.append((1.0 if kap[i] > 0 else -1.0) * math.atan(e))
    bank = moving_average(bank, 2)
    L = s[-1]
    if c["from_key"] in multi_keys:
        bank = [0.0 if s[i] < 30.0 else bank[i] for i in range(n)]
    if c["to_key"] in multi_keys:
        bank = [0.0 if s[i] > L - 30.0 else bank[i] for i in range(n)]
    return [round(b, 5) for b in bank]


def priority_table(cars, geo):
    """Per-carriageway rows for the priority bbox plus node-height consistency check."""
    x0, z0, x1, z1 = PRIORITY_XZ
    rows = []
    for ci, c in enumerate(cars):
        g = geo[ci]
        if not any(x0 <= x <= x1 and z0 <= z <= z1 for x, z in zip(g["x"], g["z"])):
            continue
        rows.append({"id": c["id"], "route": c["route"], "kind": c["kind"], "lanes": c["lanes"],
                     "length_m": round(g["s"][-1], 1), "y_min": round(min(g["y"]), 2),
                     "y_max": round(max(g["y"]), 2),
                     "structures": ["%s %.0f-%.0f" % (r["type"], r["s0"], r["s1"]) for r in c["structure"]]})
    ends = collections.defaultdict(list)
    for ci, c in enumerate(cars):
        ends[c["from_key"]].append(geo[ci]["y"][0])
        ends[c["to_key"]].append(geo[ci]["y"][-1])
    spread = max((max(v) - min(v) for v in ends.values()), default=0.0)
    return rows, round(spread, 4)


def point_in_poly(x, z, poly):
    inside = False
    n = len(poly)
    for i in range(n):
        x1_, z1_ = poly[i]
        x2_, z2_ = poly[(i + 1) % n]
        if (z1_ > z) != (z2_ > z) and x < (x2_ - x1_) * (z - z1_) / ((z2_ - z1_) or 1e-9) + x1_:
            inside = not inside
    return inside


def subdivide_xz(pts, spacing=10.0):
    out = [pts[0]]
    for (ax, az), (bx, bz) in zip(pts, pts[1:]):
        L = math.hypot(bx - ax, bz - az)
        k = max(1, int(math.ceil(L / spacing)))
        for m in range(1, k + 1):
            f = m / k
            out.append((ax + (bx - ax) * f, az + (bz - az) * f))
    return out


def build_facilities(fac, car_out, pa_service):
    """Shuto facilities mapped to the nearest carriageway point. PAs must lie within 60 m of a
    Shuto carriageway. Returns (facilities, lot_carriageways, lot_nodes)."""
    import numpy as np
    rows = []
    for ci, c in enumerate(car_out):
        for p in c["points"]:
            rows.append((p[0], p[2], p[1], ci))
    P = np.array(rows, dtype=float)
    facilities, roads, nodes = [], [], []
    outlines = []
    for el in fac.get("elements", []):
        tags = el.get("tags", {})
        name = tags.get("name", "")
        hw = tags.get("highway", "")
        if el["type"] == "node":
            la, lo, geom = el.get("lat"), el.get("lon"), None
        else:
            geom = el.get("geometry") or []
            if not geom:
                continue
            la = sum(p["lat"] for p in geom) / len(geom)
            lo = sum(p["lon"] for p in geom) / len(geom)
        if la is None or lo is None:
            continue
        if tags.get("barrier") == "toll_booth":
            ftype, maxd = "toll", 150.0
        elif re.search(r"(\u30b3\u30a4\u30f3|COIN|Park|\u30d1\u30fc\u30ad\u30f3\u30b0)", name) \
                and not PA_NAME_RE.search(name):
            continue                              # street coin car parks, not Shuto PAs
        elif hw in ("services", "rest_area") or (tags.get("amenity") == "parking" and PA_NAME_RE.search(name)):
            ftype, maxd = "pa", 60.0
        elif hw == "motorway_junction":
            ftype = "jct" if ("JCT" in name.upper() or "\u30b8\u30e3\u30f3\u30af\u30b7\u30e7\u30f3" in name) else "ic"
            maxd = 150.0
        else:
            continue
        x, z = project(la, lo)
        d2 = (P[:, 0] - x) ** 2 + (P[:, 1] - z) ** 2
        k = int(np.argmin(d2))
        dist = math.sqrt(d2[k])
        if dist > maxd:
            continue
        ci, py = int(P[k, 3]), float(P[k, 2])
        fid = "%s.%s%s" % (ftype.upper(), el["type"][0], el["id"])
        rec = {"id": fid, "type": ftype, "name_en": ascii_or_empty(tags.get("name:en", "")),
               "name_ja": name, "pos": [round(x, 3), round(py, 3), round(z, 3)],
               "carriageways": [car_out[ci]["id"]],
               "notes": "OSM %s %s; nearest Shuto carriageway %.1f m" % (el["type"], el["id"], dist)}
        if geom:
            poly = [project(p["lat"], p["lon"]) for p in geom]
            rec["outline"] = [[round(X, 3), round(py, 3), round(Z, 3)] for X, Z in poly]
            outlines.append((len(facilities), poly, py, fid))
        facilities.append(rec)
    # PA lot roads: service ways whose midpoint lies inside a PA outline.
    service = [w for w in pa_service.get("elements", []) if w.get("type") == "way" and w.get("geometry")]
    used = set()
    for fi, poly, py, fid in outlines:
        k = 0
        for w in service:
            if w["id"] in used:
                continue
            g = w["geometry"]
            mx = sum(p["lat"] for p in g) / len(g)
            my = sum(p["lon"] for p in g) / len(g)
            mxz = project(mx, my)
            if not point_in_poly(mxz[0], mxz[1], poly):
                continue
            used.add(w["id"])
            pts = subdivide_xz([project(p["lat"], p["lon"]) for p in g])
            k += 1
            pidx = len(roads) + 1
            a, b = "PA%03d.A" % pidx, "PA%03d.B" % pidx
            cid = "PA.LOT%03d.%02d" % (pidx, k)
            L = sum(math.hypot(q[0] - p[0], q[1] - p[1]) for p, q in zip(pts, pts[1:]))
            xyz = [[round(X, 3), round(py, 3), round(Z, 3)] for X, Z in pts]
            nodes.append({"id": a, "pos": xyz[0], "kind": "end", "exit": None})
            nodes.append({"id": b, "pos": xyz[-1], "kind": "end", "exit": None})
            roads.append({"id": cid, "route": "PA", "dir": "LOT", "kind": "pa", "from": a, "to": b,
                          "lanes": 1, "lane_width": 3.0, "shoulder_left": 0.5, "shoulder_right": 0.5,
                          "speed_kmh": 15, "name_en": "%s lot road %d" % (fid, k),
                          "name_ja": w.get("tags", {}).get("name", ""), "points": xyz,
                          "structure": [{"s0": 0.0, "s1": round(L, 1), "type": "at_grade"}],
                          "osm_ways": [w["id"]], "bank": [0.0] * len(xyz)})
            facilities[fi]["carriageways"].append(cid)
    return facilities, roads, nodes


def envelope(a, b, gr, s):
    """Feasible band for a profile with |slope| <= gr between pinned ends a (s=0) and b (s=L)."""
    L = s[-1]
    lo = [max(a - gr * si, b - gr * (L - si)) for si in s]
    hi = [min(a + gr * si, b + gr * (L - si)) for si in s]
    return lo, hi


def grade_stats(g):
    """(max |grade|, max grade change per 10 m) of a solved profile."""
    s, y = g["s"], g["y"]
    segs_ = [(s[i] - s[i - 1], (y[i] - y[i - 1]) / (s[i] - s[i - 1]))
             for i in range(1, len(s)) if s[i] - s[i - 1] > 2.0]
    maxg = max((abs(gr) for _, gr in segs_), default=0.0)
    ab = 0.0
    for i in range(1, len(segs_)):
        ds = 0.5 * (segs_[i - 1][0] + segs_[i][0])
        ab = max(ab, abs(segs_[i][1] - segs_[i - 1][1]) * 10.0 / ds)
    return maxg, ab


def find_overlaps(cars, geo, dist=8.0, junction=40.0):
    """Plan-view overlaps (< dist m) of carriageways at different heights (stacked decks).
    Pairs meeting at a shared junction node (within junction m) are ignored."""
    grid = collections.defaultdict(list)
    for ci, g in enumerate(geo):
        for i, (x, z) in enumerate(zip(g["x"], g["z"])):
            grid[(int(math.floor(x / dist)), int(math.floor(z / dist)))].append((ci, i))
    out = []
    for (cx, cz), items in grid.items():
        near = []
        for ddx in (-1, 0, 1):
            for ddz in (-1, 0, 1):
                near.extend(grid.get((cx + ddx, cz + ddz), []))
        for ci, i in items:
            g = geo[ci]
            x, z, y = g["x"][i], g["z"][i], g["y"][i]
            for cj, j in near:
                if cj <= ci:
                    continue
                h = geo[cj]
                d = math.hypot(x - h["x"][j], z - h["z"][j])
                if d >= dist:
                    continue
                dy = y - h["y"][j]
                if abs(dy) >= CLEAR or abs(dy) < 0.5:
                    continue
                shared = {cars[ci]["from_key"], cars[ci]["to_key"]} & {cars[cj]["from_key"], cars[cj]["to_key"]}
                if shared:
                    continue          # adjacent carriageways (share a node) are not stacked decks
                skip = False
                for k in shared:
                    nx, nz = ((g["x"][0], g["z"][0]) if cars[ci]["from_key"] == k
                              else (g["x"][-1], g["z"][-1]))
                    if math.hypot(x - nx, z - nz) < junction:
                        skip = True
                if skip:
                    continue
                out.append({"kind": "overlap", "a": ci, "b": cj, "ja": i, "jb": j, "x": x, "z": z,
                            "ya": y, "yb": h["y"][j], "dy": dy})
    return out


def chain_half_width(c, ramp):
    """Half width of the travelled lanes plus the wider shoulder (m)."""
    pieces = c["pieces"]
    lanes = max(1, sorted(default_lanes(p["tags"], p["link"]) for p in pieces)[len(pieces) // 2])
    return lanes * lane_width(pieces[0]["tags"], c["route"], ramp) / 2.0 + (0.5 if ramp else 0.75)


def _tangent(g, i):
    n = len(g["x"])
    a, b = max(0, i - 1), min(n - 1, i + 1)
    dx, dz = g["x"][b] - g["x"][a], g["z"][b] - g["z"][a]
    L = math.hypot(dx, dz) or 1.0
    return dx / L, dz / L


def overlap_pairs(cars, geo, cell=10.0):
    """Sample pairs of different carriageways closer than the sum of their half widths
    (+0.5 m). Tuple: (ci, i, cj, j, same_direction, plan_distance, half_width_sum)."""
    hw = [chain_half_width(cars[ci], geo[ci]["ramp"]) for ci in range(len(cars))]
    grid = collections.defaultdict(list)
    for ci, g in enumerate(geo):
        for i, (x, z) in enumerate(zip(g["x"], g["z"])):
            grid[(int(math.floor(x / cell)), int(math.floor(z / cell)))].append((ci, i))
    out = []
    for (cx, cz), items in grid.items():
        near = []
        for ddx in (-1, 0, 1):
            for ddz in (-1, 0, 1):
                near.extend(grid.get((cx + ddx, cz + ddz), []))
        for ci, i in items:
            g = geo[ci]
            x, z = g["x"][i], g["z"][i]
            for cj, j in near:
                if cj <= ci:
                    continue
                h = geo[cj]
                hs = hw[ci] + hw[cj]
                d = math.hypot(x - h["x"][j], z - h["z"][j])
                if d >= hs + 0.5:
                    continue
                ta, tb = _tangent(g, i), _tangent(h, j)
                dot = ta[0] * tb[0] + ta[1] * tb[1]
                if abs(dot) < 0.5:
                    continue                  # crossing at an angle: handled by the crossing rules
                out.append((ci, i, cj, j, dot > 0, d, hs))
    return out


def _near_shared_node(cars, geo, ci, cj, x, z, r=40.0):
    shared = {cars[ci]["from_key"], cars[ci]["to_key"]} & {cars[cj]["from_key"], cars[cj]["to_key"]}
    for k in shared:
        if cars[ci]["from_key"] == k:
            nx, nz = geo[ci]["x"][0], geo[ci]["z"][0]
        else:
            nx, nz = geo[ci]["x"][-1], geo[ci]["z"][-1]
        if math.hypot(x - nx, z - nz) < r:
            return True
    return False


def couple_same_direction(cars, geo, pairs, damp=0.8):
    """Pull overlapping same-direction decks towards a common height (|dy| <= 0.25 m)."""
    acc = collections.defaultdict(lambda: [0.0, 0])
    bad = 0
    for ci, i, cj, j, same, d, hs in pairs:
        if not same:
            continue
        dy = geo[ci]["y"][i] - geo[cj]["y"][j]
        if abs(dy) <= 0.25:
            continue
        bad += 1
        for c_, k_, v in ((ci, i, -dy / 2.0), (cj, j, dy / 2.0)):
            acc[(c_, k_)][0] += v
            acc[(c_, k_)][1] += 1
    for (c_, k_), (sm, n) in acc.items():
        geo[c_]["extra"][k_] += damp * sm / n
    return bad


def separate_opposite(cars, geo, pairs, clear_extra=0.5, damp=0.8):
    """Split-level opposite decks closer than the half-width sum + 0.5 m: shift the centrelines
    apart symmetrically (interior points only, junctions excluded). Returns pair count."""
    acc = collections.defaultdict(lambda: [0.0, 0.0, 0])
    n_sep = 0
    for ci, i, cj, j, same, d, hs in pairs:
        if same:
            continue
        g, h = geo[ci], geo[cj]
        if i in (0, len(g["x"]) - 1) or j in (0, len(h["x"]) - 1):
            continue
        if _near_shared_node(cars, geo, ci, cj, g["x"][i], g["z"][i]):
            continue
        deficit = hs + clear_extra - d
        if deficit <= 0 or d < 1e-6:
            continue
        nx, nz = (g["x"][i] - h["x"][j]) / d, (g["z"][i] - h["z"][j]) / d
        for c_, k_, sg in ((ci, i, 1.0), (cj, j, -1.0)):
            a = acc[(c_, k_)]
            a[0] += sg * 0.5 * deficit * nx
            a[1] += sg * 0.5 * deficit * nz
            a[2] += 1
        n_sep += 1
    for (c_, k_), (sx, sz, n) in acc.items():
        geo[c_]["x"][k_] += damp * sx / n
        geo[c_]["z"][k_] += damp * sz / n
    return n_sep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true", help="re-download OSM data")
    args = ap.parse_args()
    RAW.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    t_start = time.time()
    today = datetime.date.today().isoformat()

    ex, ey, d_true = unproject_check()
    log("frame check: 1000 m east of origin -> x=%.2f z=%s (expect 1000.00, 0.00)" % (ex, ez_fmt(ey)))

    motor = fetch_overpass("overpass_motorway", Q_MOTORWAY, args.refresh)
    fac = fetch_overpass("overpass_facilities", Q_FACILITIES, args.refresh)
    pa_service = fetch_pa_service(fac, args.refresh)
    ways = load_ways(motor)
    log(f"ways: {len(ways)}")

    (segs, ins, outs, topo, raw_chains, excluded, foreign_ways,
     dropped_nodes, node_ways, ways) = build_graph(ways)
    chains = make_chains(segs, ins, outs, topo, raw_chains, foreign_ways, dropped_nodes, node_ways, ways)
    log(f"shuto segments: {len(segs)}, chains: {len(chains)}, foreign ways near: {len(foreign_ways)}")

    cars, geo = [], []
    for ci, ch in enumerate(chains):
        ch["from_key"] = ("ext", ci, "from") if ch["ext_from"] else ch["from"]
        ch["to_key"] = ("ext", ci, "to") if ch["ext_to"] else ch["to"]
        g = resample(ch)
        if g is None:
            continue
        cars.append(ch)
        geo.append(g)
    log(f"carriageways after resampling: {len(cars)}")

    # Elevation targets per point (needed by endpoint snapping and height pins).
    for ci, c in enumerate(cars):
        g = geo[ci]
        pieces = c["pieces"]
        cls_cache, cls = {}, []
        for k in g["piece"]:
            if k not in cls_cache:
                p = pieces[k]
                cls_cache[k] = classify(p["tags"], p["link"])
            cls.append(cls_cache[k])
        g["type"] = [cl[0] for cl in cls]
        g["ramp"] = sum(1 for p in g["piece"] if pieces[p]["link"]) > len(g["piece"]) / 2
        g["target"] = moving_average([cl[1] for cl in cls], 6)   # long transitions: smooth structure steps
        g["extra"] = [0.0] * len(cls)
        g["sweep_ds"] = [g["s"][i + 1] - g["s"][i] for i in range(len(g["s"]) - 1)]

    snaps = snap_endpoints(cars, geo)

    # Node kinds, ids and merge/diverge counts (after snapping).
    deg = collections.Counter()
    for c in cars:
        deg[c["from_key"]] += 1
        deg[c["to_key"]] += 1
    node_kind = {}
    for c in cars:
        for key, kind in ((c["from_key"], c["from_kind"]), (c["to_key"], c["to_kind"])):
            if isinstance(key, tuple):
                node_kind[key] = ("boundary", kind)
            elif kind is not None and kind[0] in ("boundary", "end"):
                node_kind[key] = kind
            elif key in topo or deg[key] > 1:
                node_kind.setdefault(key, ("topo", None))
    nid = {}
    for c in cars:
        for key in (c["from_key"], c["to_key"]):
            if key not in nid:
                nid[key] = f"N{len(nid) + 1:06d}"
    ends_in = collections.Counter()
    ends_out = collections.Counter()
    for c in cars:
        ends_out[c["from_key"]] += 1
        ends_in[c["to_key"]] += 1
    multi_keys = {k for k in set(ends_in) | set(ends_out) if ends_in[k] + ends_out[k] > 2}

    pins = apply_height_pins(cars, geo, segs)

    # ---- global height solve: node heights (grade-limited) + per-carriageway profiles
    def global_solve():
        vals = collections.defaultdict(list)
        for ci, c in enumerate(cars):
            g = geo[ci]
            vals[c["from_key"]].append(g["target"][0] + g["extra"][0])
            vals[c["to_key"]].append(g["target"][-1] + g["extra"][-1])
        h = {k: sum(v) / len(v) for k, v in vals.items()}
        fixed, floor = set(), {}
        for ci, c in enumerate(cars):
            g = geo[ci]
            if g.get("pinned"):
                pass   # pinned spans set interior heights; their ends relax with the network
            for key, typ in ((c["from_key"], g["type"][0]), (c["to_key"], g["type"][-1])):
                if typ == "elevated":
                    floor[key] = max(floor.get(key, -1e9), GROUND_Y + ELEV_MIN_CLEAR)
        infeasible = 0
        for _ in range(400):
            delta = collections.defaultdict(float)
            cnt = collections.Counter()
            worst = 0.0
            for ci, c in enumerate(cars):
                g = geo[ci]
                gr = GRADE_RAMP if g["ramp"] else GRADE_MAIN
                a, b = c["from_key"], c["to_key"]
                D = h[b] - h[a]
                cap = gr * g["s"][-1] * 0.995
                if abs(D) <= cap:
                    continue
                ex = D - cap if D > 0 else D + cap       # height difference to remove
                fa, fb = a not in fixed, b not in fixed
                if fa and fb:
                    da, db = ex / 2.0, -ex / 2.0
                elif fa:
                    da, db = ex, 0.0
                elif fb:
                    da, db = 0.0, -ex
                else:
                    infeasible += 1
                    continue
                delta[a] += da
                cnt[a] += 1
                delta[b] += db
                cnt[b] += 1
                worst = max(worst, abs(ex))
            if worst < 0.01:
                break
            for k in delta:
                h[k] += delta[k] / cnt[k]
            for k, fl in floor.items():
                if k not in fixed and h[k] < fl:
                    h[k] = fl
        for ci, c in enumerate(cars):
            g = geo[ci]
            gr = GRADE_RAMP if g["ramp"] else GRADE_MAIN
            a, b = h[c["from_key"]], h[c["to_key"]]
            y = [x + z for x, z in zip(g["target"], g["extra"])]
            for _ in range(3):
                y, _w = solve_profile(y, g["sweep_ds"], a, b, gr)
                lo, hi = envelope(a, b, gr, g["s"])
                y = [min(max(v, lo_), hi_) for v, lo_, hi_ in zip(y, lo, hi)]
            for _ in range(8):                     # vertical-curve smoothing, kept inside the grade band
                y = moving_average(y, 2)
                lo, hi = envelope(a, b, gr, g["s"])
                y = [min(max(v, lo_), hi_) for v, lo_, hi_ in zip(y, lo, hi)]
            y[0], y[-1] = a, b
            g["y"] = y
        return infeasible

    infeas = global_solve()
    log("global height solve: %d carriageway(s) with both ends fixed beyond grade" % infeas)
    for _ in range(8):
        any_bump = False
        seen = set()
        for x in find_crossings(cars, geo) + find_overlaps(cars, geo):
            if abs(x["dy"]) >= CLEAR:
                continue
            key = (min(x["a"], x["b"]), max(x["a"], x["b"]), int(geo[x["a"]]["s"][x["ja"]] // 200))
            if key in seen:
                continue
            seen.add(key)
            if x.get("kind") == "overlap":
                if abs(x["dy"]) < 0.5:
                    x["status"] = "same_level_overlap"
                    continue
                up_is_a = x["ya"] > x["yb"]          # stacked: lift the higher deck
            else:
                ra = point_rank(cars[x["a"]], geo[x["a"]], x["ja"])
                rb = point_rank(cars[x["b"]], geo[x["b"]], x["jb"])
                ea = geo[x["a"]]["type"][x["ja"]] == "elevated"
                eb = geo[x["b"]]["type"][x["jb"]] == "elevated"
                if ra != rb:
                    up_is_a = ra > rb
                elif ea != eb:
                    up_is_a = ea
                elif abs(x["dy"]) >= 0.5:            # stacked decks of equal rank: lift the higher
                    up_is_a = x["ya"] > x["yb"]
                else:
                    x["status"] = "unresolved_same_level"
                    continue
            low, lidx = (x["b"], x["jb"]) if up_is_a else (x["a"], x["ja"])
            if geo[low]["type"][lidx] in ("cut", "tunnel"):
                up, idx, sign = low, lidx, -1.0      # shallow the cut/tunnel down instead
            else:
                up, idx = (x["a"], x["ja"]) if up_is_a else (x["b"], x["jb"])
                sign = 1.0
            need = CLEAR - abs(x["dy"]) + 0.3
            g = geo[up]
            s_c = g["s"][idx]
            for j in range(len(g["s"])):
                d = abs(g["s"][j] - s_c)
                if d < ELEV_CROSS_RANGE:
                    g["extra"][j] += sign * need * 0.5 * (1 + math.cos(math.pi * d / ELEV_CROSS_RANGE))
            x["status"] = "lifted"
            any_bump = True
        if not any_bump:
            break
        global_solve()

    # Overlapping same-direction decks: common height (|dy| <= 0.25 m). Opposite decks that
    # overlap at different heights: separated in plan by the half-width sum + 0.5 m.
    for c in cars:
        c.setdefault("route", segs[c["segs"][0]]["route"])
    for _ in range(40):
        pairs = overlap_pairs(cars, geo)
        n_sep = separate_opposite(cars, geo, pairs)
        n_same = couple_same_direction(cars, geo, pairs)
        if n_sep == 0 and n_same == 0:
            break
        global_solve()
    fin = overlap_pairs(cars, geo)
    bad_same = [p for p in fin if p[4] and abs(geo[p[0]]["y"][p[1]] - geo[p[2]]["y"][p[3]]) > 0.25]
    x0, z0, x1, z1 = PRIORITY_XZ
    bad_bbox = [p for p in bad_same
                if x0 <= geo[p[0]]["x"][p[1]] <= x1 and z0 <= geo[p[0]]["z"][p[1]] <= z1]
    ov_stats = {"same_dir_sample_pairs_dy_gt_0.25": len(bad_same),
                "same_dir_sample_pairs_dy_gt_0.25_in_priority_bbox": len(bad_bbox),
                "opposite_sample_pairs_within_half_width_sum": sum(1 for p in fin if not p[4])}
    log("overlap rules: %s" % ov_stats)

    # Route, direction and stable ids. Loop routes use the sweep sign about the route centre;
    # chains shorter than 150 m take the label of a longer same-route neighbour (successor first).
    route_pts = collections.defaultdict(list)
    for ci, c in enumerate(cars):
        c["route"] = segs[c["segs"][0]]["route"]
        route_pts[c["route"]].extend(zip(geo[ci]["x"], geo[ci]["z"]))
    raw = {}
    for ci, c in enumerate(cars):
        g = geo[ci]
        if c["route"] in ("C1", "C2", "C3", "C4"):
            pts = route_pts[c["route"]]
            cx = sum(p[0] for p in pts) / len(pts)
            cz = sum(p[1] for p in pts) / len(pts)
            raw[ci] = chain_dir(c["route"], [x - cx for x in g["x"]], [z - cz for z in g["z"]])
        else:
            raw[ci] = chain_dir(c["route"], g["x"], g["z"])
    lab = dict(raw)
    weak = {ci for ci in lab if geo[ci]["s"][-1] < 150.0}
    by_from, by_to = collections.defaultdict(list), collections.defaultdict(list)
    for ci, c in enumerate(cars):
        by_from[c["from_key"]].append(ci)
        by_to[c["to_key"]].append(ci)
    for _ in range(12):
        changed = False
        for ci in sorted(weak):
            r = cars[ci]["route"]
            nb = [cj for cj in by_from.get(cars[ci]["to_key"], []) + by_to.get(cars[ci]["from_key"], [])
                  if cj != ci and cj not in weak and cars[cj]["route"] == r]
            if nb:
                best = max(nb, key=lambda cj: geo[cj]["s"][-1])
                if lab[ci] != lab[best]:
                    lab[ci] = lab[best]
                    changed = True
        if not changed:
            break
    loop_ci = [ci for ci in lab if cars[ci]["route"] in ("C1", "C2") and ci not in weak]
    par = {ci: ci for ci in loop_ci}

    def fnd(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x

    for ci in loop_ci:
        if geo[ci]["ramp"]:
            continue
        for cj in by_from.get(cars[ci]["to_key"], []):
            if cj in par and cars[cj]["route"] == cars[ci]["route"] and not geo[cj]["ramp"]:
                par[fnd(ci)] = fnd(cj)
    clusters = collections.defaultdict(collections.Counter)
    for ci in loop_ci:
        clusters[fnd(ci)][lab[ci]] += geo[ci]["s"][-1]
    for ci in loop_ci:
        cv = clusters[fnd(ci)]
        top, topv = cv.most_common(1)[0]
        if topv >= 0.7 * sum(cv.values()):      # clean cluster: one circulation
            lab[ci] = top
    dir_conf = []
    for ci in lab:
        if ci in weak or cars[ci]["route"] not in ("C1", "C2"):
            continue
        for cj in by_from.get(cars[ci]["to_key"], []):
            if cj not in weak and cars[cj]["route"] == cars[ci]["route"] and lab[cj] != lab[ci]:
                dir_conf.append({"from_way": cars[ci]["pieces"][0]["way"], "to_way": cars[cj]["pieces"][0]["way"],
                                 "labels": [lab[ci], lab[cj]], "route": cars[ci]["route"]})
    log("direction labels: %d successor conflicts on same route" % len(dir_conf))
    # Stable ids: data/shuto/id_map.json maps "route|dir|OSM way ids" to an id. Known chains keep
    # their id; new chains get the next free sequence number for their route and direction.
    id_path = OUT / "id_map.json"
    id_map = json.loads(id_path.read_text(encoding="utf-8")) if id_path.exists() else {}
    keys = {}
    for ci, c in enumerate(cars):
        c["dir"] = lab[ci]
        ways_ = sorted({p["way"] for p in c["pieces"] if isinstance(p["way"], int)})
        keys[ci] = "%s|%s|%s|%d,%d" % (c["route"], lab[ci], ",".join(str(w) for w in ways_),
                                       round(geo[ci]["x"][0] / 25.0), round(geo[ci]["z"][0] / 25.0))
    assigned, taken = {}, set()
    for ci in range(len(cars)):
        cid = id_map.get(keys[ci])
        if cid and cid not in taken:
            assigned[ci] = cid
            taken.add(cid)
    maxseq = collections.Counter()
    for cid in set(id_map.values()) | taken:
        pref, _, seq_ = cid.rpartition(".")
        maxseq[pref] = max(maxseq[pref], int(seq_))
    fresh = [ci for ci in range(len(cars)) if ci not in assigned]
    fresh.sort(key=lambda ci: (cars[ci]["route"], lab[ci], seq_key(cars[ci]["route"], geo[ci]["x"], geo[ci]["z"])))
    for ci in fresh:
        pref = "%s.%s" % (cars[ci]["route"], lab[ci])
        maxseq[pref] += 1
        assigned[ci] = "%s.%03d" % (pref, maxseq[pref])
    for ci in range(len(cars)):
        cars[ci]["id"] = assigned[ci]
    id_map.update({keys[ci]: assigned[ci] for ci in range(len(cars))})
    id_path.write_text(json.dumps(id_map, indent=0, sort_keys=True), encoding="utf-8")
    log("ids: %d kept from id_map, %d new" % (len(cars) - len(fresh), len(fresh)))

    cross_final = find_crossings(cars, geo) + find_overlaps(cars, geo)
    unresolved = []
    for x in cross_final:
        if abs(x["dy"]) < CLEAR:
            same = point_rank(cars[x["a"]], geo[x["a"]], x["ja"]) == point_rank(cars[x["b"]], geo[x["b"]], x["jb"])
            unresolved.append({"pos": [round(x["x"], 1), round(x["z"], 1)],
                               "carriageways": [cars[x["a"]]["id"], cars[x["b"]]["id"]],
                               "dy": round(x["dy"], 2),
                               "status": "unresolved_same_level" if same else "unresolved"})
    log("crossings below %.1f m clearance after lifting: %d" % (CLEAR, len(unresolved)))

    # Lanes, shoulders, kinds, names, structure.
    for ci, c in enumerate(cars):
        g = geo[ci]
        pieces = c["pieces"]
        tags0 = pieces[0]["tags"]
        is_link = g["ramp"]
        lanes = max(1, sorted(default_lanes(p["tags"], p["link"]) for p in pieces)[len(pieces) // 2])
        c["lanes"] = lanes
        c["lane_width"] = lane_width(tags0, c["route"], is_link)
        c["shoulder_left"], c["shoulder_right"] = (0.5, 0.5) if is_link else (0.75, 0.5)
        c["speed_kmh"] = speed_kmh(tags0, is_link)
        if c["from_kind"] and c["from_kind"][0] == "boundary" and c["from_kind"][2] == "city":
            c["kind"] = "ramp_on"
        elif c["to_kind"] and c["to_kind"][0] == "boundary" and c["to_kind"][2] == "city":
            c["kind"] = "ramp_off"
        elif is_link:
            c["kind"] = "jct"
        else:
            c["kind"] = "main"
        names = collections.Counter(p["tags"].get("name", "") for p in pieces if p["tags"].get("name"))
        c["name_ja"] = names.most_common(1)[0][0] if names else ""
        en = ""
        for p in pieces:
            if p["tags"].get("name:en"):
                en = ascii_or_empty(p["tags"]["name:en"])
                break
        c["name_en"] = en or f"Shuto {c['route']} {c['dir']}"
        c["osm_ways"] = list(dict.fromkeys(p["way"] for p in pieces if isinstance(p["way"], int)))
        c["structure"] = structure_ranges(g)
    for ci, c in enumerate(cars):
        c["bank"] = plan_bank(c, geo[ci], multi_keys)
    grade_summary = {"limits_pct": {"design_main": 6.0, "design_ramp": 8.0, "warn_main": 8.0, "warn_ramp": 10.0},
                     "violations": [], "max_abrupt_pct_per_10m": 0.0}
    for ci, c in enumerate(cars):
        mg, ab = grade_stats(geo[ci])
        c["max_grade_pct"] = round(100.0 * mg, 2)
        c["max_abrupt_pct"] = round(100.0 * ab, 2)
        grade_summary["max_abrupt_pct_per_10m"] = max(grade_summary["max_abrupt_pct_per_10m"], c["max_abrupt_pct"])
        if mg > (0.10 if geo[ci]["ramp"] else 0.08):
            grade_summary["violations"].append({"id": c["id"], "max_grade_pct": c["max_grade_pct"],
                                                "ramp": bool(geo[ci]["ramp"])})

    # Achieved heights at pins.
    pin_achieved = []
    for pin in HEIGHT_PINS:
        la0, lo0, la1, lo1 = pin["bbox"]
        xa, za = project(la0, lo0)
        xb, zb = project(la1, lo1)
        x0, x1, z0, z1 = min(xa, xb), max(xa, xb), min(za, zb), max(za, zb)
        ym = max((y for ci in range(len(cars)) for x, z, y in
                  zip(geo[ci]["x"], geo[ci]["z"], geo[ci]["y"]) if x0 <= x <= x1 and z0 <= z <= z1), default=None)
        pin_achieved.append({"pin": pin["name"], "target_y": pin["y"],
                             "max_y_in_bbox": round(ym, 2) if ym is not None else None})
    log("pin achieved: %s" % pin_achieved)

    prio_rows, node_spread = priority_table(cars, geo)
    byid = {c["id"]: c for c in cars}
    for r_ in prio_rows:
        r_["max_grade_pct"] = byid[r_["id"]]["max_grade_pct"]
        r_["max_abrupt_pct"] = byid[r_["id"]]["max_abrupt_pct"]
    prio_unres = [u for u in unresolved if PRIORITY_XZ[0] <= u["pos"][0] <= PRIORITY_XZ[2]
                  and PRIORITY_XZ[1] <= u["pos"][1] <= PRIORITY_XZ[3]]
    log("priority bbox: %d carriageways, %d unresolved crossings, node height spread %.4f m"
        % (len(prio_rows), len(prio_unres), node_spread))

    extras = {"overlap": ov_stats, "grades": grade_summary, "dir_conflicts": dir_conf, "pa_service": pa_service, "snaps": snaps, "pins": pins, "pin_achieved": pin_achieved,
              "priority": {"bbox_xz": list(PRIORITY_XZ), "carriageways": prio_rows,
                           "unresolved_crossings": prio_unres, "node_height_spread_m": node_spread}}
    return finish(today, t_start, cars, geo, nid, node_kind, ends_in, ends_out, excluded,
                  unresolved, fac, chains, segs, ways, topo, dropped_nodes, extras)


def ez_fmt(v):
    return f"{v:.2f}"


def max_layer(c):
    best = 0
    for p in c["pieces"]:
        best = max(best, parse_int(p["tags"].get("layer"), 0))
    return best


def chain_id_placeholder(cars, ci):
    return cars[ci].get("id", f"chain{ci}")


def structure_ranges(g):
    """Contiguous ranges of structure type along the carriageway."""
    out = []
    types = g["type"]
    s = g["s"]
    start = 0
    for i in range(1, len(types) + 1):
        if i == len(types) or types[i] != types[start]:
            out.append({"s0": round(s[start], 1), "s1": round(s[i - 1], 1), "type": types[start]})
            start = i
    return out


def finish(today, t_start, cars, geo, nid, node_kind, ends_in, ends_out, excluded,
           unresolved, fac, chains, segs, ways, topo, dropped_nodes, extras):
    """Assemble outputs, validation and report."""
    # Carriageway ids must be unique; sort by id for stable output order.
    order = sorted(range(len(cars)), key=lambda i: cars[i]["id"])
    # Nodes: positions from carriageway endpoints.
    node_pos = {}
    node_flags = {}
    for ci in order:
        c, g = cars[ci], geo[ci]
        for key, idx in ((c["from_key"], 0), (c["to_key"], -1)):
            if key not in node_pos:
                node_pos[key] = [round(g["x"][idx], 3), round(g["y"][idx], 3), round(g["z"][idx], 3)]
    # Node kind: boundary if any endpoint is boundary; merge / diverge / split / end.
    out_nodes = []
    exit_info = {}
    for key, pos in sorted(node_pos.items(), key=lambda kv: nid[kv[0]]):
        ni, no = ends_in[key], ends_out[key]
        k_entry = node_kind.get(key)
        if isinstance(key, tuple) or (k_entry and k_entry[0] == "boundary"):
            kind = "boundary"
        elif k_entry and k_entry[0] == "end":
            kind = "end"
        elif no == 1 and ni >= 2:
            kind = "merge"
        elif ni == 1 and no >= 2:
            kind = "diverge"
        elif ni == 1 and no == 1:
            kind = "split"
        else:
            kind = "split"
        rec = {"id": nid[key], "pos": pos, "kind": kind}
        out_nodes.append(rec)
    # Exit info for boundary nodes (from the chain that ends/starts there).
    rec_by_id = {r["id"]: r for r in out_nodes}
    for ci in order:
        c = cars[ci]
        for key, kind in ((c["from_key"], c["from_kind"]), (c["to_key"], c["to_kind"])):
            if kind and kind[0] == "boundary" and rec_by_id[nid[key]].get("exit") is None:
                rec_by_id[nid[key]]["exit"] = {"to": kind[1], "operator": kind[2]}
    for rec in out_nodes:
        rec.setdefault("exit", None)

    car_out = []
    for ci in order:
        c, g = cars[ci], geo[ci]
        pts = [[round(g["x"][i], 3), round(g["y"][i], 3), round(g["z"][i], 3)] for i in range(len(g["x"]))]
        car_out.append({
            "id": c["id"], "route": c["route"], "dir": c["dir"], "kind": c["kind"],
            "from": nid[c["from_key"]], "to": nid[c["to_key"]],
            "lanes": c["lanes"], "lane_width": c["lane_width"],
            "shoulder_left": c["shoulder_left"], "shoulder_right": c["shoulder_right"],
            "speed_kmh": c["speed_kmh"],
            "name_en": c["name_en"], "name_ja": c["name_ja"],
            "points": pts,
            "structure": c["structure"],
            "osm_ways": c["osm_ways"],
            "bank": c["bank"],
        })

    # Facilities.
    facilities, pa_roads, pa_nodes = build_facilities(fac, car_out, extras['pa_service'])
    car_out.extend(pa_roads)
    out_nodes.extend(pa_nodes)

    # Validation and report.
    report = build_report(today, t_start, car_out, geo, out_nodes, excluded, unresolved,
                          cars, order, nid, node_pos, ends_in, ends_out, node_kind, facilities, ways, segs, fac)

    report.update({"overlap": extras["overlap"], "grades": extras["grades"], "dir_conflicts": extras["dir_conflicts"], "snaps": extras["snaps"], "height_pins": extras["pins"],
                   "pin_achieved": extras["pin_achieved"], "priority": extras["priority"],
                   "pa_lot_carriageways": len(pa_roads), "pa_facilities": sum(1 for f in facilities if f["type"] == "pa")})
    sources = [{"id": "osm", "license": "ODbL-1.0", "attribution": "(c) OpenStreetMap contributors",
                "extract": f"Overpass API: highway=motorway|motorway_link in bbox {BB}; Shuto selected by ref/name",
                "date": today}]
    net = {
        "format": "nox-shuto-network", "version": 1, "reference_date": "2002-12-31",
        "origin": {"lat": ORIGIN_LAT, "lon": ORIGIN_LON, "name": "Edobashi JCT"},
        "ground_y": GROUND_Y, "sources": sources,
        "nodes": out_nodes, "carriageways": car_out, "facilities": facilities,
        "excluded": excluded,
    }
    write_json(OUT / "network.json", net)
    write_derived(car_out, out_nodes)
    write_json(OUT / "build_report.json", report)
    log(f"done in {time.time() - t_start:.1f} s")
    return net


def write_json(path, obj):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")
    log(f"wrote {path.relative_to(ROOT)} ({path.stat().st_size / 1e6:.2f} MB)")


def write_derived(car_out, out_nodes):
    """NOX RoadNetwork JSON: one node per centreline point, one edge per consecutive pair."""
    nodes, edges = [], []
    topo_ids = {}
    for c in car_out:
        pts = c["points"]
        ids = []
        for i, p in enumerate(pts):
            if i == 0:
                nid = c["from"]
            elif i == len(pts) - 1:
                nid = c["to"]
            else:
                nid = f"{c['id']}.p{i:04d}"
            ids.append(nid)
            if nid.startswith(c["id"]):
                nodes.append({"id": nid, "pos": p})
        half = c["lanes"] * c["lane_width"] / 2 + max(c["shoulder_left"], c["shoulder_right"])
        for a, b in zip(ids, ids[1:]):
            edges.append({"a": a, "b": b, "half_width": round(half, 3), "name": c["name_en"],
                          "oneway": True, "lanes": c["lanes"], "lane_width": c["lane_width"],
                          "carriageway": c["id"]})
    # Topological nodes once each.
    seen = set()
    topo = []
    for n in out_nodes:
        topo.append({"id": n["id"], "pos": n["pos"]})
        seen.add(n["id"])
    final_nodes = topo + [n for n in nodes if n["id"] not in seen]
    doc = {"format": "nox-road-network", "road_half_width": 4.0, "sidewalk_width": 0, "traffic_side": "left", "band_walls": False,
           "nodes": final_nodes, "edges": edges}
    write_json(OUT / "nox_road_network.json", doc)


MANIFEST_NOTES = {
    "C1": "manifest lists 5 open segments only (7.5 km); real loop is about 14.5 km per direction",
    "C2": "2002 arc Itabashi-Kasai only; Kumanocho, Ohashi, Yamate, Komatsugawa JCT excluded; 2019 ramp may remain",
    "B": "manifest 56 km = open segments; ours includes ramps' main pieces and bay bridges",
    "K2": "no manifest data (route not verified)",
    "K6": "manifest 2002 section from overview only; Daishi-Tonomachi (2010) not excluded by geometry",
    "Y": "Yaesu route (open at reference) not found as ways in this extract",
    "S5": "manifest data from overview only",
    "S1": "manifest sum excludes some sections; approximate",
    "10": "excluded: opened 2009/2018",
    "K7": "excluded: opened 2017/2020",
    "S2": "excluded: main sections opened 2004/2006",
}


def compare_manifest(main_by_route):
    """Rows comparing manifest open-segment km with OSM-derived main km per direction."""
    rows, gaps = [], []
    if not MANIFEST.exists():
        return rows, ["route_manifest.json missing"]
    man = json.loads(MANIFEST.read_text(encoding="utf-8"))
    for r in man.get("routes", []):
        rid = r["id"]
        segs = [sg for sg in r.get("segments", []) if sg.get("status_at_reference") == "open"]
        exp = sum(sg.get("length_km") or 0.0 for sg in segs)
        nulls = sum(1 for sg in segs if sg.get("length_km") is None)
        ours = main_by_route.get(rid)
        main_dir = max(ours.values(), default=0.0) if ours else 0.0
        if rid == "B" and ours:          # Wangan is split by compass heading; one-way length = half the total
            main_dir = sum(ours.values()) / 2.0
        dev = round(100.0 * (main_dir - exp) / exp, 1) if exp > 0 else None
        rows.append({"route": rid, "manifest_open_km": round(exp, 2), "segments_without_length": nulls,
                     "ours_main_km_max_dir": round(main_dir, 2), "deviation_pct": dev,
                     "note": MANIFEST_NOTES.get(rid, "")})
        if not ours and exp > 0:
            gaps.append(f"{rid}: manifest says open ({exp:.1f} km) but no carriageways built")
        elif rid in MANIFEST_NOTES and rid in ("K2", "K6", "Y", "S5"):
            gaps.append(f"{rid}: {MANIFEST_NOTES[rid]}")
    gaps.append("C2 western half, Oi-Ohashi (Yamate Tunnel), Kumanocho, Komatsugawa JCT 2019 ramp: excluded or approximate, see excluded")
    gaps.append("C1 Gofukubashi/Edobashi exits and Nihonbashi underground works: present in OSM (closures post-2002 not modelled)")
    gaps.append("Route 1 Ueno / Haneda and route 6 Mukojima / Misato split by name keywords; ways without name keep ref-based assignment")
    return rows, gaps


def component_summary(car_out):
    """Weakly connected components of the carriageway graph with route mix and centre."""
    parent = {}

    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for c in car_out:
        parent[find(c["from"])] = find(c["to"])
    groups = collections.defaultdict(list)
    for c in car_out:
        groups[find(c["from"])].append(c)
    out = []
    for g in sorted(groups.values(), key=len, reverse=True)[:12]:
        xs = [p[0] for c in g for p in c["points"]]
        zs = [p[2] for c in g for p in c["points"]]
        cx, cz = sum(xs) / len(xs), sum(zs) / len(zs)
        out.append({"carriageways": len(g),
                    "routes": dict(collections.Counter(c["route"] for c in g)),
                    "centre_xz": [round(cx), round(cz)],
                    "example": g[0]["id"]})
    return out


def named_pos(fac, key):
    """First OSM facility whose name contains key: (lat, lon) or None."""
    for el in fac.get("elements", []):
        if key in el.get("tags", {}).get("name", ""):
            c = el.get("center", el)
            if "lat" in c and "lon" in c:
                return c["lat"], c["lon"]
    return None


def build_report(today, t_start, car_out, geo, out_nodes, excluded, unresolved, cars, order,
                 nid, node_pos, ends_in, ends_out, node_kind, facilities, ways, segs, fac):
    """Counts, km per route/direction, connectivity, dead ends, elevation, landmarks."""
    per_route = collections.defaultdict(lambda: {"count": 0, "km": 0.0, "by_dir": collections.defaultdict(float)})
    total_km = 0.0
    for c in car_out:
        pts = c["points"]
        L = sum(math.dist(pts[i][::2], pts[i + 1][::2]) for i in range(len(pts) - 1)) / 1000.0
        per_route[c["route"]]["count"] += 1
        per_route[c["route"]]["km"] += L
        per_route[c["route"]]["by_dir"][c["dir"]] += L
        total_km += L
    routes = {r: {"carriageways": v["count"], "km": round(v["km"], 2),
                  "km_by_dir": {d: round(k, 2) for d, k in sorted(v["by_dir"].items())}}
              for r, v in sorted(per_route.items())}

    # Connectivity over the directed node graph.
    nodes_all = set()
    adj = collections.defaultdict(list)
    radj = collections.defaultdict(list)
    for c in car_out:
        nodes_all.add(c["from"]); nodes_all.add(c["to"])
        adj[c["from"]].append(c["to"]); radj[c["to"]].append(c["from"])
    parent = {}

    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for c in car_out:
        ra, rb = find(c["from"]), find(c["to"])
        parent[ra] = rb
    wcc = collections.Counter(find(n) for n in nodes_all)
    scc = scc_kosaraju(list(nodes_all), adj, radj)
    scc_size = collections.Counter(scc.values())
    entries = [n for n in nodes_all if not radj.get(n) and adj.get(n)]
    exits = [n for n in nodes_all if not adj.get(n) and radj.get(n)]
    reach_f = bfs(entries, adj)
    reach_b = bfs(exits, radj)
    unreached, no_exit = [], []
    for c in car_out:
        in_cycle = scc.get(c["from"]) == scc.get(c["to"]) and scc_size[scc[c["from"]]] > 1
        if not in_cycle and c["from"] not in reach_f:
            unreached.append(c["id"])
        if not in_cycle and c["to"] not in reach_b:
            no_exit.append(c["id"])
    # Dead ends: nodes with a single chain end that are not boundary nodes.
    boundary_ids = {n["id"] for n in out_nodes if n["kind"] == "boundary"}
    pos_by_id = {n["id"]: n["pos"] for n in out_nodes}
    dead_ends = []
    for n in nodes_all:
        deg = len(adj.get(n, [])) + len(radj.get(n, []))
        if deg == 1 and n not in boundary_ids:
            dead_ends.append(n)
    dead_list = [{"node": n, "pos": pos_by_id.get(n)} for n in dead_ends[:200]]

    # Elevation summary.
    max_y = max((p[1] for c in car_out for p in c["points"]), default=None)
    min_y = min((p[1] for c in car_out for p in c["points"]), default=None)

    # Landmarks (approximate lat/lon, nearest topological node or carriageway point).
    # Named OSM junction nodes where available; bridges and tunnel are approximate.
    landmarks = {
        "Edobashi JCT (OSM junction; contract origin 35.6843,139.7745)": named_pos(fac, "\u6c5f\u6238\u6a4b\u30b8\u30e3\u30f3\u30af\u30b7\u30e7\u30f3"),
        "Hamazakibashi JCT (OSM)": named_pos(fac, "\u6d5c\u5d0e\u6a4bJCT"),
        "Tatsumi JCT (OSM)": named_pos(fac, "\u8fb0\u5df3JCT"),
        "Daikoku JCT (OSM)": named_pos(fac, "\u5927\u9ed2JCT"),
        "Rainbow Bridge midpoint (approx.)": (35.6365, 139.7637),
        "Yokohama Bay Bridge midpoint (approx.)": (35.4440, 139.6640),
        "Haneda tunnel (approx.)": (35.5560, 139.7600),
    }
    lm_out = {}
    for name, pos in landmarks.items():
        if pos is None:
            continue
        la, lo = pos
        x, z = project(la, lo)
        best, bp = 1e18, None
        for c in car_out:
            for p in c["points"]:
                d = (p[0] - x) ** 2 + (p[2] - z) ** 2
                if d < best:
                    best, bp = d, (c["id"], p)
        if bp:
            lm_out[name] = {"lat": la, "lon": lo, "xz": [round(x, 1), round(z, 1)],
                            "nearest_carriageway": bp[0], "nearest_point": bp[1],
                            "offset_m": round(math.sqrt(best), 1)}
    main_by_route = collections.defaultdict(lambda: collections.defaultdict(float))
    for c in car_out:
        if c["kind"] == "main":
            pts = c["points"]
            main_by_route[c["route"]][c["dir"]] += sum(
                math.dist(pts[i][::2], pts[i + 1][::2]) for i in range(len(pts) - 1)) / 1000.0
    manifest_rows, manifest_gaps = compare_manifest(
        {k: {d: round(v, 2) for d, v in dd.items()} for k, dd in main_by_route.items()})
    manifest_note = (f"docs/shuto/route_manifest.json used for era rules and route ids; "
                     f"per-route comparison below (manifest lengths are one direction, summed over open segments)")
    comps = component_summary(car_out)

    return {
        "generated": today,
        "build_seconds": round(time.time() - t_start, 1),
        "source": "OpenStreetMap via Overpass API (ODbL-1.0), bbox " + BB,
        "manifest": manifest_note,
        "totals": {"carriageways": len(car_out), "nodes": len(out_nodes),
                   "boundary_nodes": len(boundary_ids), "km_total": round(total_km, 2),
                   "facilities": len(facilities), "excluded_ways": sum(len(e["osm_ways"]) for e in excluded)},
        "routes": routes,
        "manifest_comparison": manifest_rows,
        "manifest_gaps": manifest_gaps,
        "connectivity": {
            "components": comps,
            "weakly_connected_components": len(wcc),
            "wcc_sizes": sorted(wcc.values(), reverse=True)[:20],
            "strongly_connected_components": len(scc_size),
            "scc_sizes_top": sorted(scc_size.values(), reverse=True)[:10],
            "entries": len(entries), "exits": len(exits),
            "carriageways_unreachable_from_entry": unreached[:200],
            "carriageways_cannot_reach_exit": no_exit[:200],
            "dead_end_nodes_not_boundary": dead_list,
        },
        "elevation": {"y_min": min_y, "y_max": max_y,
                      "unresolved_crossings": unresolved[:300],
                      "unresolved_crossing_count": len(unresolved)},
        "landmarks": lm_out,
        "excluded": excluded,
        "gaps": dead_list,
    }


if __name__ == "__main__":
    main()
