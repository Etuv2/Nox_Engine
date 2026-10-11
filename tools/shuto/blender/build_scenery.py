"""Shuto scenery builder: scenery_plan.json -> chunked render/collision GLBs, .blend, scenery.json.

Run headless (Blender 4.3, Python 3.11) after tools/shuto/scenery_plan.py:
  blender.exe -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/build_scenery.py -- ^
      --plan models/shuto/rep_section/scenery_plan.json --out models/shuto/rep_section

Outputs (paths relative to the repo root unless absolute):
  <out>/scenery/<cx>_<cz>.glb              render meshes, world coordinates (node at origin)
  <out>/scenery_collision/<cx>_<cz>_col.glb  building walls, landmark boxes, container walls as collision
  <out>/scenery.json                       chunk list, triangle counts, placements, licences
  <out>/scenery_tex/*.jpg|png              derived 1024 px atlases (base, lit-window emissive, roof, foliage)
  models/shuto/src/rep_section_scenery.blend

Frame: NOX (+X east, +Y up, +Z south, metres). Blender is written as (x, -z, y) so the glTF
exporter (Y-up) writes NOX coordinates back unchanged. Triangle winding is computed in NOX
space and checked against the intended normal, so faces point outward / up.

Materials (names are part of the contract with the engine):
  facade_apartment|commercial|industrial  facade atlas, darkened albedo, emissive lit windows
                                          (KHR_materials_emissive_strength); used by full and far masses
  roof_membrane                           flat roofs (rooftop_membrane.png, darkened)
  container                               container boxes (industrial atlas, no emission)
  foliage                                 foliage_cutouts alpha atlas, alpha MASK (cutoff 0.5), double sided
  water                                   rough dark water surface, roughness 0.05, metallic 0
  collision                               collision only
"""
import argparse
import array
import json
import math
import os
import random
import sys
from collections import defaultdict

try:
    import bpy  # only available inside Blender
    from mathutils import Matrix, Vector
    import numpy as np
except ImportError:  # keeps the module importable for syntax checks outside Blender
    bpy = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
TEX_SRC = os.path.join(ROOT, "textures", "shuto")
DERIVED = os.path.join(TEX_SRC, "derived")
KIT_GLB = os.path.join(ROOT, "models", "shuto", "kit", "buildings_asian_themed.glb")
KIT_INDEX = os.path.join(ROOT, "models", "shuto", "kit", "kit_index.json")
FACADE_UNITS_DIR = os.path.join(DERIVED, "facade_units")
FACADE_UNITS_MANIFEST = os.path.join(FACADE_UNITS_DIR, "manifest.json")
FACADE_STYLES_DIR = os.path.join(DERIVED, "facade_styles")
FACADE_STYLES_MANIFEST = os.path.join(FACADE_STYLES_DIR, "manifest.json")

STOREY = 3.3
BAY = {"apartment": 3.5, "commercial": 4.0, "industrial": 6.0}
BASE_TARGET_SRGB = 0.40   # mean sRGB albedo of facade/roof atlases (about 0.13 linear); night-dark
ROOF_TARGET_SRGB = 0.26  # rooftop membrane, mean sRGB
BASE_Y = 2.9       # buildings start just under the ground gravel (ground_y - 0.1)
WATER_Y = 3.05     # above the gravel (ground_y - 0.05 = 2.95) and the deck slab at ground_y
EMIT_STRENGTH = 2.5  # room-painted panes at EV100 3: 2 to 3 (see docs/shuto/scenery.md)
TEX_SIZE = 1024
FACADE_TEX_SIZE = 768  # facade base and lit atlases (kept small: chunks embed them)
INSET_PX = 4.0
CONTAINER_H = 2.59
CONTAINER_L = 12.19
CONTAINER_W = 2.44


def log(msg):
    print("[build_scenery] " + msg, flush=True)


def resolve(p):
    return p if os.path.isabs(p) else os.path.join(ROOT, p)


# ---------------------------------------------------------------- triangle buckets

class Buckets:
    """Per (chunk, material) flat vertex/uv arrays. Positions are stored in Blender space."""

    def __init__(self):
        self.pos = defaultdict(lambda: array.array("f"))
        self.uv = defaultdict(lambda: array.array("f"))
        self.tris = defaultdict(int)


def nox_to_blender(p):
    return (p[0], -p[2], p[1])


def tri_nox(bk, key, a, b, c, ua, ub, uc, want):
    """Append a triangle to bucket key; points converted to Blender space."""
    e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    e2 = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    if n[0] * want[0] + n[1] * want[1] + n[2] * want[2] < 0.0:
        b, c = c, b
        ub, uc = uc, ub
    p = bk.pos[key]
    q = bk.uv[key]
    for pt in (a, b, c):
        p.extend(nox_to_blender(pt))
    q.extend(ua)
    q.extend(ub)
    q.extend(uc)
    bk.tris[key] += 1


def quad_nox(bk, key, p00, p10, p11, p01, uv00, uv10, uv11, uv01, want):
    tri_nox(bk, key, p00, p10, p11, uv00, uv10, uv11, want)
    tri_nox(bk, key, p00, p11, p01, uv00, uv11, uv01, want)


# ---------------------------------------------------------------- geometry

def signed_area(poly):
    a = 0.0
    n = len(poly)
    for i in range(n):
        x0, z0 = poly[i]
        x1, z1 = poly[(i + 1) % n]
        a += x0 * z1 - x1 * z0
    return 0.5 * a


def triangulate(poly):
    """Ear clipping on a simple polygon. Returns index triples (CCW in x,z). Falls back to fan."""
    n = len(poly)
    if n < 3:
        return []
    idx = list(range(n))
    if signed_area(poly) < 0.0:
        idx.reverse()
    tris = []
    guard = 0
    while len(idx) > 3 and guard < 20000:
        guard += 1
        m = len(idx)
        found = False
        for k in range(m):
            i0, i1, i2 = idx[(k - 1) % m], idx[k], idx[(k + 1) % m]
            a, b, c = poly[i0], poly[i1], poly[i2]
            cr = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if cr <= 1e-9:
                continue
            ok = True
            for j in idx:
                if j in (i0, i1, i2):
                    continue
                px, pz = poly[j]
                d1 = (b[0] - a[0]) * (pz - a[1]) - (b[1] - a[1]) * (px - a[0])
                d2 = (c[0] - b[0]) * (pz - b[1]) - (c[1] - b[1]) * (px - b[0])
                d3 = (a[0] - c[0]) * (pz - c[1]) - (a[1] - c[1]) * (px - c[0])
                if d1 >= 0 and d2 >= 0 and d3 >= 0:
                    ok = False
                    break
            if ok:
                tris.append((i0, i1, i2))
                idx.pop(k)
                found = True
                break
        if not found:
            break
    for k in range(1, len(idx) - 1):
        tris.append((idx[0], idx[k], idx[k + 1]))
    return tris


def wall_edges(ring):
    n = len(ring)
    out = []
    for i in range(n):
        a = ring[i]
        b = ring[(i + 1) % n]
        L = math.hypot(b[0] - a[0], b[1] - a[1])
        if L > 0.05:
            out.append((a, b, L))
    return out


def outward_normal(a, b, centre):
    dx, dz = b[0] - a[0], b[1] - a[1]
    L = math.hypot(dx, dz)
    nx, nz = dz / L, -dx / L  # right-hand normal in (x, z)
    if (a[0] - centre[0]) * nx + (a[1] - centre[1]) * nz < 0.0:
        return (nx, 0.0, nz)
    return (-nx, 0.0, -nz)


def unit_pick(pick, si, bi, n):
    """Deterministic unit index for (building, storey, bay); ground floor uses ground units."""
    return (pick * 7919 + si * 104729 + bi * 1299709) % n


