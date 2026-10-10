"""Build the Shuto building kit from the three source GLBs (Blender headless).

Run from the repository root (paths are absolute arguments):
  blender -b --factory-startup --python-exit-code 1 --python tools/shuto/blender/build_kit.py -- <repo_root>

Inputs : models/shuto/src/originals/*.glb (copied, never modified)
Outputs: models/shuto/src/buildings_kit.blend
         models/shuto/kit/buildings_<source>.glb   (one GLB per source, one root node per building)
         models/shuto/kit/kit_index.json

Per-source decisions (see docs/shuto/assets_and_licenses.md):
  asian_themed   scale 0.3   authored skyline towers up to 300 m; 0.3 gives max ~90 m, median ~30 m
  japanese       scale 1.0   main body 21 m tall, roughly 6-7 floors at 3.0-3.5 m
  low-poly city  scale 9.4   median inferred storey 0.35 m in source units, target 3.3 m
Geometry is baked to world space, mirrored parts are re-wound, flare/light sprites are
removed to metadata, each building gets a base-centre pivot at y=0 (Blender Z-up, glTF Y-up
on export), and 4096 textures are downscaled to 2048.
"""
import bmesh
import bpy
import json
import os
import sys
from mathutils import Matrix, Vector

ORIGINALS = "models/shuto/src/originals"
SOURCES = [
    dict(key="asian_themed", file="asian_themed_low_poly_night_city_buildings.glb", scale=0.3,
         license="CC-BY-4.0", author="99.Miles", drop_ground=False),
    dict(key="japanese", file="japanese_building__cc0.glb", scale=1.0,
         license="Sketchfab Standard (unverified; file title says CC0)", author="unknown (see Sketchfab page)",
         drop_ground=False),
    dict(key="lowpoly", file="low-poly_city_buildings.glb", scale=9.4,
         license="CC-BY-4.0", author="smooth998", drop_ground=True),
]
MAX_TEX = 2048
STOREY_M = 3.3
VARIANT_FRACTIONS = [0.6]
VARIANT_MIN_HEIGHT = 12.0


def is_flare(o):
    name = o.name.upper()
    mats = [s.material.name.upper() for s in o.material_slots if s.material]
    return "FLARE" in name or any("FLARE" in m for m in mats)


def bake_to_world(o):
    if o.data.users > 1:
        o.data = o.data.copy()
    M = o.matrix_world.copy()
    if M.determinant() < 0:
        bm = bmesh.new()
        bm.from_mesh(o.data)
        bmesh.ops.reverse_faces(bm, faces=bm.faces[:])
        bm.to_mesh(o.data)
        bm.free()
    o.parent = None
    o.data.transform(M)
    o.matrix_world = Matrix.Identity(4)
    o.data.update()


def normalise_pivot(o, scale):
    o.data.transform(Matrix.Scale(scale, 4))
    vs = [v.co for v in o.data.vertices]
    minx, maxx = min(v.x for v in vs), max(v.x for v in vs)
    miny, maxy = min(v.y for v in vs), max(v.y for v in vs)
    minz = min(v.z for v in vs)
    o.data.transform(Matrix.Translation((-(minx + maxx) / 2, -(miny + maxy) / 2, -minz)))
    o.data.update()


def bounds(o):
    vs = [v.co for v in o.data.vertices]
    return (max(v.x for v in vs) - min(v.x for v in vs),
            max(v.y for v in vs) - min(v.y for v in vs),
            max(v.z for v in vs) - min(v.z for v in vs))


def tri_count(o):
    return sum(len(p.vertices) - 2 for p in o.data.polygons)


def has_emissive(o):
    for slot in o.material_slots:
        m = slot.material
        if not m or not m.use_nodes:
            continue
        for n in m.node_tree.nodes:
            if n.type == "BSDF_PRINCIPLED":
                if n.inputs["Emission Color"].is_linked:
                    return True
                s = n.inputs["Emission Strength"].default_value
                if s > 0.0 and n.inputs["Emission Color"].default_value[0] + n.inputs["Emission Color"].default_value[1] > 0.0:
                    return True
    return False


def truncate_variant(src, frac, name):
    me = src.data.copy()
    obj = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(obj)
    bm = bmesh.new()
    bm.from_mesh(me)
    top = max(v.co.z for v in bm.verts)
    zc = top * frac
    # Split faces at the cut plane first, so side faces are clipped rather than lost.
    bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                           plane_co=(0.0, 0.0, zc), plane_no=(0.0, 0.0, 1.0), dist=1e-5)
    above_faces = [f for f in bm.faces if min(v.co.z for v in f.verts) > zc + 1e-4]
    bmesh.ops.delete(bm, geom=above_faces, context="FACES")
    above_verts = [v for v in bm.verts if v.co.z > zc + 1e-4 and not v.link_faces]
    bmesh.ops.delete(bm, geom=above_verts, context="VERTS")
    boundary = [e for e in bm.edges if len(e.link_faces) == 1]
    if boundary:
        bmesh.ops.holes_fill(bm, edges=boundary, sides=0)
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    me.update()
    return obj


