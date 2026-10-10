"""Print a structural summary of GLB sources for kit planning (Blender headless).

Usage: blender -b --factory-startup --python-exit-code 1 --python inspect_sources.py -- <glb> [<glb> ...]
"""
import sys
import bpy
import mathutils


def argv_after_dashes():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


for path in argv_after_dashes():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=path)
    print("=" * 70)
    print("FILE", path)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    print("objects", len(bpy.data.objects), "meshes", len(meshes), "materials", len(bpy.data.materials),
          "images", len(bpy.data.images))
    for img in bpy.data.images:
        print("  image", img.name, tuple(img.size), img.colorspace_settings.name, img.filepath[:40])
    for o in bpy.data.objects:
        s = o.matrix_world.to_scale()
        dims = o.dimensions
        parent = o.parent.name if o.parent else "-"
        tris = sum(len(p.vertices) - 2 for p in o.data.polygons) if o.type == "MESH" else 0
        mats = [m.material.name if m.material else None for m in o.material_slots] if o.type == "MESH" else []
        neg = o.matrix_world.determinant() < 0
        print(f"  {o.type:6s} {o.name[:40]:40s} parent={parent[:20]:20s} loc={tuple(round(v,2) for v in o.location)} "
              f"scale={tuple(round(v,3) for v in o.scale)} dim={tuple(round(v,2) for v in dims)} tris={tris} "
              f"mirror={neg} mats={mats[:3]}")
    for m in bpy.data.materials:
        print("  mat", m.name, "nodes", [n.type for n in m.node_tree.nodes][:12] if m.node_tree else None)
    # world bounds of all meshes
    lo = mathutils.Vector((1e9, 1e9, 1e9))
    hi = mathutils.Vector((-1e9, -1e9, -1e9))
    for o in meshes:
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            lo = mathutils.Vector((min(lo[i], w[i]) for i in range(3)))
            hi = mathutils.Vector((max(hi[i], w[i]) for i in range(3)))
    print("WORLD BOUNDS lo", tuple(round(v, 2) for v in lo), "hi", tuple(round(v, 2) for v in hi))
    print("size", tuple(round(hi[i] - lo[i], 2) for i in range(3)))