def emit_building_walls(bk, key, ring, h, family, lod, pick, units, base_y):
    """Walls from storey x bay units (facade_units atlas). Each quad is one storey high (h / n,
    close to STOREY) and one bay wide (edge / round(edge / BAY)), and takes exactly one unit, so the
    window rows keep real storey heights. Bottom storey = ground units (shopfronts).
    'plain' (far masses, > 350 m from expressways): one quad per edge per storey, the unit stretched
    horizontally along the edge."""
    centre = (sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring))
    top = base_y + h
    upper, ground = units["upper"], units["ground"]
    storeys = 1 if lod == "collision" else max(1, int(round(h / STOREY)))
    band = h / storeys
    for a, b, L in wall_edges(ring):
        want = outward_normal(a, b, centre)
        bays = 1 if lod in ("plain", "collision") else max(1, int(round(L / BAY[family])))
        for bi in range(bays):
            t0 = bi / bays
            t1 = (bi + 1) / bays
            xa = a[0] + (b[0] - a[0]) * t0
            za = a[1] + (b[1] - a[1]) * t0
            xb = a[0] + (b[0] - a[0]) * t1
            zb = a[1] + (b[1] - a[1]) * t1
            for si in range(storeys):
                y0 = base_y + si * band
                y1 = y0 + band
                pool = ground if si == 0 else upper
                u_lo, v_lo, u_hi, v_hi = pool[unit_pick(pick, si, bi, len(pool))]
                p00 = (xa, y0, za)
                p10 = (xb, y0, zb)
                p11 = (xb, y1, zb)
                p01 = (xa, y1, za)
                quad_nox(bk, key, p00, p10, p11, p01,
                         (u_lo, v_lo), (u_hi, v_lo), (u_hi, v_hi), (u_lo, v_hi), want)


SIDEWALK_Y = 3.15
ROAD_Y = 3.03
PAINT_Y = 3.035
CHUNK_M = 500.0
ROOF_TOP_ALL = None


def scale_ring(ring, k):
    """Ring scaled about its centroid (used for setback upper volumes; stays inside the footprint)."""
    cx = sum(p[0] for p in ring) / len(ring)
    cz = sum(p[1] for p in ring) / len(ring)
    return [(cx + (p[0] - cx) * k, cz + (p[1] - cz) * k) for p in ring]


def point_in_ring2(x, z, ring):
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


def interior_point(ring):
    """Centroid if inside, else the first inside point on a 6x6 grid over the bbox."""
    cx = sum(p[0] for p in ring) / len(ring)
    cz = sum(p[1] for p in ring) / len(ring)
    if point_in_ring2(cx, cz, ring):
        return cx, cz
    xs = [p[0] for p in ring]
    zs = [p[1] for p in ring]
    for i in range(1, 6):
        for j in range(1, 6):
            x = min(xs) + (max(xs) - min(xs)) * i / 6.0
            z = min(zs) + (max(zs) - min(zs)) * j / 6.0
            if point_in_ring2(x, z, ring):
                return x, z
    return cx, cz


STYLE_BY_NAME = {}
TINTS = [(1.0, 1.0, 1.0), (0.90, 0.94, 1.0), (1.06, 0.97, 0.86)]
NEAR_M = 250.0


def styled_quad_uv(u0, u1, v0, v1):
    return (u0, v0), (u1, v0), (u1, v1), (u0, v1)


def emit_styled_walls(bk, cx, cz, ring, h, style, tint, off, base_y, near, shop_style=None):
    """One quad per wall edge (per storey band when a ground shopfront is used). The style texture
    repeats; bays and storeys are whole texture cells, so the facade starts and ends on whole bays
    and every wall of a building shares one storey grid. Near buildings get a shopfront ground floor."""
    sd = STYLE_BY_NAME[style]
    NB, NS = STYLE_BY_NAME["_grid"]
    bay_m, storey_m = sd["bay_m"], sd["storey_m"]
    centre = (sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring))
    storeys = max(1, int(round(h / storey_m)))
    shop = bool(near and shop_style is not None and storeys >= 2)
    g = h / storeys if shop else 0.0
    ov = off[1] % NS
    ou = off[0] % NB
    for a, b, L in wall_edges(ring):
        want = outward_normal(a, b, centre)
        bays = max(1, int(round(L / bay_m)))
        u0 = ou / NB
        u1 = u0 + bays / NB
        if shop:
            sh = shop_style
            sg = STYLE_BY_NAME[sh]
            key_g = (cx, cz, "style_%s_t%d" % (sh, tint))
            p00 = (a[0], base_y, a[1])
            p10 = (b[0], base_y, b[1])
            p11 = (b[0], base_y + g, b[1])
            p01 = (a[0], base_y + g, a[1])
            gb = max(1, int(round(L / sg["bay_m"])))
            quad_nox(bk, key_g, p00, p10, p11, p01, (0.0, 0.0), (gb / NB, 0.0), (gb / NB, 1.0 / NS), (0.0, 1.0 / NS), want)
            vu0 = ((ov + 1) % NS) / NS
            vu1 = vu0 + (storeys - 1) / NS
            key = (cx, cz, "style_%s_t%d" % (style, tint))
            p00 = (a[0], base_y + g, a[1])
            p10 = (b[0], base_y + g, b[1])
            p11 = (b[0], base_y + h, b[1])
            p01 = (a[0], base_y + h, a[1])
            quad_nox(bk, key, p00, p10, p11, p01, (u0, vu0), (u1, vu0), (u1, vu1), (u0, vu1), want)
        else:
            key = (cx, cz, "style_%s_t%d" % (style, tint))
            v0 = ov / NS
            v1 = v0 + storeys / NS
            p00 = (a[0], base_y, a[1])
            p10 = (b[0], base_y, b[1])
            p11 = (b[0], base_y + h, b[1])
            p01 = (a[0], base_y + h, a[1])
            quad_nox(bk, key, p00, p10, p11, p01, (u0, v0), (u1, v0), (u1, v1), (u0, v1), want)
    return storeys, shop


def emit_slab(bk, key, p0, p1, n, depth, y0, thick):
    """Closed box slab from wall segment p0-p1 (XZ), projecting `depth` along unit normal n, from y0 up
    `thick`. All six faces, outward normals (quad_nox orients each face)."""
    ux, uz = (p1[0] - p0[0]), (p1[1] - p0[1])
    L = math.hypot(ux, uz)
    ux, uz = ux / L, uz / L
    o0 = (p0[0], p0[1])
    o1 = (p1[0], p1[1])
    f0 = (o0[0] + n[0] * depth, o0[1] + n[1] * depth)
    f1 = (o1[0] + n[0] * depth, o1[1] + n[1] * depth)
    uv = ((0, 0), (1, 0), (1, 1), (0, 1))
    yt = y0 + thick
    # top, bottom, front, two ends
    quad_nox(bk, key, (o0[0], yt, o0[1]), (o1[0], yt, o1[1]), (f1[0], yt, f1[1]), (f0[0], yt, f0[1]), *uv,
             (0.0, 1.0, 0.0))
    quad_nox(bk, key, (o0[0], y0, o0[1]), (f0[0], y0, f0[1]), (f1[0], y0, f1[1]), (o1[0], y0, o1[1]), *uv,
             (0.0, -1.0, 0.0))
    quad_nox(bk, key, (f0[0], y0, f0[1]), (f1[0], y0, f1[1]), (f1[0], yt, f1[1]), (f0[0], yt, f0[1]), *uv,
             (n[0], 0.0, n[1]))
    quad_nox(bk, key, (o0[0], y0, o0[1]), (f0[0], y0, f0[1]), (f0[0], yt, f0[1]), (o0[0], yt, o0[1]), *uv,
             (-ux, 0.0, -uz))
    quad_nox(bk, key, (o1[0], y0, o1[1]), (f1[0], y0, f1[1]), (f1[0], yt, f1[1]), (o1[0], yt, o1[1]), *uv,
             (ux, 0.0, uz))


