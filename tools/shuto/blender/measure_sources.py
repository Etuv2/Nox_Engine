"""Measure world-space building sizes and implied storey height per mesh (Blender headless).

Storeys are estimated from horizontal floor-slab bands: vertex Z values rounded to
0.25 m that are shared by at least 8 vertices. This is a heuristic for scale choice.

Usage: blender -b --factory-startup --python-exit-code 1 --python measure_sources.py -- <glb>
"""
import sys
from collections import Counter
import bpy
from mathutils import Vector


def argv_after_dashes():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def world_verts(o):
    m = o.matrix_world
    return [m @ v.co for v in o.data.vertices]


for path in argv_after_dashes():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=path)
    print("FILE", path.split("/")[-1])
    rows = []
    for o in bpy.data.objects:
        if o.type != "MESH" or len(o.data.vertices) == 0:
            continue
        vs = world_verts(o)
        zs = [round(v.z / 0.25) * 0.25 for v in vs]
        cnt = Counter(zs)
        bands = [z for z, c in cnt.items() if c >= 8]
        xs = [v.x for v in vs]; ys = [v.y for v in vs]; zz = [v.z for v in vs]
        w = max(xs) - min(xs); d = max(ys) - min(ys); h = max(zz) - min(zz)
        floors = max(0, len(bands) - 1)
        st = (h / floors) if floors >= 2 else float("nan")
        rows.append((o.name[:34], round(w, 1), round(d, 1), round(h, 1), len(bands), round(st, 2) if st == st else None,
                     sum(len(p.vertices) - 2 for p in o.data.polygons), [m.name for m in o.data.materials][:2]))
    for r in sorted(rows, key=lambda r: -r[3]):
        print("  %-34s w=%6.1f d=%6.1f h=%6.1f bands=%3d storey=%s tris=%d mats=%s" % (r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]))
    hs = sorted(r[3] for r in rows)
    if hs:
        print("  height median %.1f  min %.1f  max %.1f  n=%d" % (hs[len(hs) // 2], hs[0], hs[-1], len(hs)))
