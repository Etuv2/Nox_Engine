"""Editable Japanese architecture, NOX parallax panes, LODs and external-texture GLBs.

blender -b --factory-startup --python-exit-code 1 --python this_file -- --mode probe|kit|section|context
Run probe and verify it in NOX before kit/section. Originals are never written.
"""
import argparse
import array
from collections import defaultdict
import json
import math
from pathlib import Path
import random
import struct
import sys

import bpy
from mathutils import Matrix, Vector

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import build_scenery as g

OUT = ROOT / "models/shuto/buildings"
TEX = OUT / "textures"
BASE = 2.9
ROOMS = {}
MATS = {}
SPHERE_CENTERS = {}


def save_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def material(name, color, rough=0.8, metal=0, image=None, emit=0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes.get("Principled BSDF")
    p.inputs["Base Color"].default_value = (*color, 1)
    p.inputs["Roughness"].default_value = rough
    p.inputs["Metallic"].default_value = metal
    if image:
        t = m.node_tree.nodes.new("ShaderNodeTexImage")
        t.image = bpy.data.images.load(str(image), check_existing=True)
        t.extension = "REPEAT" if "patch_" not in str(image) else "EXTEND"
        m.node_tree.links.new(t.outputs["Color"], p.inputs["Base Color"])
    if emit:
        p.inputs["Emission Color"].default_value = (1, 1, 1, 1)
        p.inputs["Emission Strength"].default_value = emit
    MATS[name] = m
    return m


def materials():
    for i, c in enumerate([(0.72, 0.70, 0.62), (0.50, 0.55, 0.56), (0.60, 0.44, 0.35), (0.78, 0.77, 0.72)]):
        material("wall_%d" % i, c, image=TEX / ("concrete_weathered.png" if i == 2 else "concrete_cast.png"))
    material("roof", (0.48, 0.49, 0.48), image=TEX / "rooftop_membrane.png")
    material("corrugated", (0.58, 0.61, 0.58), image=TEX / "corrugated_metal.png")
    material("steel", (0.65, 0.65, 0.66), rough=0.38, metal=1, image=TEX / "galvanized_steel.png")
    material("frame", (0.09, 0.10, 0.10), rough=0.4, metal=0.7)
    material("glass_dark", (0.035, 0.055, 0.065), rough=0.18)
    material("window_lit", (0.07, 0.065, 0.045), rough=0.18, emit=1.5)
    material("collision", (0.5, 0.1, 0.1))
    # Pale aluminium cladding per the reference photo; mostly dielectric so it stays readable at night.
    material("fuji_aluminium", (0.66, 0.68, 0.70), rough=0.42, metal=0.15)
    material("fuji_panel_gray", (0.50, 0.53, 0.56), rough=0.55, metal=0.1)
    material("fuji_glass", (0.035, 0.058, 0.071), rough=0.15, metal=0.15)
    material("fuji_window_warm", (0.21, 0.17, 0.10), rough=0.3, emit=0.4)
    material("fuji_sphere_titanium", (0.72, 0.70, 0.66), rough=0.3, metal=1)
    material("fuji_sphere_glass", (0.028, 0.04, 0.051), rough=0.12, metal=0.2)
    for style, name in enumerate(["apartment", "shop", "office"]):
        material("room_" + name, (0.035, 0.04, 0.045), rough=0.14, emit=2.5)
        ROOMS["room_" + name] = {"depth": 4.5 if style != 1 else 6.0, "strength": 1,
                                  "style": style, "lighting": 0.26 if style != 1 else 0.65,
                                  "blinds": 0.28 if style != 1 else 0.06,
                                  "pane": [0.8, 1.0], "sill": 0.75, "seed": style + 0.25}
        MATS["room_" + name]["nox"] = {"interior": ROOMS["room_" + name]}
    for name in ["minato_coffee", "aoba_store", "shibaura_storage"]:
        material("sign_" + name, (1, 1, 1), rough=0.7, image=TEX / ("sign_" + name + ".png"))
    # Placed chunks use one stacked sign atlas (rows in SIGN_ROWS order) to keep one sign primitive per chunk.
    material("sign_atlas", (1, 1, 1), rough=0.7, image=TEX / "sign_atlas.png")
    # Existing facade styles retain coherent bay/storey mapping for mid/far geometry.
    sm = json.loads((ROOT / "textures/shuto/derived/facade_styles/manifest.json").read_text(encoding="utf-8"))
    styles = sm["styles"]
    if isinstance(styles, list):
        styles = {s["name"]: s for s in styles}
    g.STYLE_BY_NAME.clear()
    g.STYLE_BY_NAME["_grid"] = (16, 12)
    for name, info in styles.items():
        g.STYLE_BY_NAME[name] = info
        for tint in range(3):
            key = "style_%s_t%d" % (name, tint)
            p = ROOT / "textures/shuto/derived/facade_styles" / (name + "_base.jpg")
            if not p.exists():
                p = ROOT / "textures/shuto/derived/facade_styles" / info["base"]
            m = material(key, g.TINTS[tint], rough=0.7, image=p)
            ep = p.with_name(name + "_emit.jpg")
            if ep.exists():
                n = m.node_tree.nodes.new("ShaderNodeTexImage")
                n.image = bpy.data.images.load(str(ep), check_existing=True)
                bsdf = m.node_tree.nodes.get("Principled BSDF")
                m.node_tree.links.new(n.outputs["Color"], bsdf.inputs["Emission Color"])
                bsdf.inputs["Emission Strength"].default_value = 1.5


SIGN_ROWS = ["minato_coffee", "aoba_store", "shibaura_storage"]


def sign_uv(name):
    """UVs of one sign row in sign_atlas.png (Blender V up; row 0 is the top of the image)."""
    row = SIGN_ROWS.index(name)
    inset = 4 / 768
    v0, v1 = (2 - row) / 3 + inset, (3 - row) / 3 - inset
    return [(0, v0), (1, v0), (1, v1), (0, v1)]


def panel(bk, key, a, b, y0, y1, normal, offset=0, uv=None):
    nx, _, nz = normal
    a, b = (a[0] + nx * offset, a[1] + nz * offset), (b[0] + nx * offset, b[1] + nz * offset)
    if uv is None:
        length = math.dist(a, b)
        uv = [(0, 0), (length / 3, 0), (length / 3, (y1 - y0) / 3), (0, (y1 - y0) / 3)]
    if key[-1].startswith("sign_") and (b[0] - a[0]) * nz - (b[1] - a[1]) * nx < 0:
        uv = [(1 - u, v) for u, v in uv]
    g.quad_nox(bk, key, (a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1]), *uv, normal)


def normal(a, b, ring):
    length = math.dist(a, b)
    sign = 1 if g.signed_area(ring) > 0 else -1
    return ((b[1] - a[1]) / length * sign, 0, -(b[0] - a[0]) / length * sign)