def emit_rail(bk, key, p0, p1, n, depth, y0, height, thick=0.05):
    """Solid light railing panel along the outer edge of a balcony (no alpha)."""
    fx0 = p0[0] + n[0] * (depth - 0.05)
    fz0 = p0[1] + n[1] * (depth - 0.05)
    fx1 = p1[0] + n[0] * (depth - 0.05)
    fz1 = p1[1] + n[1] * (depth - 0.05)
    ux, uz = (fx1 - fx0), (fz1 - fz0)
    L = math.hypot(ux, uz)
    ux, uz = ux / L, uz / L
    ox, oz = -uz, ux  # offset across the panel thickness
    t = thick / 2
    a0 = (fx0 + n[0] * t, fz0 + n[1] * t)
    a1 = (fx1 + n[0] * t, fz1 + n[1] * t)
    b0 = (fx0 - n[0] * t, fz0 - n[1] * t)
    b1 = (fx1 - n[0] * t, fz1 - n[1] * t)
    yt = y0 + height
    uv = ((0, 0), (1, 0), (1, 1), (0, 1))
    quad_nox(bk, key, (a0[0], y0, a0[1]), (a1[0], y0, a1[1]), (a1[0], yt, a1[1]), (a0[0], yt, a0[1]), *uv,
             (n[0], 0.0, n[1]))
    quad_nox(bk, key, (b0[0], y0, b0[1]), (b1[0], y0, b1[1]), (b1[0], yt, b1[1]), (b0[0], yt, b0[1]), *uv,
             (-n[0], 0.0, -n[1]))
    quad_nox(bk, key, (a0[0], yt, a0[1]), (a1[0], yt, a1[1]), (b1[0], yt, b1[1]), (b0[0], yt, b0[1]), *uv,
             (0.0, 1.0, 0.0))


def emit_balconies(bk, cx, cz, ring, h, storey_m, base_y):
    """Balcony units for apartment styles (near only): per storey and per bay, a 0.18 m concrete
    slab projecting 1.2 m under the window zone, with a 0.7 m solid railing on its outer edge.
    Party walls: 0.35 m gap each side of every bay unit. Only the wall-edge and floor-line
    geometry is emitted (no glazing covered: the slab sits at the sill line)."""
    centre = (sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring))
    storeys = max(1, int(round(h / storey_m)))
    sh = h / storeys
    gap = 0.35
    depth = 1.2
    for a, b, L in wall_edges(ring):
        if L < 6.0:
            continue
        want = outward_normal(a, b, centre)
        n = (want[0], want[2])
        bays = max(1, int(round(L / BAY["apartment"])))
        bw = L / bays
        ux, uz = (b[0] - a[0]) / L, (b[1] - a[1]) / L
        for bi in range(bays):
            s0 = bi * bw + gap
            s1 = (bi + 1) * bw - gap
            if s1 - s0 < 2.0:
                continue
            p0 = (a[0] + ux * s0, a[1] + uz * s0)
            p1 = (a[0] + ux * s1, a[1] + uz * s1)
            for k in range(1, storeys):
                y0 = base_y + k * sh - 0.18
                emit_slab(bk, (cx, cz, "balcony_slab"), p0, p1, n, depth, y0, 0.18)
                emit_rail(bk, (cx, cz, "balcony_rail"), p0, p1, n, depth, y0 + 0.18, 0.7)


def emit_far_walls(bk, cx, cz, ring, h, fam, pick, units, base_y, far_names):
    """Far LOD: ground quad from the ground units, then one quad per bay for the upper storeys
    with v REPEATing a single-storey far tile (no stretching: v is in storeys, u spans one bay)."""
    centre = (sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring))
    top = base_y + h
    g = min(h, STOREY)
    for a, b, L in wall_edges(ring):
        want = outward_normal(a, b, centre)
        bays = max(1, int(round(L / BAY[fam])))
        for bi in range(bays):
            t0 = bi / bays
            t1 = (bi + 1) / bays
            xa = a[0] + (b[0] - a[0]) * t0
            za = a[1] + (b[1] - a[1]) * t0
            xb = a[0] + (b[0] - a[0]) * t1
            zb = a[1] + (b[1] - a[1]) * t1
            pool = units["ground"]
            u_lo, v_lo, u_hi, v_hi = pool[unit_pick(pick, 0, bi, len(pool))]
            quad_nox(bk, (cx, cz, "facade_" + fam), (xa, base_y, za), (xb, base_y, zb),
                     (xb, base_y + g, zb), (xa, base_y + g, za),
                     (u_lo, v_lo), (u_hi, v_lo), (u_hi, v_hi), (u_lo, v_hi), want)
            if h - g > 0.5:
                name = far_names[(pick + bi) % len(far_names)]
                vspan = (h - g) / STOREY
                quad_nox(bk, (cx, cz, name), (xa, base_y + g, za), (xb, base_y + g, zb),
                         (xb, top, zb), (xa, top, za), (0.0, 0.0), (1.0, 0.0), (1.0, vspan), (0.0, vspan), want)


def emit_cornice(bk, key, ring, top):
    """Parapet lip: a band of vertical quads 0.45 m proud of the top edge, 0.8 m high."""
    centre = (sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring))
    for a, b, L in wall_edges(ring):
        want = outward_normal(a, b, centre)
        ox, oz = want[0] * 0.45, want[2] * 0.45
        p00 = (a[0] + ox, top - 0.8, a[1] + oz)
        p10 = (b[0] + ox, top - 0.8, b[1] + oz)
        p11 = (b[0] + ox, top, b[1] + oz)
        p01 = (a[0] + ox, top, a[1] + oz)
        quad_nox(bk, key, p00, p10, p11, p01, (0, 0), (1, 0), (1, 1), (0, 1), want)


def emit_roof_features(bk, cx, cz, ring, top, flags, roof_key):
    """Simple rooftop geometry: AC units, a water tank, a stair tower (boxes)."""
    ip = interior_point(ring)
    uv = (0.0, 0.0, 1.0, 1.0)
    if flags.get("stair"):
        emit_box(bk, roof_key, (ip[0] - 3.0, ip[1] - 3.0), (1.0, 0.0), 1.6, 1.6, top, 3.4, uv, roof_key)
    if flags.get("tank"):
        emit_box(bk, roof_key, (ip[0] + 3.0, ip[1] + 2.5), (1.0, 0.0), 1.2, 1.2, top, 2.6, uv, roof_key)
    for i in range(int(flags.get("ac", 0))):
        ox = -4.0 + 2.2 * i
        emit_box(bk, roof_key, (ip[0] + ox, ip[1] - 1.0), (1.0, 0.0), 0.6, 0.5, top, 0.9, uv, roof_key)


def emit_full_building(bk, cx, cz, ring, h, fam, pick, units, flags, roof_mat, base_y):
    """Textured storey x bay walls, roof, cornice, rooftop detail; towers get a setback."""
    roof_key = (cx, cz, roof_mat)
    key = (cx, cz, "facade_" + fam)
    if flags.get("setback") and h >= 60.0:
        h1 = h * 0.7
        emit_building_walls(bk, key, ring, h1, fam, "full", pick, units, base_y)
        emit_roof(bk, roof_key, ring, base_y + h1, 4.0)
        inner = scale_ring(ring, 0.72)
        emit_building_walls(bk, key, inner, h - h1, fam, "full", pick + 1, units, base_y + h1)
        emit_roof(bk, roof_key, inner, base_y + h, 4.0)
        emit_cornice(bk, (cx, cz, "roof_membrane"), inner, base_y + h)
        emit_roof_features(bk, cx, cz, inner, base_y + h, flags, (cx, cz, "roof_membrane"))
    else:
        emit_building_walls(bk, key, ring, h, fam, "full", pick, units, base_y)
        emit_roof(bk, roof_key, ring, base_y + h, 4.0)
        emit_cornice(bk, (cx, cz, "roof_membrane"), ring, base_y + h)
        emit_roof_features(bk, cx, cz, ring, base_y + h, flags, (cx, cz, "roof_membrane"))


