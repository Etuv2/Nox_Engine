"""Weld generated mesh topology for editing and frame the delivered Blender libraries."""
import json
import math
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "models/shuto/buildings"
index = json.loads((OUT / "section_index.json").read_text(encoding="utf-8"))
source_ids = {b["id"] for b in index["placements"] if b.get("source_asset")}
stats = []
for filename, center, distance in (("japanese_architecture_kit.blend", (450, -440, 35), 1250),
                                    ("placed_buildings.blend", (-1300, -3800, 15), 6400),
                                    ("source_asian.blend", (280, -280, 80), 1250),
                                    ("source_lowpoly.blend", (280, -280, 100), 1250),
                                    ("source_japanese.blend", (0, 0, 13), 55)):
    path = OUT / filename
    bpy.ops.wm.open_mainfile(filepath=str(path))
    bpy.context.scene.unit_settings.system = "METRIC"
    bpy.context.scene.unit_settings.scale_length = 1
    bpy.context.scene.unit_settings.length_unit = "METERS"
    if filename == "japanese_architecture_kit.blend":
        for name in ("component_rooftop_ac", "component_shop_sign"):
            collection = bpy.data.collections[name]
            if collection.get("nox_forward") != "+Z":
                for obj in collection.objects:
                    obj.data.transform(Matrix.Rotation(math.pi, 4, 'Z'))
                collection["nox_forward"] = "+Z"
    for material in bpy.data.materials:
        for style, name in enumerate(("apartment", "shop", "office")):
            if material.name.split(".")[0] == "room_" + name:
                material["nox"] = {"interior": {
                    "depth": 6.0 if style == 1 else 4.5, "strength": 1,
                    "style": style, "lighting": 0.65 if style == 1 else 0.26,
                    "blinds": 0.06 if style == 1 else 0.28, "pane": [0.8, 1.0],
                    "sill": 0.75, "seed": style + 0.25}}
    before, after = 0, 0
    if filename in {"japanese_architecture_kit.blend", "placed_buildings.blend"}:
        centers = {}
        for obj in bpy.data.objects:
            if obj.type == "MESH" and obj.data.materials and obj.data.materials[0].name.startswith("fuji_sphere_titanium"):
                points = [v.co for v in obj.data.vertices]
                centers[(obj.get("building_id"), obj.get("lod", -1))] = Vector(tuple((min(p[i] for p in points) + max(p[i] for p in points)) / 2 for i in range(3)))
        meshes = set()
        for obj in bpy.data.objects:
            if obj.type != "MESH" or obj.data in meshes or obj.get("building_id") in source_ids:
                continue
            if filename == "placed_buildings.blend" and not obj.get("building_id"):
                continue
            mesh = obj.data
            meshes.add(mesh)
            before += len(mesh.vertices)
            bm = bmesh.new()
            bm.from_mesh(mesh)
            bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=0.00001)
            bm.to_mesh(mesh)
            bm.free()
            if mesh.materials and mesh.materials[0].name.startswith("fuji_sphere_"):
                sphere_center = centers[(obj.get("building_id"), obj.get("lod", -1))]
                mesh.normals_split_custom_set_from_vertices([(v.co - sphere_center).normalized() for v in mesh.vertices])
            after += len(mesh.vertices)
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == "VIEW_3D":
                view = area.spaces.active.region_3d
                view.view_location = center
                view.view_distance = distance
                view.view_rotation = Vector((1, -1.2, 1.3)).to_track_quat('Z', 'Y')
                area.spaces.active.clip_end = 20000
    bpy.ops.wm.save_as_mainfile(filepath=str(path), compress=True)
    stats.append({"file": filename, "vertices_before": before, "vertices_after": after,
                  "uv_policy": "Per-corner UV discontinuities preserved when coincident vertices are welded"})
    print(filename, before, "->", after, flush=True)
(ROOT / "data/shuto/buildings/editable_topology.json").write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")