def add_box(bk, key, c, u, w, d, y, h):
    ux, uz = u
    ring = [(c[0] + ux * x + uz * z, c[1] + uz * x - ux * z)
            for x, z in [(-w / 2, -d / 2), (w / 2, -d / 2), (w / 2, d / 2), (-w / 2, d / 2)]]
    for p, q, _ in g.wall_edges(ring):
        panel(bk, key, p, q, y, y + h, normal(p, q, ring))
    g.emit_roof(bk, key, ring, y + h, 3)
    bottom(bk, key, ring, y)


def bottom(bk, key, ring, y):
    for i, j, k in g.triangulate(ring):
        pts = [(ring[v][0], y, ring[v][1]) for v in (i, j, k)]
        g.tri_nox(bk, key, *pts, (0, 0), (1, 0), (0, 1), (0, -1, 0))


def upper_style(style, family):
    if style in g.STYLE_BY_NAME:
        return style
    return "apartment_concrete" if family == "apartment" else "warehouse_clerestory" if family == "industrial" else "office_ribbon"


def rear_wall(bk, keyid, a, b, length, n, y0, y1, pitch, style, family):
    """Wall facing away from every carriageway: one shared mid/far facade patch (2 triangles)."""
    bay = 6 if family == "industrial" else 3.2
    uv = [(0, 0), (length / bay / 16, 0), (length / bay / 16, (y1 - y0) / pitch / 12), (0, (y1 - y0) / pitch / 12)]
    panel(bk, (keyid, "style_%s_t1" % upper_style(style, family)), a, b, y0, y1, n, uv=uv)


def pane_groups(bay, panes):
    """Pane left offsets inside one bay: 1.6 m panes; paired panes share a 0.12 m mullion."""
    k = panes if bay >= panes * 1.6 + (panes - 1) * 0.12 + 0.5 else 1
    group = k * 1.6 + (k - 1) * 0.12
    return [(bay - group) / 2 + i * 1.72 for i in range(k)]


def detailed_walls(bk, keyid, ring, h, pitch, family, seed, ybase, balconies=True, front_edge=None,
                   edges=None, facade=None, style=None, wall=None):
    facade = facade or {}
    wall = "wall_%d" % (seed % 4 if wall is None else wall)
    floors = max(1, int(h / pitch + 1e-5))
    detailed_floors = min(floors, 9)
    bay_target = facade.get("bay", 3.2 if family != "industrial" else 6.0)
    panes = facade.get("panes", 1)
    sill = min(facade.get("sill", 0.85), pitch - 2.15)
    all_edges = list(g.wall_edges(ring))
    if front_edge is None:
        front_edge = edges[0] if edges else seed % len(ring)
    for edge, (a, b, length) in enumerate(all_edges):
        n = normal(a, b, ring)
        if edges is not None and edge not in edges:
            rear_wall(bk, keyid, a, b, length, n, ybase, ybase + h, pitch, style, family)
            continue
        u = ((b[0] - a[0]) / length, (b[1] - a[1]) / length)

        def pt(s):
            return (a[0] + u[0] * s, a[1] + u[1] * s)
        bays = max(1, int(length / bay_target))
        bay = length / bays
        lefts = pane_groups(bay, panes)
        for f in range(detailed_floors):
            y = ybase + f * pitch
            # Fixed 1.6 x 2.0 m panes and a fixed storey pitch; corner infill absorbs footprint variation.
            if bay < 2.25:
                panel(bk, (keyid, wall), a, b, y, y + pitch, n)
                continue
            low = 0.30 if f == 0 else sill
            high = low + 2.0
            panel(bk, (keyid, wall), a, b, y, y + low, n)
            panel(bk, (keyid, wall), a, b, y + high, y + pitch, n)
            for j in range(bays):
                x0 = j * bay
                panel(bk, (keyid, wall), pt(x0), pt(x0 + lefts[0]), y + low, y + high, n)
                for i in range(len(lefts) - 1):
                    # Paired panes: flush mullion between the two openings.
                    panel(bk, (keyid, "frame"), pt(x0 + lefts[i] + 1.6), pt(x0 + lefts[i + 1]), y + low, y + high, n)
                panel(bk, (keyid, wall), pt(x0 + lefts[-1] + 1.6), pt((j + 1) * bay), y + low, y + high, n)
                for left_offset in lefts:
                    left, right = x0 + left_offset, x0 + left_offset + 1.6
                    # Recess returns, not painted shadows. Pane is 14 cm behind the facade plane.
                    pa, pb = pt(left), pt(right)
                    ra = (pa[0] - n[0] * 0.14, pa[1] - n[2] * 0.14)
                    rb = (pb[0] - n[0] * 0.14, pb[1] - n[2] * 0.14)
                    panel(bk, (keyid, wall), pa, ra, y + low, y + high, (u[0], 0, u[1]))
                    panel(bk, (keyid, wall), rb, pb, y + low, y + high, (-u[0], 0, -u[1]))
                    for yy, ny in [(y + low, 1), (y + high, -1)]:
                        g.quad_nox(bk, (keyid, wall), (pa[0], yy, pa[1]), (pb[0], yy, pb[1]),
                                   (rb[0], yy, rb[1]), (ra[0], yy, ra[1]), (0, 0), (1, 0), (1, 1), (0, 1), (0, ny, 0))
                    room = "room_shop" if f == 0 and family == "commercial" else "room_office" if family == "commercial" else "room_apartment"
                    if family == "industrial":
                        room = "frame"     # plain dark industrial glazing shares the sash material
                    # Exporter flips Blender V; the four UVs become one 0.242 NOX tile.
                    panel(bk, (keyid, room), pa, pb, y + low, y + high, n, -0.14,
                          [(0.004, 0.004), (0.246, 0.004), (0.246, 0.246), (0.004, 0.246)])
                    # Sash frame and narrow mullion, kept outside glass without covering the opening.
                    for lo, hi in [(left - 0.05, left + 0.025), (right - 0.025, right + 0.05),
                                   ((left + right) / 2 - 0.025, (left + right) / 2 + 0.025)]:
                        panel(bk, (keyid, "frame"), pt(lo), pt(hi), y + low, y + high, n, -0.11)
                    for yy in [y + low, y + high - 0.06]:
                        panel(bk, (keyid, "frame"), pa, pb, yy, yy + 0.06, n, -0.11)
            if balconies and family == "apartment" and f > 0 and edge == front_edge:
                # Planner reserves 1.5 m slack beyond the corridor; projection is limited to 1.1 m.
                c = pt(length / 2)
                c = (c[0] + n[0] * 0.55, c[1] + n[2] * 0.55)
                add_box(bk, (keyid, wall), c, u, length - 0.4, 1.1, y - 0.1, 0.13)
                rail_a, rail_b = pt(0.2), pt(length - 0.2)
                panel(bk, (keyid, "steel"), rail_a, rail_b, y + 0.95, y + 1.0, n, 1.07)
                panel(bk, (keyid, "steel"), rail_a, rail_b, y + 0.3, y + 0.34, n, 1.07)
                for j in range(max(2, int(length / 0.8))):
                    s = 0.25 + (length - 0.5) * j / max(1, int(length / 0.8) - 1)
                    panel(bk, (keyid, "steel"), pt(s - 0.018), pt(s + 0.018), y, y + 1.0, n, 1.07)
        detailed_h = detailed_floors * pitch
        if h > detailed_h + 0.01:
            # Upper storeys retain the same physical pitch and the building's own facade style.
            uv = [(0, 0), (bays / 16, 0), (bays / 16, (h - detailed_h) / pitch / 12), (0, (h - detailed_h) / pitch / 12)]
            panel(bk, (keyid, "style_%s_t1" % upper_style(style, family)), a, b, ybase + detailed_h, ybase + h, n, uv=uv)
    # Ground entrance and canopy on the front (road-facing) edge; all added parts remain inside footprint.
    if 0 <= front_edge < len(all_edges):
        a, b, length = all_edges[front_edge]
    else:
        a, b, length = max(all_edges, key=lambda e: e[2])
    n = normal(a, b, ring)
    u = ((b[0] - a[0]) / length, (b[1] - a[1]) / length)
    c = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
    add_box(bk, (keyid, "steel"), (c[0] + n[0] * 0.55, c[1] + n[2] * 0.55), u, min(length - 0.2, 3.4), 1.1, ybase + 2.7, 0.10)
    if family != "apartment" and length >= 6:
        sign = "shibaura_storage" if family == "industrial" else "minato_coffee" if seed % 2 else "aoba_store"
        pa = (c[0] - u[0] * 1.9, c[1] - u[1] * 1.9)
        pb = (c[0] + u[0] * 1.9, c[1] + u[1] * 1.9)
        panel(bk, (keyid, "sign_atlas"), pa, pb, ybase + 2.9, ybase + 3.85, n, 0.015, sign_uv(sign))