def emit_sign(bk, cx, cz, ring, h, base_y, pick, bill_uv):
    """Billboard-art sign on the longest facade edge, above the shopfronts (scenery only)."""
    best = None
    for a, b, L in wall_edges(ring):
        if best is None or L > best[2]:
            best = (a, b, L)
    if best is None or best[2] < 8.0 or h < 12.0:
        return
    a, b, L = best
    centre = (sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring))
    want = outward_normal(a, b, centre)
    w = min(L * 0.6, 8.0)
    ux, uz = (b[0] - a[0]) / L, (b[1] - a[1]) / L
    mx, mz = (a[0] + b[0]) / 2 + want[0] * 0.3, (a[1] + b[1]) / 2 + want[2] * 0.3
    y0, y1 = base_y + 4.2, base_y + 4.2 + 2.4
    p00 = (mx - ux * w / 2, y0, mz - uz * w / 2)
    p10 = (mx + ux * w / 2, y0, mz + uz * w / 2)
    p11 = (mx + ux * w / 2, y1, mz + uz * w / 2)
    p01 = (mx - ux * w / 2, y1, mz - uz * w / 2)
    u_lo, v_lo, u_hi, v_hi = bill_uv[pick % len(bill_uv)]
    quad_nox(bk, (cx, cz, "billboard"), p00, p10, p11, p01,
             (u_lo, v_lo), (u_hi, v_lo), (u_hi, v_hi), (u_lo, v_hi), want)


def emit_street_pieces(bk, pieces):
    """City streets at grade: asphalt, raised sidewalks and kerbs, lane paint."""
    n_seg = 0
    for pc in pieces:
        w, sw, cls = pc["w"], pc["sw"], pc["cls"]
        pts = pc["pts"]
        for a, b in zip(pts, pts[1:]):
            L = math.dist(a, b)
            if L < 1e-6:
                continue
            ux, uz = (b[0] - a[0]) / L, (b[1] - a[1]) / L
            nx, nz = -uz, ux
            ck = (int(math.floor((a[0] + b[0]) / 2 / CHUNK_M)), int(math.floor((a[1] + b[1]) / 2 / CHUNK_M)))
            n_seg += 1

            def P(t, off, y):
                return (a[0] + ux * t + nx * off, y, a[1] + uz * t + nz * off)

            # asphalt (metres/4 UV, repeat tile)
            hw = w / 2
            quad_nox(bk, (ck[0], ck[1], "street_asphalt"),
                     P(0, -hw, ROAD_Y), P(L, -hw, ROAD_Y), P(L, hw, ROAD_Y), P(0, hw, ROAD_Y),
                     (0.0, -hw / 4), (L / 4, -hw / 4), (L / 4, hw / 4), (0.0, hw / 4), (0.0, 1.0, 0.0))
            for side in (1, -1):
                o0, o1 = side * hw, side * (hw + sw)
                quad_nox(bk, (ck[0], ck[1], "sidewalk"),
                         P(0, o0, SIDEWALK_Y), P(L, o0, SIDEWALK_Y), P(L, o1, SIDEWALK_Y), P(0, o1, SIDEWALK_Y),
                         (0.0, o0 / 3), (L / 3, o0 / 3), (L / 3, o1 / 3), (0.0, o1 / 3), (0.0, 1.0, 0.0))
                want = (-side * nx, 0.0, -side * nz)
                quad_nox(bk, (ck[0], ck[1], "sidewalk"),
                         P(0, o0, ROAD_Y), P(L, o0, ROAD_Y), P(L, o0, SIDEWALK_Y), P(0, o0, SIDEWALK_Y),
                         (0, 0), (L / 3, 0), (L / 3, 0.15 / 3), (0, 0.15 / 3), want)
            # lane paint on arterials: yellow double centre line, white edge lines, white dashed centre
            # on secondary streets (decal texture spans the stripe width).
            if cls in ("primary", "secondary"):
                for side in (1, -1):
                    eo = side * (hw - 0.45)
                    quad_nox(bk, (ck[0], ck[1], "paint_white"),
                             P(0, eo - 0.08, PAINT_Y), P(L, eo - 0.08, PAINT_Y),
                             P(L, eo + 0.08, PAINT_Y), P(0, eo + 0.08, PAINT_Y),
                             (0, 0), (1, 0), (1, 1), (0, 1), (0.0, 1.0, 0.0))
                if cls == "primary":
                    for side in (1, -1):
                        co = side * 0.2
                        quad_nox(bk, (ck[0], ck[1], "paint_yellow"),
                                 P(0, co - 0.12, PAINT_Y), P(L, co - 0.12, PAINT_Y),
                                 P(L, co + 0.12, PAINT_Y), P(0, co + 0.12, PAINT_Y),
                                 (0, 0), (1, 0), (1, 1), (0, 1), (0.0, 1.0, 0.0))
                else:
                    t = 0.0
                    while t + 3.0 <= L:
                        quad_nox(bk, (ck[0], ck[1], "paint_white"),
                                 P(t, -0.12, PAINT_Y), P(t + 3.0, -0.12, PAINT_Y),
                                 P(t + 3.0, 0.12, PAINT_Y), P(t, 0.12, PAINT_Y),
                                 (0, 0), (1, 0), (1, 1), (0, 1), (0.0, 1.0, 0.0))
                        t += 9.0
    log("street segments emitted: %d" % n_seg)


def emit_street_lamps(bk, lamps):
    """Simple street lamps: a metal pole with an emissive head (boxes)."""
    uv = (0.0, 0.0, 1.0, 1.0)
    for lm in lamps:
        x, z = lm["pos"]
        ck = (int(math.floor(x / CHUNK_M)), int(math.floor(z / CHUNK_M)))
        u = tuple(lm["u"])
        emit_box(bk, (ck[0], ck[1], "street_metal"), (x, z), (1.0, 0.0), 0.12, 0.12, SIDEWALK_Y, 7.5, uv)
        emit_box(bk, (ck[0], ck[1], "lamp_head"), (x, z), u, 0.45, 0.14, SIDEWALK_Y + 7.3, 0.25, uv)


def emit_roof(bk, key, ring, top, uv_scale):
    tris = triangulate(ring)
    for i0, i1, i2 in tris:
        a, b, c = ring[i0], ring[i1], ring[i2]
        pa = (a[0], top, a[1])
        pb = (b[0], top, b[1])
        pc = (c[0], top, c[1])
        if uv_scale:
            ua = (a[0] / uv_scale, a[1] / uv_scale)
            ub = (b[0] / uv_scale, b[1] / uv_scale)
            uc = (c[0] / uv_scale, c[1] / uv_scale)
        else:
            ua = ub = uc = (0.0, 0.0)
        tri_nox(bk, key, pa, pb, pc, ua, ub, uc, (0.0, 1.0, 0.0))


def emit_box(bk, key, centre, u, half_l, half_w, y0, height, uv_rect, roof_key=None):
    """Axis box with footprint along unit vector u (NOX x, z). Four walls, optional roof."""
    ux, uz = u
    vx, vz = uz, -ux
    cx, cz = centre
    corners = []
    for sl, sw in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        corners.append((cx + ux * half_l * sl + vx * half_w * sw, cz + uz * half_l * sl + vz * half_w * sw))
    cen = (cx, cz)
    u_lo, v_lo, u_hi, v_hi = uv_rect
    for i in range(4):
        a = corners[i]
        b = corners[(i + 1) % 4]
        want = outward_normal(a, b, cen)
        p00 = (a[0], y0, a[1])
        p10 = (b[0], y0, b[1])
        p11 = (b[0], y0 + height, b[1])
        p01 = (a[0], y0 + height, a[1])
        quad_nox(bk, key, p00, p10, p11, p01, (u_lo, v_lo), (u_hi, v_lo), (u_hi, v_hi), (u_lo, v_hi), want)
    if roof_key is not None:
        top = y0 + height
        tri_nox(bk, roof_key, (corners[0][0], top, corners[0][1]), (corners[1][0], top, corners[1][1]),
                (corners[2][0], top, corners[2][1]), (0, 0), (0, 0), (0, 0), (0.0, 1.0, 0.0))
        tri_nox(bk, roof_key, (corners[0][0], top, corners[0][1]), (corners[2][0], top, corners[2][1]),
                (corners[3][0], top, corners[3][1]), (0, 0), (0, 0), (0, 0), (0.0, 1.0, 0.0))