def main():
    repo = sys.argv[sys.argv.index("--") + 1]
    os.chdir(repo)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    kit_dir = os.path.join("models", "shuto", "kit")
    os.makedirs(kit_dir, exist_ok=True)

    index = {"units": "metres", "axes": "glTF Y-up export of Blender Z-up; pivot at base centre",
             "buildings": [], "light_sprites": [], "dropped": []}
    per_source_objects = {}

    for src in SOURCES:
        path = os.path.join(ORIGINALS, src["file"])
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=path)
        new_objs = [o for o in bpy.data.objects if o not in before]
        meshes = [o for o in new_objs if o.type == "MESH"]
        for o in meshes:
            bake_to_world(o)
        for name in [o.name for o in new_objs if o.type != "MESH"]:
            bpy.data.objects.remove(bpy.data.objects[name], do_unlink=True)

        flares = []
        keep = []
        for o in meshes:
            if is_flare(o):
                vs = [v.co for v in o.data.vertices]
                c = sum(vs, Vector()) / max(1, len(vs))
                flares.append({"source_object": o.name, "centre_source_m": [round(x * src["scale"], 3) for x in c],
                               "size_source_m": [round(x * src["scale"], 3) for x in bounds(o)]})
                bpy.data.objects.remove(o, do_unlink=True)
            elif src["drop_ground"] and any(s.material and s.material.name == "ground" for s in o.material_slots):
                index["dropped"].append({"source": src["key"], "object": o.name, "reason": "ground plane"})
                bpy.data.objects.remove(o, do_unlink=True)
            else:
                keep.append(o)
        for f in flares:
            index["light_sprites"].append(dict(f, source=src["key"]))

        if src["key"] == "japanese":
            bpy.ops.object.select_all(action="DESELECT")
            for o in keep:
                o.select_set(True)
            bpy.context.view_layer.objects.active = keep[0]
            bpy.ops.object.join()
            keep = [bpy.context.view_layer.objects.active]

        for o in keep:
            normalise_pivot(o, src["scale"])

        keep.sort(key=lambda o: -bounds(o)[2])
        built = []
        for i, o in enumerate(keep, 1):
            name = "bld_%s_%02d" % (src["key"], i)
            o.name = name
            o.data.name = name
            built.append(o)
        for o in list(built):
            w, d, h = bounds(o)
            if h >= VARIANT_MIN_HEIGHT:
                for frac in VARIANT_FRACTIONS:
                    tag = "_h%02d" % int(frac * 100)
                    v = truncate_variant(o, frac, o.name + tag)
                    built.append(v)
        for o in built:
            if o.name.startswith("bld_") and o.data.users == 1 and o.data.name != o.name:
                o.data.name = o.name
        per_source_objects[src["key"]] = built
        for o in built:
            w, d, h = bounds(o)
            index["buildings"].append({
                "name": o.name, "source": src["key"], "license": src["license"], "author": src["author"],
                "footprint_m": [round(w, 2), round(d, 2)], "height_m": round(h, 2),
                "storeys_est": max(1, round(h / STOREY_M)), "has_emissive": has_emissive(o),
                "tris": tri_count(o), "scale_applied": src["scale"],
                "variant_of": o.name.rsplit("_h", 1)[0] if "_h" in o.name[-4:] else None,
            })

    for img in bpy.data.images:
        if img.size[0] > MAX_TEX or img.size[1] > MAX_TEX:
            img.scale(img.size[0] // 2, img.size[1] // 2)

    blend_path = os.path.join("models", "shuto", "src", "buildings_kit.blend")
    bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(blend_path))

    for key, objs in per_source_objects.items():
        bpy.ops.object.select_all(action="DESELECT")
        for o in objs:
            o.select_set(True)
        bpy.context.view_layer.objects.active = objs[0]
        out = os.path.join(kit_dir, "buildings_%s.glb" % key)
        bpy.ops.export_scene.gltf(filepath=os.path.abspath(out), export_format="GLB", use_selection=True,
                                  export_yup=True, export_apply=False, export_image_format="AUTO",
                                  export_lights=False, export_cameras=False)
        print("exported", out, len(objs))

    with open(os.path.join(kit_dir, "kit_index.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(index, f, indent=2, ensure_ascii=True)
        f.write("\n")
    print("buildings", len(index["buildings"]), "light sprites", len(index["light_sprites"]))


if __name__ == "__main__":
    main()