def lightweight_windows(bk, key, b, base):
    ring, h, pitch, seed = b["ring"], b["h"], b["storey_m"], b["seed"]
    family = b["family"]
    wall = "wall_%d" % b.get("wall", seed % 4)
    facade = b.get("facade", {})
    edges = b.get("road_edges")
    style = b.get("style", "office_ribbon")
    for edge, (p, q, length) in enumerate(g.wall_edges(ring)):
        n = normal(p, q, ring)
        if edges is not None and edge not in edges:
            rear_wall(bk, key, p, q, length, n, base, base + h, pitch, style, family)
            continue
        u = ((q[0] - p[0]) / length, (q[1] - p[1]) / length)

        def along(s):
            return (p[0] + u[0] * s, p[1] + u[1] * s)
        panel(bk, (key, wall), p, q, base, base + min(h, pitch * 9), n)
        bays = max(1, int(length / facade.get("bay", 6 if family == "industrial" else 3.2)))
        bay = length / bays
        lefts = pane_groups(bay, facade.get("panes", 1))
        sill = min(facade.get("sill", 0.85), pitch - 2.15)
        if bay >= 2.25:
            for f in range(min(9, int(h / pitch + 1e-5))):
                y = base + f * pitch + (0.3 if f == 0 else sill)
                room = "room_shop" if f == 0 and family == "commercial" else "room_office" if family == "commercial" else "room_apartment"
                if family == "industrial":
                    room = "frame"
                for j in range(bays):
                    for left in lefts:
                        c = j * bay + left + 0.8
                        panel(bk, (key, "frame"), along(c - 0.84), along(c + 0.84), y - 0.04, y + 2.04, n, 0.02)
                        panel(bk, (key, room), along(c - 0.8), along(c + 0.8), y, y + 2, n, 0.045,
                              [(0.004, 0.004), (0.246, 0.004), (0.246, 0.246), (0.004, 0.246)])
                front = b.get("front_edge", edges[0] if edges else seed % len(ring))
                if f > 0 and family == "apartment" and facade.get("balconies", True) and edge == front:
                    c = along(length / 2)
                    add_box(bk, (key, wall), (c[0] + n[0] * 0.55, c[1] + n[2] * 0.55), u,
                            length - 0.4, 1.1, base + f * pitch - 0.1, 0.13)
                    for low, high in ((0.3, 0.34), (0.95, 1.0)):
                        panel(bk, (key, "steel"), along(0.2), along(length - 0.2), base + f * pitch + low,
                              base + f * pitch + high, n, 1.07)
        if b.get("kind") == "infill_building" and family == "commercial" and edge == 0 and length >= 6 and pitch >= 3.5:
            # Fascia sign above the shop windows, readable from the expressway side.
            c = along(length / 2)
            sign = "minato_coffee" if seed % 2 else "aoba_store"
            panel(bk, (key, "sign_atlas"), (c[0] - u[0] * 1.9, c[1] - u[1] * 1.9), (c[0] + u[0] * 1.9, c[1] + u[1] * 1.9),
                  base + 2.45, base + 3.4, n, 0.06, sign_uv(sign))
        if h > pitch * 9:
            uv = [(0, 0), (length / 3.2 / 16, 0), (length / 3.2 / 16, (h - pitch * 9) / pitch / 12), (0, (h - pitch * 9) / pitch / 12)]
            panel(bk, (key, "style_%s_t1" % upper_style(style, family)), p, q, base + pitch * 9, base + h, n, uv=uv)


def yard(b, base=BASE):
    """Fenced yard beside the expressway: low block wall, steel posts and rails, small site hut."""
    bk = g.Buckets()
    key, ring = b["id"], b["ring"]
    wall = "wall_%d" % b.get("wall", 2)
    for p, q, length in g.wall_edges(ring):
        n = normal(p, q, ring)
        inward = (-n[0], 0, -n[2])
        u = ((q[0] - p[0]) / length, (q[1] - p[1]) / length)
        panel(bk, (key, wall), p, q, base, base + 0.6, n)
        panel(bk, (key, wall), q, p, base, base + 0.6, inward, 0.18)
        g.quad_nox(bk, (key, wall), (p[0], base + 0.6, p[1]), (q[0], base + 0.6, q[1]),
                   (q[0] - n[0] * 0.18, base + 0.6, q[1] - n[2] * 0.18), (p[0] - n[0] * 0.18, base + 0.6, p[1] - n[2] * 0.18),
                   (0, 0), (length / 3, 0), (length / 3, 0.06), (0, 0.06), (0, 1, 0))
        for lo, hi in ((1.15, 1.2), (1.72, 1.78)):
            panel(bk, (key, "steel"), p, q, base + lo, base + hi, n, -0.09)
            panel(bk, (key, "steel"), q, p, base + lo, base + hi, inward, 0.09)
        posts = max(2, int(length / 2.5) + 1)
        for k in range(posts):
            s = min(max(length * k / (posts - 1), 0.04), length - 0.04)
            c = (p[0] + u[0] * s, p[1] + u[1] * s)
            pa, pb = (c[0] - u[0] * 0.04, c[1] - u[1] * 0.04), (c[0] + u[0] * 0.04, c[1] + u[1] * 0.04)
            panel(bk, (key, "frame"), pa, pb, base + 0.6, base + 1.8, n, -0.08)
            panel(bk, (key, "frame"), pb, pa, base + 0.6, base + 1.8, inward, 0.1)
    # Site hut set back from the road edge, inside the yard.
    o = b["obb"]
    ux, uz = o["u"]
    hut = (o["c"][0] + uz * o["d"] * 0.2 + ux * o["w"] * 0.25, o["c"][1] - ux * o["d"] * 0.2 + uz * o["w"] * 0.25)
    add_box(bk, (key, wall), hut, (ux, uz), 3.6, 2.4, base, 2.6)
    return bk