# ---------------------------------------------------------------- textures and materials

def manifest_cells():
    with open(os.path.join(DERIVED, "derived_manifest.json"), "r", encoding="utf-8") as f:
        man = json.load(f)
    out = {}
    for a in man["assets"]:
        if a.get("cells") and isinstance(a["cells"], list):
            out[a["id"]] = (a["source_size"], a["cells"])
    return out


def rect_uv(rect, size):
    """Atlas pixel rect (top-left origin) -> Blender UV (bottom-left origin), inset to avoid bleed."""
    W, H = size
    x0, y0, x1, y1 = rect
    u_lo = (x0 + INSET_PX) / W
    u_hi = (x1 - INSET_PX) / W
    v_lo = 1.0 - (y1 - INSET_PX) / H
    v_hi = 1.0 - (y0 + INSET_PX) / H
    return (u_lo, v_lo, u_hi, v_hi)


def load_image_file(path):
    return bpy.data.images.load(path, check_existing=True)


def save_scaled(img, path, fmt, size=TEX_SIZE):
    img.scale(size, size)
    img.filepath_raw = path
    img.file_format = fmt
    img.save()
    return path


def darken_to_target(img, target):
    """Scale RGB so the mean sRGB value of the image is at most target. Returns the factor used."""
    px = np.array(img.pixels[:], dtype=np.float32).reshape(-1, 4)
    mean_rgb = float(px[:, :3].mean())
    k = min(1.0, target / max(mean_rgb, 1e-3))
    px[:, :3] *= k
    img.pixels.foreach_set(px.ravel().tolist())
    return k, mean_rgb


def make_textures(tex_dir, cells_all):
    os.makedirs(tex_dir, exist_ok=True)
    out = {}
    style_man = json.load(open(FACADE_STYLES_MANIFEST, "r", encoding="utf-8"))
    out["styles"] = {}
    for st in style_man["styles"]:
        out["styles"][st["name"]] = {"base": os.path.join(FACADE_STYLES_DIR, st["base"]),
                                     "emit": os.path.join(FACADE_STYLES_DIR, st["emit"]),
                                     "bay_m": st["bay_m"], "storey_m": st["storey_m"], "kind": st["kind"]}
    out["_grid"] = (style_man["bays"], style_man["storeys"])
    # Containers keep the original industrial atlas (their cell rectangles are in its pixel space).
    cont_src = load_image_file(os.path.join(TEX_SRC, "facades_industrial.png"))
    darken_to_target(cont_src, BASE_TARGET_SRGB)
    cont_path = os.path.join(tex_dir, "containers_industrial.jpg")
    save_scaled(cont_src, cont_path, "JPEG", 1024)
    bpy.data.images.remove(cont_src)
    out["container"] = cont_path
    for key, src in (("asphalt", os.path.join(DERIVED, "asphalt_fine.png")),
                     ("sidewalk", os.path.join(DERIVED, "concrete_cast.png")),
                     ("corrugated", os.path.join(DERIVED, "corrugated_metal_seamless.png"))):
        img = load_image_file(src)
        path = os.path.join(tex_dir, "street_%s.jpg" % key)
        save_scaled(img, path, "JPEG", 512)
        bpy.data.images.remove(img)
        out[key] = path
    out["paint_white"] = os.path.join(TEX_SRC, "..", "road_decals", "line_single_white.png")
    out["paint_yellow"] = os.path.join(TEX_SRC, "..", "road_decals", "line_double_yellow.png")
    bill = load_image_file(os.path.join(TEX_SRC, "billboards_art.png"))
    bill_path = os.path.join(tex_dir, "billboards_art.jpg")
    save_scaled(bill, bill_path, "JPEG", 1024)
    bpy.data.images.remove(bill)
    out["billboards"] = bill_path
    roof_src = load_image_file(os.path.join(DERIVED, "rooftop_membrane.png"))
    k, m0 = darken_to_target(roof_src, ROOF_TARGET_SRGB)
    log("roof albedo: mean sRGB %.3f -> %.3f" % (m0, m0 * k))
    roof_path = os.path.join(tex_dir, "rooftop_membrane.jpg")
    save_scaled(roof_src, roof_path, "JPEG", 384)
    out["roof"] = roof_path
    fol_src = load_image_file(os.path.join(TEX_SRC, "foliage_cutouts.png"))
    fol_path = os.path.join(tex_dir, "foliage_cutouts.png")
    save_scaled(fol_src, fol_path, "PNG", 256)
    out["foliage"] = fol_path
    return out


