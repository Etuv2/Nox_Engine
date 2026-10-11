"""Normalize the supplied architecture without destroying source UVs or textures.

Blender headless. Run after the new-building probe passes. All transforms are
captured before detaching any parent. The Standard-licensed file stays separate.
"""
import json
import math
from pathlib import Path
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_architecture as arch

ROOT = arch.ROOT
OUT = arch.OUT
SOURCES = [("asian", "asian_themed_low_poly_night_city_buildings.glb", 1.0, "CC-BY-4.0", "99.Miles"),
           ("japanese", "japanese_building__cc0.glb", 1.0, "Sketchfab Standard", "Lit For 20"),
           ("lowpoly", "low-poly_city_buildings.glb", 160.0, "CC-BY-4.0", "smooth998")]


def main():
    entries = []
    for key, filename, scale, license_name, author in SOURCES:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.context.scene.unit_settings.system = "METRIC"
        source = ROOT / "models/shuto/src/originals" / filename
        bpy.ops.import_scene.gltf(filepath=str(source))
        meshes = [o for o in bpy.data.objects if o.type == "MESH"]
        transforms = {o: o.matrix_world.copy() for o in meshes}
        kept = []
        for obj in meshes:
            names = [m.name.upper() for m in obj.data.materials if m]
            if "FLARE" in obj.name.upper() or any("FLARE" in m for m in names) or "GROUND" in names:
                bpy.data.objects.remove(obj, do_unlink=True)
                continue
            if key == "japanese" and obj.name == "Object_4":
                bpy.data.objects.remove(obj, do_unlink=True)
                continue
            if obj.data.users > 1:
                obj.data = obj.data.copy()
            matrix = transforms[obj]
            obj.parent = None
            obj.matrix_world = Matrix.Identity(4)
            obj.data.transform(Matrix.Scale(scale, 4) @ matrix)
            bm = bmesh.new()
            bm.from_mesh(obj.data)
            if matrix.determinant() < 0:
                bmesh.ops.reverse_faces(bm, faces=list(bm.faces))
            bmesh.ops.triangulate(bm, faces=list(bm.faces))
            tiny = [f for f in bm.faces if (f.verts[1].co - f.verts[0].co).cross(f.verts[2].co - f.verts[0].co).length_squared < 1e-12]
            bmesh.ops.delete(bm, geom=tiny, context="FACES")
            bm.normal_update()
            bm.to_mesh(obj.data)
            bm.free()
            kept.append(obj)
        for obj in list(bpy.data.objects):
            if obj.type != "MESH":
                bpy.data.objects.remove(obj, do_unlink=True)
        if key == "japanese":
            bpy.ops.object.select_all(action="DESELECT")
            for obj in kept:
                obj.select_set(True)
            bpy.context.view_layer.objects.active = kept[0]
            bpy.ops.object.join()
            kept = [bpy.context.object]
        for img in bpy.data.images:
            img.name = "source_%s_%s" % (key, img.name)
            if max(img.size) > 2048:
                ratio = 2048 / max(img.size)
                img.scale(round(img.size[0] * ratio), round(img.size[1] * ratio))
            img.filepath_raw = str(arch.TEX / (img.name.replace(".", "_") + ".png"))
            img.file_format = "PNG"
            img.save()
            img.pack()
        for mat in bpy.data.materials:
            mat.name = "source_%s_%s" % (key, mat.name)
        arch.material("collision", (0.4, 0.1, 0.1))
        arch.material("source_mass", (0.16, 0.18, 0.19), rough=0.75)
        kept.sort(key=lambda o: (-max(v.co.z for v in o.data.vertices), o.name))
        for i, obj in enumerate(kept):
            name = "source_%s_%02d" % (key, i + 1)
            original_name = obj.name
            obj.name = name
            points = [v.co for v in obj.data.vertices]
            lo = Vector(tuple(min(p[j] for p in points) for j in range(3)))
            hi = Vector(tuple(max(p[j] for p in points) for j in range(3)))
            obj.data.transform(Matrix.Translation(Vector((-(lo.x + hi.x) / 2, -(lo.y + hi.y) / 2, -lo.z))))
            # Recheck after float32 coordinate recentering; nearly collinear source faces can collapse here.
            bm = bmesh.new()
            bm.from_mesh(obj.data)
            bmesh.ops.delete(bm, geom=[f for f in bm.faces if (f.verts[1].co - f.verts[0].co).cross(f.verts[2].co - f.verts[0].co).length_squared < 1e-12], context="FACES")
            bm.to_mesh(obj.data)
            bm.free()
            dims = hi - lo
            obj["source_file"] = filename
            obj["original_object"] = original_name
            obj["license"] = license_name
            obj["scale_applied"] = scale
            obj.asset_mark()
            folder = "restricted_local" if key == "japanese" else "sources"
            path = OUT / folder / (name + "_LOD0.glb")
            arch.export([obj], path)
            entry = {"id": name, "source_file": filename, "source_object": original_name,
                     "license": license_name, "author": author, "standalone_redistribution": key != "japanese",
                     "dimensions_m": [round(dims.x, 4), round(dims.z, 4), round(dims.y, 4)],
                     "scale_applied": scale, "scale_basis": "uniform visual floor-pitch calibration; no surveyed source units",
                     "lods": [{"level": 0, "path": path.relative_to(ROOT).as_posix(),
                               "triangles": sum(len(p.vertices) - 2 for p in obj.data.polygons)}],
                     "placement_status": "generic reference architecture; not a real Japanese landmark"}
            clone = obj.copy()
            clone.data = obj.data.copy()
            clone.name = name + "_LOD1"
            bpy.context.collection.objects.link(clone)
            mod = clone.modifiers.new("Planar simplification preserving UV/material boundaries", "DECIMATE")
            mod.decimate_type = "DISSOLVE"
            mod.angle_limit = math.radians(2)
            mod.delimit = {"UV", "MATERIAL", "NORMAL"}
            bpy.context.view_layer.objects.active = clone
            bpy.ops.object.modifier_apply(modifier=mod.name)
            bm = bmesh.new()
            bm.from_mesh(clone.data)
            bmesh.ops.triangulate(bm, faces=list(bm.faces))
            bmesh.ops.delete(bm, geom=[f for f in bm.faces if (f.verts[1].co - f.verts[0].co).cross(f.verts[2].co - f.verts[0].co).length_squared < 1e-12], context="FACES")
            bm.to_mesh(clone.data)
            bm.free()
            lpath = path.with_name(name + "_LOD1.glb")
            arch.export([clone], lpath)
            entry["lods"].append({"level": 1, "path": lpath.relative_to(ROOT).as_posix(),
                                   "triangles": sum(len(p.vertices) - 2 for p in clone.data.polygons)})
            far = arch.g.Buckets()
            arch.add_box(far, (name, "source_mass"), (0, 0), (1, 0), dims.x, dims.y, 0, dims.z)
            far_objs = arch.objects_from(far, name + "_LOD2")
            fpath = path.with_name(name + "_LOD2.glb")
            arch.export(far_objs, fpath)
            entry["lods"].append({"level": 2, "path": fpath.relative_to(ROOT).as_posix(), "triangles": 12})
            collision = arch.g.Buckets()
            arch.add_box(collision, (name, "collision"), (0, 0), (1, 0), dims.x, dims.y, 0, dims.z)
            collision_objs = arch.objects_from(collision, name + "_COL")
            cpath = path.with_name(name + "_col.glb")
            arch.export(collision_objs, cpath)
            entry["collision"] = cpath.relative_to(ROOT).as_posix()
            for o in far_objs + collision_objs:
                o.hide_set(True)
            for o in [obj, clone]:
                o.hide_set(True)
            entries.append(entry)
        for i, obj in enumerate(kept):
            obj.hide_set(False)
            obj.location = ((i % 5) * 140, -(i // 5) * 140, 0)
        bpy.ops.file.pack_all()
        bpy.ops.wm.save_as_mainfile(filepath=str(OUT / ("source_%s.blend" % key)), compress=True)
        print("source", key, "buildings", len(kept), flush=True)
    arch.save_json(OUT / "source_kit_index.json", {"buildings": entries,
                                                 "warning": "Japanese Standard source is local only; do not redistribute the standalone derivative"})


if __name__ == "__main__":
    main()