def roof_equipment(bk, key, b, base, near):
    """Roof plant varies with the cached flags: stair housing, AC condensers and an elevated water tank."""
    ring, h, seed = b["ring"], b["h"], b["seed"]
    flags = b.get("flags", {})
    wall = "wall_%d" % b.get("wall", seed % 4)
    c = g.interior_point(ring)
    ux, uz = b["obb"]["u"]
    def inside(x, z, r):
        return all(g.point_in_ring2(x + dx, z + dz, ring) for dx, dz in ((r, r), (r, -r), (-r, r), (-r, -r)))
    if flags.get("stair"):
        if inside(c[0], c[1], 1.6):
            add_box(bk, (key, wall), c, (ux, uz), 3.0, 2.6, base + h, 2.6)
    elif inside(c[0], c[1], 1.4):
        add_box(bk, (key, wall), c, (ux, uz), 2.8, 2.4, base + h, 2.2)
    units = min(flags.get("ac", 1), 3 if near else 2)
    for i in range(units):
        x, z = c[0] + ux * (2.4 + i * 1.7), c[1] + uz * (2.4 + i * 1.7)
        if not inside(x, z, 0.8):
            break
        add_box(bk, (key, "steel"), (x, z), (ux, uz), 1.4, 0.8, base + h, 0.65)
        if near:
            # Slatted AC condenser face rather than a featureless cube.
            fx, fz = x - uz * 0.41, z + ux * 0.41
            for k in range(5):
                panel(bk, (key, "frame"), (fx - ux * 0.65, fz - uz * 0.65), (fx + ux * 0.65, fz + uz * 0.65),
                      base + h + 0.08 + k * 0.1, base + h + 0.105 + k * 0.1, (uz, 0, -ux))
    if (flags.get("tank") or seed % 3 == 0) and b["detail"] in ("near", "window_mid"):
        x, z = c[0] - ux * 2.6, c[1] - uz * 2.6
        if inside(x, z, 1.0):
            cylinder(bk, (key, "steel"), (x, base + h + 0.8, z), 0.65, 1.6)
            add_box(bk, (key, "steel"), (x, z), (ux, uz), 1.3, 1.3, base + h, 0.8)


def building(b, lod=0, base=BASE):
    if b.get("landmark"):
        if b["landmark"] == "fuji" and b["detail"] == "mid" and lod == 0:
            lod = 1
        return landmark(b, lod, base)
    if b.get("kind") == "infill_yard":
        return yard(b, base)
    bk = g.Buckets()
    ring = b["ring"]
    h = b["h"]
    seed = b["seed"]
    key = b["id"]
    near = b["detail"] == "near" and lod == 0
    window_mid = (b["detail"] == "window_mid" and lod == 0) or (b["detail"] == "near" and lod == 1)
    family = b["family"]
    wall = "wall_%d" % b.get("wall", seed % 4)
    # Rectangular tower modules have a floor-aligned setback, rather than scaled windows.
    if h >= 60 and len(ring) == 4 and lod < 2:
        lower = dict(b, h=math.floor(h * 0.72 / b["storey_m"]) * b["storey_m"], skirt=b.get("skirt"))
        upper = dict(b, ring=g.scale_ring(ring, 0.76), h=h - lower["h"], detail="mid", skirt=False,
                     road_edges=None, flags=dict(b.get("flags", {}), stair=True))
        low_bk = building(lower, lod, base)
        high_bk = building(upper, lod, base + lower["h"])
        for data in [low_bk, high_bk]:
            for k in data.pos:
                bk.pos[k].extend(data.pos[k])
                bk.uv[k].extend(data.uv[k])
                bk.tris[k] += data.tris[k]
        return bk
    style = b.get("style", "office_ribbon")
    if style not in g.STYLE_BY_NAME:
        style = "warehouse_clerestory" if family == "industrial" else "apartment_concrete" if family == "apartment" else "office_ribbon"
    if near:
        facade = b.get("facade", {})
        detailed_walls(bk, key, ring, h, b["storey_m"], family, seed, base, balconies=facade.get("balconies", True),
                       front_edge=b.get("front_edge"), edges=b.get("road_edges"), facade=facade, style=style,
                       wall=b.get("wall"))
    elif window_mid:
        lightweight_windows(bk, key, b, base)
    else:
        for a, z, length in g.wall_edges(ring):
            n = normal(a, z, ring)
            bay = 3.2 if family != "industrial" else 6
            uv = [(0, 0), (length / bay / 16, 0), (length / bay / 16, h / b["storey_m"] / 12), (0, h / b["storey_m"] / 12)]
            panel(bk, (key, "style_%s_t1" % style), a, z, base, base + h, n, uv=uv)
    if b.get("skirt") and base == BASE:
        # Beyond the exported ground the base continues down to y -1.5 instead of ending in mid-air.
        skirt = wall if (near or window_mid) else "style_%s_t1" % style
        for a, z, length in g.wall_edges(ring):
            uv = None if skirt == wall else [(0, -4.4 / b["storey_m"] / 12), (length / 3.2 / 16, -4.4 / b["storey_m"] / 12),
                                             (length / 3.2 / 16, 0), (0, 0)]
            panel(bk, (key, skirt), a, z, BASE - 4.4, BASE, normal(a, z, ring), uv=uv)
    roof = "corrugated" if family == "industrial" else "roof"
    g.emit_roof(bk, (key, roof), ring, base + h, 3)
    if lod < 2 and b["detail"] != "far":
        parapet = b.get("facade", {}).get("parapet", 0.55)
        for a, z, length in g.wall_edges(ring):
            panel(bk, (key, wall), a, z, base + h, base + h + parapet, normal(a, z, ring))
        if min(b["obb"]["w"], b["obb"]["d"]) > 7:
            roof_equipment(bk, key, b, base, near)
    return bk


def site_pad(bk, key, pad):
    """Paved site block beyond the exported ground; vertical quay edge down to bottom_y."""
    ring = pad["ring"]
    g.emit_roof(bk, key, ring, pad["top_y"], 4)
    for a, z, length in g.wall_edges(ring):
        panel(bk, key, a, z, pad["bottom_y"], pad["top_y"], normal(a, z, ring))


def cylinder(bk, key, c, radius, height, sides=12):
    for i in range(sides):
        a, z = i * 2 * math.pi / sides, (i + 1) * 2 * math.pi / sides
        p, q = (c[0] + radius * math.cos(a), c[2] + radius * math.sin(a)), (c[0] + radius * math.cos(z), c[2] + radius * math.sin(z))
        panel(bk, key, p, q, c[1], c[1] + height, (math.cos((a + z) / 2), 0, math.sin((a + z) / 2)))
        g.tri_nox(bk, key, (c[0], c[1] + height, c[2]), (p[0], c[1] + height, p[1]),
                  (q[0], c[1] + height, q[1]), (0.5, 0.5), (0, 0), (1, 0), (0, 1, 0))