def make_material(name, base_img=None, base_rgb=(0.8, 0.8, 0.8), rough=0.8, metal=0.0,
                  emit_img=None, emit_strength=0.0, alpha_mask=False, double_sided=False, tint=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    if base_img is not None:
        tn = nt.nodes.new("ShaderNodeTexImage")
        tn.image = base_img
        if tint is not None and tint != (1.0, 1.0, 1.0):
            mix = nt.nodes.new("ShaderNodeMixRGB")
            mix.blend_type = "MULTIPLY"
            mix.inputs["Fac"].default_value = 1.0
            mix.inputs["Color2"].default_value = (tint[0], tint[1], tint[2], 1.0)
            nt.links.new(tn.outputs["Color"], mix.inputs["Color1"])
            nt.links.new(mix.outputs["Color"], bsdf.inputs["Base Color"])
        else:
            nt.links.new(tn.outputs["Color"], bsdf.inputs["Base Color"])
        if alpha_mask:
            nt.links.new(tn.outputs["Alpha"], bsdf.inputs["Alpha"])
    else:
        bsdf.inputs["Base Color"].default_value = (base_rgb[0], base_rgb[1], base_rgb[2], 1.0)
    if emit_img is not None and emit_strength > 0.0:
        te = nt.nodes.new("ShaderNodeTexImage")
        te.image = emit_img
        nt.links.new(te.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = emit_strength
    if alpha_mask:
        try:
            m.blend_method = "CLIP"
            m.alpha_threshold = 0.5
        except AttributeError:
            pass
    if double_sided:
        m.use_backface_culling = False
    return m


def make_materials(tex, kit_cells):
    mats = {}
    mats["container"] = make_material("container", base_img=load_image_file(tex["container"]), rough=0.6)
    mats["roof_membrane"] = make_material("roof_membrane", base_img=load_image_file(tex["roof"]), rough=0.9)
    corr = load_image_file(tex["corrugated"])
    darken_to_target(corr, ROOF_TARGET_SRGB)
    mats["roof_corrugated"] = make_material("roof_corrugated", base_img=corr, rough=0.6, metal=0.2)
    for sname, info in tex["styles"].items():
        base_img = load_image_file(info["base"])
        emit_img = load_image_file(info["emit"])
        for t, tint in enumerate(TINTS):
            mats["style_%s_t%d" % (sname, t)] = make_material(
                "style_%s_t%d" % (sname, t), base_img=base_img, rough=0.75,
                emit_img=emit_img, emit_strength=EMIT_STRENGTH, tint=tint)
    mats["street_asphalt"] = make_material("street_asphalt", base_img=load_image_file(tex["asphalt"]), rough=0.85)
    mats["sidewalk"] = make_material("sidewalk", base_img=load_image_file(tex["sidewalk"]), rough=0.9)
    mats["paint_white"] = make_material("paint_white", base_img=load_image_file(tex["paint_white"]),
                                        rough=0.6, alpha_mask=True, double_sided=True)
    mats["paint_yellow"] = make_material("paint_yellow", base_img=load_image_file(tex["paint_yellow"]),
                                         rough=0.6, alpha_mask=True, double_sided=True)
    bill = load_image_file(tex["billboards"])
    mats["billboard"] = make_material("billboard", base_img=bill, rough=0.6,
                                      emit_img=bill, emit_strength=1.2)
    mats["balcony_slab"] = make_material("balcony_slab", base_img=load_image_file(tex["sidewalk"]), rough=0.85)
    mats["balcony_rail"] = make_material("balcony_rail", base_rgb=(0.72, 0.72, 0.70), rough=0.6, metal=0.3)
    mats["street_metal"] = make_material("street_metal", base_rgb=(0.22, 0.24, 0.27), rough=0.45, metal=0.7)
    mats["lamp_head"] = make_material("lamp_head", base_rgb=(1.0, 0.85, 0.6), rough=0.3)
    mats["lamp_head"].node_tree.nodes["Principled BSDF"].inputs["Emission Color"].default_value = (1.0, 0.82, 0.55, 1.0)
    mats["lamp_head"].node_tree.nodes["Principled BSDF"].inputs["Emission Strength"].default_value = 10.0
    mats["foliage"] = make_material("foliage", base_img=load_image_file(tex["foliage"]), rough=0.8,
                                    alpha_mask=True, double_sided=True)
    mats["water"] = make_material("water", base_rgb=(0.05, 0.09, 0.12), rough=0.05, metal=0.0)
    mats["collision"] = make_material("collision", base_rgb=(0.5, 0.5, 0.5), rough=1.0)
    return mats


# ---------------------------------------------------------------- kit landmarks

def shrink_kit_images(mats, tex_dir, size=256):
    """Kit GLB textures are multi-MB PNGs. Replace each with a 256 px JPEG on disk (embedded per chunk)."""
    done = {}
    for m in mats:
        if m is None or not m.use_nodes:
            continue
        for n in m.node_tree.nodes:
            if n.type != "TEX_IMAGE" or n.image is None:
                continue
            img = n.image
            if img.name not in done:
                path = os.path.join(tex_dir, "kit_%s.jpg" % "".join(
                    c if c.isalnum() else "_" for c in img.name))
                img.scale(size, size)
                img.filepath_raw = path
                img.file_format = "JPEG"
                img.save()
                done[img.name] = load_image_file(path)
            n.image = done[img.name]
    return len(done)


def load_kit_templates(names, tex_dir):
    before = set(bpy.data.objects.keys())
    bpy.ops.import_scene.gltf(filepath=KIT_GLB)
    imported = [o for o in bpy.data.objects if o.name not in before]
    kit_mats = {s.material for o in imported if o.type == "MESH" for s in o.material_slots if s.material}
    log("kit textures shrunk: %d" % shrink_kit_images(kit_mats, tex_dir))
    tpl = {}
    for o in imported:
        if o.type != "MESH" or o.name not in names:
            continue
        me = o.data.copy()
        me.transform(o.matrix_world)
        vs = [v.co for v in me.vertices]
        mn = Vector((min(v.x for v in vs), min(v.y for v in vs), min(v.z for v in vs)))
        mx = Vector((max(v.x for v in vs), max(v.y for v in vs), max(v.z for v in vs)))
        ctr = Vector(((mn.x + mx.x) / 2, (mn.y + mx.y) / 2, mn.z))
        me.transform(Matrix.Translation(-ctr))
        tpl[o.name] = {"mesh": me, "dims": mx - mn}
    for o in imported:
        bpy.data.objects.remove(o, do_unlink=True)
    return tpl


def landmark_matrix(lm, dims):
    """Blender matrix mapping the normalised kit (base centred, Z up) onto the footprint OBB."""
    ux, uz = lm["obb"]["u"]
    vx, vz = uz, -ux  # right-handed: det(+1) keeps winding
    w, d = lm["obb"]["w"], lm["obb"]["d"]
    H = lm["h"]
    cx, cz = lm["obb"]["c"]
    k = lm.get("scale")
    if k is None:
        k = min(w / max(dims.x, 1e-3), d / max(dims.y, 1e-3))
    sx = sy = sz = k  # uniform: a kit building is never stretched
    M = Matrix.Identity(4)
    M.col[0] = Vector((ux * sx, -uz * sx, 0.0, 0.0))
    M.col[1] = Vector((vx * sy, -vz * sy, 0.0, 0.0))
    M.col[2] = Vector((0.0, 0.0, sz, 0.0))
    M.col[3] = Vector((cx, -cz, BASE_Y, 1.0))
    return M


def landmark_box(bk, key, lm, H):
    """Collision walls for a landmark footprint OBB."""
    cx, cz = lm["obb"]["c"]
    ux, uz = lm["obb"]["u"]
    w, d = lm["obb"]["w"], lm["obb"]["d"]
    emit_box(bk, key, (cx, cz), (ux, uz), w / 2, d / 2, BASE_Y, H, (0, 0, 1, 1))


# ---------------------------------------------------------------- object creation

def new_object(name, pos_flat, uv_flat, mat, scene_objs):
    nv = len(pos_flat) // 3
    nt = nv // 3
    me = bpy.data.meshes.new(name)
    me.vertices.add(nv)
    co = array.array("f", pos_flat)
    me.vertices.foreach_set("co", co)
    me.loops.add(nv)
    me.loops.foreach_set("vertex_index", array.array("i", range(nv)))
    me.polygons.add(nt)
    me.polygons.foreach_set("loop_start", array.array("i", range(0, nv, 3)))
    me.polygons.foreach_set("loop_total", array.array("i", [3] * nt))
    uvl = me.uv_layers.new(name="UVMap")
    uvl.data.foreach_set("uv", array.array("f", uv_flat))
    me.validate(clean_customdata=False)
    me.update()
    if mat is not None:
        me.materials.append(mat)
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    scene_objs.append(obj)
    return obj


def export_objects(objs, path):
    """Render chunks export as .gltf with EXTERNAL textures (GLTF_SEPARATE): each image is written
    once into the scenery folder and every chunk refers to it by a relative URI. Collision stays GLB."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    external = path.endswith(".gltf")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLTF_SEPARATE" if external else "GLB",
                              use_selection=True, export_yup=True,
                              export_image_format="AUTO", export_cameras=False, export_lights=False,
                              export_animations=False)
    set_glb_alpha_mask(path, "foliage", 0.5)


def set_glb_alpha_mask(path, mat_name, cutoff):
    """Blender 4.3 exports alpha-clipped materials as BLEND (its blend_method setter is ignored),
    so the glTF JSON chunk is rewritten to alphaMode MASK for the named material."""
    import struct
    if path.endswith(".gltf"):
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
        hit = False
        for m in doc.get("materials", []):
            if m.get("name") == mat_name:
                m["alphaMode"] = "MASK"
                m["alphaCutoff"] = cutoff
                hit = True
        if hit:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(doc, f, separators=(",", ":"))
        return
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 20:
        return
    magic, ver, _total = struct.unpack("<III", data[:12])
    jlen, jtype = struct.unpack("<II", data[12:20])
    doc = json.loads(data[20:20 + jlen].decode("utf-8"))
    hit = False
    for m in doc.get("materials", []):
        if m.get("name") == mat_name:
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = cutoff
            hit = True
    if not hit:
        return
    jb = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    jb += b" " * ((4 - len(jb) % 4) % 4)
    body = struct.pack("<II", len(jb), jtype) + jb + data[20 + jlen:]
    with open(path, "wb") as f:
        f.write(struct.pack("<III", magic, ver, 12 + len(body)) + body)


# ---------------------------------------------------------------- main

def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", default="models/shuto/rep_section/scenery_plan.json")
    ap.add_argument("--out", default="models/shuto/rep_section")
    ap.add_argument("--blend", default="models/shuto/src/rep_section_scenery.blend")
    ap.add_argument("--no-blend", action="store_true")
    args = ap.parse_args(argv)
    if bpy is None:
        raise SystemExit("build_scenery.py must run inside Blender (bpy missing)")

    random.seed(20021231)
    plan = json.load(open(resolve(args.plan), "r", encoding="utf-8"))
    out_dir = resolve(args.out)
    tex_dir = os.path.join(out_dir, "scenery_tex")

    # Clear the default scene.
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    cells_all = manifest_cells()
    units_man = json.load(open(FACADE_UNITS_MANIFEST, "r", encoding="utf-8"))
    bill_size, bill_cells = cells_all["billboards_art"]
    fam_units = {}
    for fam in ("apartment", "commercial", "industrial"):
        fm = units_man["families"][fam]
        W, H = fm["size_px"]
        up, gr = [], []
        for u in fm["units"]:
            x0, y0, x1, y1 = u["rect_px"]
            uv = (x0 / W, 1.0 - y1 / H, x1 / W, 1.0 - y0 / H)   # content rect, no inset (padded)
            (gr if u["ground"] else up).append(uv)
        fam_units[fam] = {"upper": up, "ground": gr or up}
    fol_size, fol_cells = cells_all["foliage_cutouts"]
    fol_name_uv = {c["cell"]: rect_uv(c["atlas_source_rect"], fol_size) for c in fol_cells}
    cont_size, cont_cells = cells_all["facades_industrial"]
    cont_uv = {c["cell"]: rect_uv(c["atlas_source_rect"], cont_size) for c in cont_cells}

    tex = make_textures(tex_dir, cells_all)
    kit_index = json.load(open(KIT_INDEX, "r", encoding="utf-8"))
    kit_names = sorted({k["kit"] for k in plan["landmarks"]})
    kit_tpl = load_kit_templates(set(kit_names), tex_dir) if kit_names else {}
    log("kit templates: %s" % sorted(kit_tpl))
    mats = make_materials(tex, kit_names)

    bk = Buckets()
    collision = Buckets()
    scene_objs = []

    # Water: rectangles at WATER_Y, plus its own material.
    for r in plan["water_rects"]:
        key = (r["cx"], r["cz"], "water")
        a = (r["x0"], WATER_Y, r["z0"])
        b = (r["x1"], WATER_Y, r["z0"])
        c = (r["x1"], WATER_Y, r["z1"])
        d = (r["x0"], WATER_Y, r["z1"])
        quad_nox(bk, key, a, b, c, d, (0, 0), (1, 0), (1, 1), (0, 1), (0.0, 1.0, 0.0))
    log("water rects emitted: %d" % len(plan["water_rects"]))

    # Buildings: one styled quad per wall (window grid from the style texture). Near buildings (within
    # NEAR_M of an expressway) add shopfront ground floors, balconies, cornice and rooftop units.
    STYLE_BY_NAME.clear()
    for sname, info in tex["styles"].items():
        STYLE_BY_NAME[sname] = info
    STYLE_BY_NAME["_grid"] = tex["_grid"]
    bill_uv = [rect_uv(c["atlas_source_rect"], bill_size) for c in bill_cells]
    near_n = 0
    for b in plan["buildings"]:
        fam = b["family"]
        ring = [(p[0], p[1]) for p in b["ring"]]
        h = b["h"]
        flags = b.get("flags") or {}
        near = b.get("near_m", 1e9) <= NEAR_M
        near_n += 1 if near else 0
        style = b.get("style") or "ivory_modern"
        tint = int(b.get("tint", 0))
        off = b.get("off") or [0, 0]
        roof_mat = "roof_corrugated" if flags.get("corrugated") else "roof_membrane"
        shop = "shopfront_retail" if near and fam != "industrial" else None
        if b["lod"] == "full" and flags.get("setback") and h >= 60.0:
            h1 = h * 0.7
            emit_styled_walls(bk, b["cx"], b["cz"], ring, h1, style, tint, off, BASE_Y, near, shop)
            emit_roof(bk, (b["cx"], b["cz"], roof_mat), ring, BASE_Y + h1, 4.0)
            inner = scale_ring(ring, 0.72)
            emit_styled_walls(bk, b["cx"], b["cz"], inner, h - h1, style, tint, [off[0] + 1, off[1]],
                              BASE_Y + h1, False, None)
            emit_roof(bk, (b["cx"], b["cz"], roof_mat), inner, BASE_Y + h, 4.0)
            top_ring = inner
            top = BASE_Y + h
        else:
            emit_styled_walls(bk, b["cx"], b["cz"], ring, h, style, tint, off, BASE_Y, near, shop)
            emit_roof(bk, (b["cx"], b["cz"], roof_mat), ring, BASE_Y + h, 4.0)
            top_ring = ring
            top = BASE_Y + h
        if near:
            emit_cornice(bk, (b["cx"], b["cz"], "roof_membrane"), top_ring, top)
            emit_roof_features(bk, b["cx"], b["cz"], top_ring, top, flags, (b["cx"], b["cz"], "roof_membrane"))
            if style.startswith("apartment"):
                emit_balconies(bk, b["cx"], b["cz"], ring, h, STYLE_BY_NAME[style]["storey_m"], BASE_Y)
            if flags.get("sign") and style.startswith(("spandrel", "office", "ivory")):
                emit_sign(bk, b["cx"], b["cz"], ring, h, BASE_Y, b["pick"], bill_uv)
        ckey = (b["cx"], b["cz"], "col")
        emit_building_walls(collision, ckey, ring, h, fam, "collision", 0,
                            {"upper": [(0, 0, 1, 1)], "ground": [(0, 0, 1, 1)]}, BASE_Y)
    log("buildings emitted: %d (near %d)" % (len(plan["buildings"]), near_n))

    # City streets at grade and street lamps.
    emit_street_pieces(bk, plan.get("streets", []))
    emit_street_lamps(bk, plan.get("street_lamps", []))

    # Landmarks: kit mesh instances (transformed copies) plus OBB collision.
    placements = []
    kit_by_name = {k["name"]: k for k in kit_index["buildings"]}
    lm_objs = []
    for lm in plan["landmarks"]:
        t = kit_tpl.get(lm["kit"])
        if t is None:
            continue
        M = landmark_matrix(lm, t["dims"])
        me = t["mesh"].copy()
        me.transform(M)
        obj = bpy.data.objects.new("lm_%s_%d_%d" % (lm["id"], lm["cx"], lm["cz"]), me)
        bpy.context.scene.collection.objects.link(obj)
        scene_objs.append(obj)
        lm_objs.append((lm, obj))
        landmark_box(collision, (lm["cx"], lm["cz"], "col"), lm, lm["h"])
        kd = kit_by_name.get(lm["kit"], {})
        placements.append({"id": lm["id"], "kind": "landmark", "building": lm["kit"],
                           "pos": [round(lm["obb"]["c"][0], 2), BASE_Y, round(lm["obb"]["c"][1], 2)],
                           "yaw_deg": round(math.degrees(math.atan2(lm["obb"]["u"][1], lm["obb"]["u"][0])), 2),
                           "scale": [round(lm["obb"]["w"], 2), round(lm["h"], 2), round(lm["obb"]["d"], 2)],
                           "source": lm["source"], "license": lm["license"], "author": lm["author"],
                           "footprint_m": kd.get("footprint_m"), "osm": lm["osm"]})
    log("landmarks placed: %d" % len(lm_objs))

    # Containers.
    for c in plan["containers"]:
        key = (c["cx"], c["cz"], "container")
        rect = cont_uv["red_container"] if int(c["id"][4:]) % 2 == 0 else cont_uv["teal_container"]
        roof_key = (c["cx"], c["cz"], "roof_membrane")
        ux, uz = c["u"]
        if abs(ux) < 1e-6 and abs(uz) < 1e-6:
            ux, uz = 1.0, 0.0
        cx, cz = c["pos"][0] + ux * CONTAINER_L / 2, c["pos"][1] + uz * CONTAINER_L / 2
        for level in range(c["stack"]):
            y0 = BASE_Y + level * CONTAINER_H
            emit_box(bk, key, (cx, cz), (ux, uz), CONTAINER_L / 2, CONTAINER_W / 2, y0, CONTAINER_H, rect,
                     roof_key if level == c["stack"] - 1 else None)
            ccol = (c["cx"], c["cz"], "col")
            if level == 0:
                emit_box(collision, ccol, (cx, cz), (ux, uz), CONTAINER_L / 2, CONTAINER_W / 2, BASE_Y,
                         CONTAINER_H * c["stack"], (0, 0, 1, 1))
    log("containers emitted: %d" % len(plan["containers"]))

    # Trees: two crossed foliage cards per tree.
    for t in plan["trees"]:
        key = (t["cx"], t["cz"], "foliage")
        yaw = math.radians(t["yaw"])
        s = t["size"]
        x, z = t["pos"]
        uv = fol_name_uv.get(t["cell"], fol_name_uv["shrub"])
        for k in range(2):
            a = yaw + k * math.pi / 2
            ux, uz = math.cos(a), math.sin(a)
            p0 = (x - ux * s / 2, BASE_Y, z - uz * s / 2)
            p1 = (x + ux * s / 2, BASE_Y, z + uz * s / 2)
            p2 = (x + ux * s / 2, BASE_Y + s, z + uz * s / 2)
            p3 = (x - ux * s / 2, BASE_Y + s, z - uz * s / 2)
            u_lo, v_lo, u_hi, v_hi = uv
            quad_nox(bk, key, p0, p1, p2, p3, (u_lo, v_lo), (u_hi, v_lo), (u_hi, v_hi), (u_lo, v_hi),
                     (0.0, 0.0, 1.0))
    log("trees emitted: %d" % len(plan["trees"]))

    # Remove stale scenery chunks (GLB from earlier passes, and the previous GLTF set).
    scen_dir = os.path.join(out_dir, "scenery")
    if os.path.isdir(scen_dir):
        for fn in os.listdir(scen_dir):
            if fn.endswith((".glb", ".gltf", ".bin", ".png", ".jpg")):
                os.remove(os.path.join(scen_dir, fn))

    # Build render objects per (chunk, material) and collision objects per chunk.
    chunk_tris = defaultdict(int)
    render_objs = defaultdict(list)
    for key in sorted(bk.pos.keys(), key=lambda k: (k[0], k[1], k[2])):
        cx, cz, mname = key
        obj = new_object("r_%d_%d_%s" % (cx, cz, mname), bk.pos[key], bk.uv[key], mats[mname], scene_objs)
        render_objs[(cx, cz)].append(obj)
        chunk_tris[(cx, cz)] += bk.tris[key]
    col_objs = defaultdict(list)
    col_tris = defaultdict(int)
    for key in sorted(collision.pos.keys(), key=lambda k: (k[0], k[1])):
        cx, cz, _ = key
        obj = new_object("c_%d_%d" % (cx, cz), collision.pos[key], collision.uv[key], mats["collision"], scene_objs)
        col_objs[(cx, cz)].append(obj)
        col_tris[(cx, cz)] += collision.tris[key]
    for lm, obj in lm_objs:
        render_objs[(lm["cx"], lm["cz"])].append(obj)
        chunk_tris[(lm["cx"], lm["cz"])] += len(obj.data.polygons)
    total_render = sum(chunk_tris.values())
    total_col = sum(col_tris.values())
    log("render triangles total %d, collision triangles %d" % (total_render, total_col))

    chunks_meta = []
    for (cx, cz) in sorted(set(render_objs) | set(col_objs)):
        rel_r = os.path.join("scenery", "%d_%d.gltf" % (cx, cz))
        rel_c = os.path.join("scenery_collision", "%d_%d_col.glb" % (cx, cz))
        rs = render_objs.get((cx, cz), [])
        cs = col_objs.get((cx, cz), [])
        if rs:
            export_objects(rs, os.path.join(out_dir, rel_r))
        if cs:
            export_objects(cs, os.path.join(out_dir, rel_c))
        chunks_meta.append({"cx": cx, "cz": cz, "bounds_m": [cx * 500, cz * 500, (cx + 1) * 500, (cz + 1) * 500],
                            "render": rel_r.replace(os.sep, "/") if rs else None,
                            "collision": rel_c.replace(os.sep, "/") if cs else None,
                            "render_tris": chunk_tris.get((cx, cz), 0), "collision_tris": col_tris.get((cx, cz), 0)})
        log("chunk %d_%d: render %d tris, collision %d tris" % (cx, cz, chunk_tris.get((cx, cz), 0),
                                                              col_tris.get((cx, cz), 0)))

    # Placements (buildings, containers, trees are placed as records; landmarks above).
    for b in plan["buildings"]:
        o = b["obb"]
        placements.append({"id": b["id"], "kind": "osm_mass", "lod": b["lod"], "family": b["family"],
                           "pos": [o["c"][0], BASE_Y, o["c"][1]],
                           "yaw_deg": round(math.degrees(math.atan2(o["u"][1], o["u"][0])), 2),
                           "scale": [o["w"], b["h"], o["d"]],
                           "source": "OpenStreetMap building footprint (procedural extrusion)",
                           "license": "ODbL-1.0 (c) OpenStreetMap contributors", "osm": b["osm"]})
    for c in plan["containers"]:
        placements.append({"id": c["id"], "kind": "container", "pos": [c["pos"][0], BASE_Y, c["pos"][1]],
                           "yaw_deg": round(math.degrees(math.atan2(c["u"][1], c["u"][0])), 2),
                           "scale": [CONTAINER_L, CONTAINER_H * c["stack"], CONTAINER_W],
                           "source": "procedural container box", "license": "project-internal"})
    meta = {
        "format": "nox-shuto-scenery",
        "version": 1,
        "name": "rep_section",
        "plan": os.path.relpath(resolve(args.plan), ROOT).replace(os.sep, "/"),
        "chunk_m": plan["chunk_m"],
        "ground_y": plan["ground_y"],
        "water_y": WATER_Y,
        "base_y": BASE_Y,
        "bbox": plan["bbox"],
        "materials": sorted(mats.keys()),
        "textures": {k: os.path.relpath(v, ROOT).replace(os.sep, "/") if isinstance(v, str) else None
                     for k, v in tex.items() if isinstance(v, str)},
        "totals": {"render_tris": total_render, "collision_tris": total_col,
                   "buildings": len(plan["buildings"]), "near_buildings": near_n,
                   "landmarks": len(lm_objs), "containers": len(plan["containers"]),
                   "trees": len(plan["trees"]), "water_rects": len(plan["water_rects"])},
        "chunks": chunks_meta,
        "sources": plan["sources"],
        "attribution": [
            "(c) OpenStreetMap contributors, ODbL-1.0 (building footprints, heights, water, parks).",
            "Kit buildings bld_asian_themed_* by 99.Miles, CC-BY-4.0, https://sketchfab.com/3d-models/asian-themed-low-poly-night-city-buildings-9f0343aff4814b758dc6e905aba5b5e0 (modified: scaled to footprints, baked transforms).",
        ],
        "placements": placements,
    }
    with open(os.path.join(out_dir, "scenery.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, separators=(",", ":"))
    log("wrote scenery.json with %d placements" % len(placements))

    if not args.no_blend:
        blend_path = resolve(args.blend)
        os.makedirs(os.path.dirname(blend_path), exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=blend_path, compress=True)
        log("saved " + blend_path)


if __name__ == "__main__":
    main()