def landmark(b, lod, base):
    bk = g.Buckets()
    key = b["id"]
    if b["landmark"] == "dentsu":
        ring = b["ring"]
        for a, z, length in g.wall_edges(ring):
            n = normal(a, z, ring)
            uv = [(0, 0), (length / 3.2 / 16, 0), (length / 3.2 / 16, 48 / 12), (0, 48 / 12)]
            panel(bk, (key, "style_curtain_mullion_t1"), a, z, base, base + b["h"], n, uv=uv)
            if lod < 2:
                for f in range(1, 48):
                    yy = base + f * b["h"] / 48
                    panel(bk, (key, "steel"), a, z, yy, yy + 0.12, n, 0.03)
                for j in range(max(1, int(length / 6))):
                    u = ((z[0] - a[0]) / length, (z[1] - a[1]) / length)
                    s = (j + 0.5) * length / max(1, int(length / 6))
                    p = (a[0] + u[0] * s, a[1] + u[1] * s)
                    add_box(bk, (key, "steel"), p, u, 0.12, 0.18, base, b["h"])
        g.emit_roof(bk, (key, "roof"), ring, base + b["h"], 4)
        return bk
    import fuji_tv
    return fuji_tv.build(b, lod, base, sys.modules[__name__])


def objects_from(bk, prefix, collection=None):
    objects = []
    for key in sorted(bk.pos):
        obj = g.new_object(prefix + "_" + key[-1], bk.pos[key], bk.uv[key], MATS[key[-1]], [])
        if key[-1].startswith("sign_"):
            obj.data["nox_sign_orientation"] = "outward_readable"
        if key[-1].startswith("fuji_sphere_"):
            center = SPHERE_CENTERS.get(key[0])
            if center is None:
                center = next(iter(SPHERE_CENTERS.values()))
            obj.data.normals_split_custom_set_from_vertices([(v.co - center).normalized() for v in obj.data.vertices])
        if collection:
            for old in list(obj.users_collection):
                old.objects.unlink(obj)
            collection.objects.link(obj)
        objects.append(obj)
    return objects


def export(objs, path):
    """Use Blender's exporter, then pack binary buffers while retaining shared image URIs."""
    path.parent.mkdir(parents=True, exist_ok=True)
    for obj in bpy.data.objects:
        if obj.select_get():
            obj.select_set(False)
    for obj in objs:
        obj.hide_set(False)
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    temporary = path.with_suffix(".gltf")
    bpy.ops.export_scene.gltf(filepath=str(temporary), export_format="GLTF_SEPARATE", use_selection=True,
                              export_yup=True, export_tangents=True, export_animations=False,
                              export_cameras=False, export_lights=False, export_extras=True,
                              export_texture_dir=str(TEX.relative_to(OUT)) if path.parent == OUT else "../textures")
    doc = json.loads(temporary.read_text(encoding="utf-8"))
    doc["asset"].setdefault("extras", {})["nox_sign_u_right"] = True
    buffer_path = temporary.parent / doc["buffers"][0].pop("uri")
    binary = bytearray(buffer_path.read_bytes())
    for mat in doc.get("materials", []):
        name = mat.get("name", "")
        if name in ROOMS:
            mat.setdefault("extras", {}).setdefault("nox", {})["interior"] = ROOMS[name]
    # Blender 4.3 exports the image-up tangent basis used by NOX's pane contract.
    # Preserve it: changing handedness makes the window seed vary across a single pane.
    encoded = json.dumps(doc, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    encoded += b" " * (-len(encoded) % 4)
    binary += b"\0" * (-len(binary) % 4)
    body = struct.pack("<II", len(encoded), 0x4e4f534a) + encoded + struct.pack("<II", len(binary), 0x004e4942) + binary
    path.write_bytes(struct.pack("<III", 0x46546c67, 2, 12 + len(body)) + body)
    temporary.unlink()
    buffer_path.unlink()


def sample(name="apartment_06", width=14.4, depth=10.8, floors=6, family="apartment", seed=17):
    return {"id": name, "ring": [[-width / 2, -depth / 2], [width / 2, -depth / 2],
                                  [width / 2, depth / 2], [-width / 2, depth / 2]],
            "h": floors * (3.3 if family == "apartment" else 3.6 if family == "commercial" else 5),
            "storey_m": 3.3 if family == "apartment" else 3.6 if family == "commercial" else 5,
            "family": family, "seed": seed, "detail": "near",
            "obb": {"c": [0, 0], "u": [1, 0], "w": width, "d": depth},
            "style": "apartment_concrete" if family == "apartment" else "office_ribbon" if family == "commercial" else "warehouse_clerestory"}


def render(objs, path, target=(0, 0, 10), eye=(35, -45, 28), ortho=None):
    scene = bpy.context.scene
    for obj in bpy.data.objects:
        obj.hide_render = obj.type == "MESH" and obj not in objs
    bpy.ops.object.camera_add(location=eye)
    camera = bpy.context.object
    camera.rotation_euler = (Vector(target) - camera.location).to_track_quat('-Z', 'Y').to_euler()
    camera.data.clip_end = 3000
    if ortho:
        camera.data.type = "ORTHO"
        camera.data.ortho_scale = ortho
    scene.camera = camera
    scene.world = bpy.data.worlds.new("ArchitectureWorld")
    scene.world.color = (0.15, 0.15, 0.15)
    bpy.ops.object.light_add(type="SUN", rotation=(0.4, -0.5, -0.2))
    bpy.context.object.data.energy = 2
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x, scene.render.resolution_y = 1400, 1000
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "AgX"
    path.parent.mkdir(parents=True, exist_ok=True)
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)


def probe():
    b = sample()
    objs = objects_from(building(b, base=0), b["id"])
    export(objs, OUT / "probe/apartment_06.glb")
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / "probe/apartment_06.blend"), compress=True)
    render(objs, ROOT / "captures/shuto/buildings/blender_probe.png")
    template = json.loads((ROOT / "scenes/shuto_roundtrip.json").read_text(encoding="utf-8"))
    template["scene_name"] = "shuto_building_probe"
    template["window_room_root"] = "textures/window_rooms"
    template["nodes"][0]["path"] = "models/shuto/buildings/probe/apartment_06.glb"
    poses = [("front", [18, 9, 26], [0, 8, 0]), ("grazing", [12, 6, 9], [0, 6, 4.9]),
             ("room_left", [-3, 6.3, 9], [0, 5.6, 5.25]), ("room_right", [3, 6.3, 9], [0, 5.6, 5.25])]
    template["camera"] = {"position": poses[0][1], "look_at": poses[0][2], "fov": 55, "near": 0.1, "far": 1000}
    template["benchmark"] = {"warmup_frames": 30, "fixed_hz": 60,
                             "shots": [{"name": name, "duration": 1, "capture_times": [0.8],
                                        "camera": {"keys": [{"t": 0, "position": eye, "look_at": target, "fov": 55}]}}
                                       for name, eye, target in poses]}
    save_json(ROOT / "scenes/shuto_building_probe.json", template)
    save_json(ROOT / "data/shuto/buildings/probe_expected.json", {"bounds": [[-7.2, 0, -5.4], [7.2, 22, 5.4]],
                                                               "floor_pitch_m": 3.3, "pane_size_m": [1.6, 2.0],
                                                               "triangles": sum(len(o.data.polygons) for o in objs)})


def kit():
    entries = []
    previews = []
    specs = [("residential_compact", 9.6, 8.0, 3, "apartment"), ("residential_balcony", 14.4, 10.8, 6, "apartment"),
             ("residential_slab", 28.8, 12.8, 8, "apartment"), ("apartment_tower", 25.6, 19.2, 20, "apartment"),
             ("office_small", 12.8, 12.8, 5, "commercial"), ("office_ribbon", 25.6, 19.2, 10, "commercial"),
             ("office_tower", 32, 25.6, 30, "commercial"), ("retail_frontage", 16, 8, 3, "commercial"),
             ("retail_corner", 12.8, 16, 5, "commercial"), ("commercial_midrise", 19.2, 16, 9, "commercial"),
             ("warehouse_small", 24, 18, 1, "industrial"), ("warehouse_large", 48, 24, 2, "industrial"),
             ("waterfront_facility", 36, 24, 3, "industrial"), ("industrial_workshop", 19.2, 12.8, 1, "industrial"),
             ("landmark_dentsu", 127.9, 57.25, 48, "commercial"), ("landmark_fuji_tv", 304, 104, 25, "commercial")]
    for i, (name, w, d, floors, family) in enumerate(specs):
        b = sample(name, w, d, floors, family, i + 11)
        b["front_edge"] = 2
        if name == "landmark_dentsu":
            real = next(x for x in json.loads((ROOT / "data/shuto/buildings/placements.json").read_text(encoding="utf-8"))["buildings"] if x.get("landmark") == "dentsu")
            cx, cz = real["obb"]["c"]
            ux, uz = real["obb"]["u"]
            b.update(landmark="dentsu", h=real["h"], storey_m=real["storey_m"],
                     ring=[[(p[0] - cx) * ux + (p[1] - cz) * uz,
                            (p[0] - cx) * uz - (p[1] - cz) * ux] for p in real["ring"]])
        elif name == "landmark_fuji_tv":
            b.update(landmark="fuji", h=123.45, storey_m=4.1)
            b["obb"]["u"] = [-1, 0]
        entry = {"id": name, "family": family, "dimensions_m": [w, b["h"], d], "floor_pitch_m": b["storey_m"],
                 "license": "project-authored geometry; project texture pack", "lods": []}
        for lod in range(3):
            coll = bpy.data.collections.new(name + "_LOD%d" % lod)
            bpy.context.scene.collection.children.link(coll)
            objs = objects_from(building(b, lod, base=0), name + "_LOD%d" % lod, coll)
            for o in objs:
                o["building_id"] = name
                o["lod"] = lod
                o["floor_pitch_m"] = b["storey_m"]
            if lod == 0:
                coll.asset_mark()
            path = OUT / "kit" / (name + "_LOD%d.glb" % lod)
            export(objs, path)
            entry["lods"].append({"level": lod, "path": path.relative_to(ROOT).as_posix(),
                                   "triangles": sum(len(o.data.polygons) for o in objs), "recommended_start_m": [0, 50, 120][lod]})
            if lod == 0 and name in {"residential_balcony", "retail_corner", "warehouse_large", "landmark_fuji_tv"}:
                previews.append((name, objs, b["h"]))
            for o in objs:
                o.hide_set(True)
                o.hide_render = True
        cb = g.Buckets()
        add_box(cb, (name, "collision"), (0, 0), (1, 0), w, d, 0, b["h"])
        co = objects_from(cb, name + "_COL")
        path = OUT / "kit" / (name + "_col.glb")
        export(co, path)
        for o in co:
            o.hide_set(True)
            o.hide_render = True
        entry["collision"] = path.relative_to(ROOT).as_posix()
        entries.append(entry)
    components = []
    for name in ["rooftop_ac", "water_tank", "shop_sign", "window_bay"]:
        bk = g.Buckets()
        if name == "rooftop_ac":
            add_box(bk, (name, "steel"), (0, 0), (1, 0), 1.4, 0.8, 0, 0.65)
            for j in range(5):
                panel(bk, (name, "frame"), (-0.65, 0.41), (0.65, 0.41), 0.08 + j * 0.1, 0.105 + j * 0.1, (0, 0, 1))
        elif name == "water_tank":
            cylinder(bk, (name, "steel"), (0, 0, 0), 0.65, 1.6)
        elif name == "shop_sign":
            panel(bk, (name, "sign_minato_coffee"), (-1.9, 0), (1.9, 0), 0, 0.95, (0, 0, 1),
                  uv=[(0, 0), (1, 0), (1, 1), (0, 1)])
        else:
            detailed_walls(bk, name, [[-1.6, -1], [1.6, -1], [1.6, 1], [-1.6, 1]], 3.3, 3.3, "apartment", 12, 0, False)
        objs = objects_from(bk, "component_" + name)
        path = OUT / "kit" / ("component_" + name + ".glb")
        export(objs, path)
        component = bpy.data.collections.new("component_" + name)
        bpy.context.scene.collection.children.link(component)
        component.asset_mark()
        component["nox_forward"] = "+Z"
        component.instance_offset = (-70 + len(components) * 15, 70, 0)
        for o in objs:
            for old in list(o.users_collection):
                old.objects.unlink(o)
            component.objects.link(o)
            o.location = component.instance_offset
        components.append({"id": name, "path": path.relative_to(ROOT).as_posix()})
    # Editable source geometry keeps a base pivot. Show LOD0 in a spaced gallery on opening.
    for i, entry in enumerate(entries):
        offset = Vector(((i % 4) * 310, -(i // 4) * 310, 0))
        bpy.data.collections[entry["id"] + "_LOD0"].instance_offset = offset
        for obj in bpy.data.collections[entry["id"] + "_LOD0"].objects:
            obj.hide_set(False)
            obj.hide_render = False
            obj.location = offset
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / "japanese_architecture_kit.blend"), compress=True)
    save_json(OUT / "kit_index.json", {"format": "nox-japanese-architecture-kit", "units": "metres", "axes": "glTF Y-up",
                                      "pivot": "base center; front local +Z NOX", "buildings": entries, "components": components,
                                      "lod_activation": "Separate files; NOX does not automatically switch these LODs"})
    for name, objs, height in previews:
        for o in objs:
            o.hide_set(False)
            o.location = (0, 0, 0)
        width = max(v.co.x for o in objs for v in o.data.vertices) - min(v.co.x for o in objs for v in o.data.vertices)
        eye = (35, -45, 28) if height < 40 else (height * 1.7, -height * 2.2, height * 1.4)
        render(objs, ROOT / "captures/shuto/buildings" / ("kit_" + name + ".png"),
               target=(0, 0, height / 2), eye=eye, ortho=max(height * 1.5, width * 1.5, 65))
        if name == "landmark_fuji_tv":
            render(objs, ROOT / "captures/shuto/buildings/fuji_front.png", target=(0, 0, 62),
                   eye=(0, -420, 72), ortho=340)


def source_templates():
    index = json.loads((OUT / "source_kit_index.json").read_text(encoding="utf-8"))
    templates = []
    for entry in index["buildings"]:
        if entry["id"] not in {"source_asian_16", "source_asian_17", "source_asian_18", "source_asian_19",
                                "source_lowpoly_10", "source_lowpoly_12", "source_lowpoly_15", "source_lowpoly_18"}:
            continue
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=str(ROOT / entry["lods"][0]["path"]))
        objs = [o for o in bpy.data.objects if o not in before and o.type == "MESH"]
        for obj in objs:
            obj.hide_set(True)
            for mat in obj.data.materials:
                if mat:
                    MATS[mat.name] = mat
        templates.append((entry, objs))
    return templates


def fitted_source(b, templates, placed):
    if b["detail"] != "far" or b.get("landmark") or b.get("name_en") or placed >= 32:
        return None
    o = b["obb"]
    for entry, objs in templates:
        w, h, d = entry["dimensions_m"]
        if not (0.70 <= w / o["w"] <= 0.94 and 0.70 <= d / o["d"] <= 0.94 and abs(h - b["h"]) <= b["h"] * 0.16):
            continue
        ux, uz = o["u"]
        vx, vz = uz, -ux
        cx, cz = o["c"]
        corners = [(cx + ux * x + vx * z, cz + uz * x + vz * z) for x in [-w / 2, w / 2] for z in [-d / 2, d / 2]]
        if not all(g.point_in_ring2(x, z, b["ring"]) for x, z in corners):
            continue
        matrix = Matrix.Identity(4)
        matrix.col[0] = Vector((ux, -uz, 0, 0))
        matrix.col[1] = Vector((vx, -vz, 0, 0))
        matrix.col[2] = Vector((0, 0, 1, 0))
        matrix.col[3] = Vector((cx, -cz, BASE, 1))
        bk = g.Buckets()
        instances = []
        for template in objs:
            instance = template.copy()
            instance.name = b["id"] + "_" + entry["id"]
            bpy.context.collection.objects.link(instance)
            instance.matrix_world = matrix
            instance.hide_set(False)
            instances.append(instance)
            mesh = instance.data
            mesh.calc_loop_triangles()
            for triangle in mesh.loop_triangles:
                mat = mesh.materials[triangle.material_index]
                key = (b["id"], mat.name)
                for loop in triangle.loops:
                    p = matrix @ mesh.vertices[mesh.loops[loop].vertex_index].co
                    bk.pos[key].extend(p)
                    bk.uv[key].extend(mesh.uv_layers.active.data[loop].uv)
                bk.tris[key] += 1
        return bk, instances, entry
    return None


def section():
    plan = json.loads((ROOT / "data/shuto/buildings/placements.json").read_text(encoding="utf-8"))
    chunk_buckets = defaultdict(g.Buckets)
    col_buckets = defaultdict(g.Buckets)
    records = []
    templates = source_templates()
    placed_sources = 0
    for i, b in enumerate(plan["buildings"]):
        coll = bpy.data.collections.new(b["id"])
        bpy.context.scene.collection.children.link(coll)
        fitted = fitted_source(b, templates, placed_sources)
        if fitted:
            bk, objs, source = fitted
            placed_sources += 1
            for obj in objs:
                for old in list(obj.users_collection):
                    old.objects.unlink(obj)
                coll.objects.link(obj)
        else:
            bk = building(b)
            objs = objects_from(bk, b["id"], coll)
            anchor = b.get("model_anchor", b["obb"]["c"])
            pivot = Vector((anchor[0], -anchor[1], BASE))
            for obj in objs:
                obj.data.transform(Matrix.Translation(-pivot))
                obj.location = pivot
            source = None
        for o in objs:
            o["building_id"] = b["id"]
            o["chunk"] = "%d_%d" % (b["cx"], b["cz"])
            o["source_osm"] = b["osm"]
            o["detail"] = b["detail"]
        chunk = (b["cx"], b["cz"])
        dest = chunk_buckets[chunk]
        for key in bk.pos:
            dkey = (chunk[0], chunk[1], key[-1])
            dest.pos[dkey].extend(bk.pos[key])
            dest.uv[dkey].extend(bk.uv[key])
            dest.tris[dkey] += bk.tris[key]
        cb = col_buckets[chunk]
        # Closed polygonal prism collision proxy follows the actual footprint, not an oversized OBB.
        collision_ring = b.get("collision_ring", b["ring"])
        for a, z, _ in g.wall_edges(collision_ring):
            panel(cb, (chunk[0], chunk[1], "collision"), a, z, BASE, BASE + b["h"], normal(a, z, collision_ring))
        g.emit_roof(cb, (chunk[0], chunk[1], "collision"), collision_ring, BASE + b["h"], 1)
        bottom(cb, (chunk[0], chunk[1], "collision"), collision_ring, BASE)
        records.append({"id": b["id"], "chunk": list(chunk), "detail": b["detail"], "osm": b["osm"],
                        "triangles": sum(bk.tris.values()), "height_m": b["h"], "floor_pitch_m": b["storey_m"],
                        "era_status": b["era_status"]})
        if source:
            records[-1].update(source_asset=source["id"], source_license=source["license"], uniform_scale=1.0)
        if i % 500 == 0:
            print("buildings", i, "triangles", sum(r["triangles"] for r in records), flush=True)
    for pad in plan.get("site_pads", []):
        # Paved site blocks under the Daiba waterfront context, beyond the exported ground section.
        coll = bpy.data.collections.new(pad["id"])
        bpy.context.scene.collection.children.link(coll)
        bk = g.Buckets()
        site_pad(bk, (pad["id"], "roof"), pad)
        for o in objects_from(bk, pad["id"], coll):
            o["building_id"] = pad["building"]
            o["chunk"] = "%d_%d" % (pad["cx"], pad["cz"])
            o["site_pad"] = True
        dest = chunk_buckets[(pad["cx"], pad["cz"])]
        for key in bk.pos:
            dkey = (pad["cx"], pad["cz"], key[-1])
            dest.pos[dkey].extend(bk.pos[key])
            dest.uv[dkey].extend(bk.uv[key])
            dest.tris[dkey] += bk.tris[key]
        records.append({"id": pad["id"], "chunk": [pad["cx"], pad["cz"]], "detail": "site_pad", "osm": 0,
                        "triangles": sum(bk.tris.values()), "height_m": pad["top_y"] - pad["bottom_y"],
                        "floor_pitch_m": 0, "era_status": "site_paving_for_landmark"})
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / "placed_buildings.blend"), compress=True)
    chunks = []
    for chunk in sorted(chunk_buckets):
        name = "%d_%d" % chunk
        objs = objects_from(chunk_buckets[chunk], "chunk_" + name)
        export(objs, OUT / "render" / (name + ".glb"))
        cb = objects_from(col_buckets[chunk], "col_" + name)
        export(cb, OUT / "collision" / (name + "_col.glb"))
        chunks.append({"id": name, "render": "models/shuto/buildings/render/" + name + ".glb",
                       "collision": "models/shuto/buildings/collision/" + name + "_col.glb",
                       "triangles": sum(chunk_buckets[chunk].tris.values()),
                       "collision_triangles": sum(col_buckets[chunk].tris.values())})
    save_json(OUT / "section_index.json", {"chunks": chunks, "placements": records,
                                          "source_instances": placed_sources,
                                          "render_triangles": sum(c["triangles"] for c in chunks),
                                          "collision_triangles": sum(c["collision_triangles"] for c in chunks)})


def fuji_update():
    plan = json.loads((ROOT / "data/shuto/buildings/placements.json").read_text(encoding="utf-8"))
    b = next(b for b in plan["buildings"] if b.get("landmark") == "fuji")
    assert sum((p["cx"], p["cz"]) == (b["cx"], b["cz"]) for p in plan["buildings"]) == 1
    path = OUT / "placed_buildings.blend"
    bpy.ops.wm.open_mainfile(filepath=str(path))
    for obj in list(bpy.data.objects):
        if obj.get("building_id") == b["id"]:
            bpy.data.objects.remove(obj, do_unlink=True)
    MATS.clear()
    materials()
    bk = building(b, lod=1)
    collection = bpy.data.collections.get(b["id"])
    objs = objects_from(bk, b["id"], collection)
    for obj in objs:
        obj["building_id"] = b["id"]
        obj["chunk"] = "%d_%d" % (b["cx"], b["cz"])
        obj["source_osm"] = b["osm"]
        obj["detail"] = "mid"
    chunk = "%d_%d" % (b["cx"], b["cz"])
    export(objs, OUT / "render" / (chunk + ".glb"))
    cb = g.Buckets()
    ring = b["collision_ring"]
    for p, q, _ in g.wall_edges(ring):
        panel(cb, (b["id"], "collision"), p, q, BASE, BASE + b["h"], normal(p, q, ring))
    g.emit_roof(cb, (b["id"], "collision"), ring, BASE + b["h"], 1)
    bottom(cb, (b["id"], "collision"), ring, BASE)
    cols = objects_from(cb, b["id"] + "_COL")
    export(cols, OUT / "collision" / (chunk + "_col.glb"))
    for obj in cols:
        bpy.data.objects.remove(obj, do_unlink=True)
    anchor = b.get("model_anchor", b["obb"]["c"])
    pivot = Vector((anchor[0], -anchor[1], BASE))
    for obj in objs:
        obj.data.transform(Matrix.Translation(-pivot))
        obj.location = pivot
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(path), compress=True)
    index = json.loads((OUT / "section_index.json").read_text(encoding="utf-8"))
    for c in index["chunks"]:
        if c["id"] == chunk:
            c["triangles"] = sum(bk.tris.values())
            c["collision_triangles"] = sum(cb.tris.values())
    for p in index["placements"]:
        if p["id"] == b["id"]:
            p["triangles"] = sum(bk.tris.values())
            p["model_revision"] = "reference-led Fuji v2"
    index["render_triangles"] = sum(c["triangles"] for c in index["chunks"])
    index["collision_triangles"] = sum(c["collision_triangles"] for c in index["chunks"])
    save_json(OUT / "section_index.json", index)


def context():
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / "models/shuto/src/rep_section_scenery.blend"))
    by_chunk = defaultdict(list)
    allowed = {"water", "foliage", "container", "street_asphalt", "sidewalk", "paint_white", "paint_yellow", "street_metal", "lamp_head"}
    for obj in bpy.data.objects:
        if obj.type == "MESH" and obj.name.startswith("r_") and obj.data.materials:
            if obj.data.materials[0].name in allowed:
                parts = obj.name.split("_")
                by_chunk[(int(parts[1]), int(parts[2]))].append(obj)
    MATS["roof_membrane"] = bpy.data.materials["roof_membrane"]
    material("collision", (0.5, 0.1, 0.1))
    roofs, collisions = defaultdict(g.Buckets), defaultdict(g.Buckets)
    plan = json.loads((ROOT / "models/shuto/rep_section/scenery_plan.json").read_text(encoding="utf-8"))
    for c in plan["containers"]:
        chunk = (c["cx"], c["cz"])
        ux, uz = c["u"]
        center = (c["pos"][0] + ux * g.CONTAINER_L / 2, c["pos"][1] + uz * g.CONTAINER_L / 2)
        ring = [(center[0] + ux * x + uz * z, center[1] + uz * x - ux * z)
                for x, z in [(-g.CONTAINER_L / 2, -g.CONTAINER_W / 2), (g.CONTAINER_L / 2, -g.CONTAINER_W / 2),
                             (g.CONTAINER_L / 2, g.CONTAINER_W / 2), (-g.CONTAINER_L / 2, g.CONTAINER_W / 2)]]
        g.emit_roof(roofs[chunk], (*chunk, "roof_membrane"), ring, g.BASE_Y + c["stack"] * g.CONTAINER_H, 3)
        add_box(collisions[chunk], (*chunk, "collision"), center, (ux, uz), g.CONTAINER_L, g.CONTAINER_W,
                g.BASE_Y, c["stack"] * g.CONTAINER_H)
    for chunk, objs in sorted(by_chunk.items()):
        if chunk in roofs:
            objs += objects_from(roofs[chunk], "container_roofs")
        path = OUT / "context" / ("%d_%d.glb" % chunk)
        export(objs, path)
        # Preserve the existing tree-card alpha convention in the repackaged context.
        raw = path.read_bytes()
        size = struct.unpack_from("<I", raw, 12)[0]
        doc = json.loads(raw[20:20 + size])
        for mat in doc.get("materials", []):
            if mat.get("name") == "foliage":
                mat.update(alphaMode="MASK", alphaCutoff=0.5, doubleSided=True)
        encoded = json.dumps(doc, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
        encoded += b" " * ((-len(encoded)) % 4)
        tail = raw[20 + size:]
        path.write_bytes(struct.pack("<III", 0x46546C67, 2, 20 + len(encoded) + len(tail)) +
                         struct.pack("<II", len(encoded), 0x4E4F534A) + encoded + tail)
    for chunk, bk in collisions.items():
        export(objects_from(bk, "container_collision"), OUT / "context_collision" / ("%d_%d_col.glb" % chunk))
    print("context chunks", len(by_chunk), flush=True)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["probe", "kit", "section", "context", "fuji_update"], required=True)
    args = parser.parse_args(argv)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.unit_settings.system = "METRIC"
    bpy.context.scene.unit_settings.scale_length = 1
    OUT.mkdir(parents=True, exist_ok=True)
    if args.mode not in {"context", "fuji_update"}:
        materials()
    globals()[args.mode]()


if __name__ == "__main__":
    main()
