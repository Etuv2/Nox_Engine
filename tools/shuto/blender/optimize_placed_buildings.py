"""Migrate multipart IDs and consolidate distant facade tints into shared chunk materials."""
from collections import defaultdict
import json
from pathlib import Path
import re
import sys

import bpy
from mathutils import Vector

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_architecture as a

ROOT, OUT = a.ROOT, a.OUT
plan = json.loads((ROOT / "data/shuto/buildings/placements.json").read_text(encoding="utf-8"))
index = json.loads((OUT / "section_index.json").read_text(encoding="utf-8"))
path = OUT / "placed_buildings.blend"
bpy.ops.wm.open_mainfile(filepath=str(path))
parts = {(b["legacy_id"], b["legacy_part_index"]): b for b in plan["buildings"] if "legacy_id" in b}
for (legacy, number), b in sorted(parts.items()):
    old = legacy if number == 0 else legacy + ".%03d" % number
    collection = bpy.data.collections.get(old)
    if collection is None:
        assert bpy.data.collections.get(b["id"]) is not None, old
        continue
    collection.name = b["id"]
    for obj in collection.objects:
        obj["building_id"] = b["id"]
        if obj.type == "MESH":
            obj.name = b["id"] + "_" + obj.data.materials[0].name
seen = defaultdict(int)
for record in index["placements"]:
    legacy = record["id"]
    part = parts.get((legacy, seen[legacy]))
    seen[legacy] += 1
    if part:
        record["id"] = part["id"]
        record["seed_basis"] = part["seed_basis"]

fuji = next(b for b in plan["buildings"] if b.get("landmark") == "fuji")
cx, cz = fuji["obb"]["c"]
ux, uz = fuji["obb"]["u"]
z = 4.5 + fuji.get("model_offset_m", 0)
import fuji_tv
a.SPHERE_CENTERS[fuji["id"]] = Vector((cx + ux * 39 + uz * z, -(cz + uz * 39 - ux * z), 2.9 + fuji_tv.SPHERE_Y))
a.ROOMS.update({"room_" + name: {"depth": 6.0 if i == 1 else 4.5, "strength": 1, "style": i,
                              "lighting": 0.65 if i == 1 else 0.26, "blinds": 0.06 if i == 1 else 0.28,
                              "pane": [0.8, 1.0], "sill": 0.75, "seed": i + 0.25}
                for i, name in enumerate(("apartment", "shop", "office"))})
chunks = defaultdict(a.g.Buckets)
before = defaultdict(set)
changed = 0
valid_ids = {b["id"] for b in plan["buildings"]}
for obj in list(bpy.data.objects):
    if obj.type != "MESH" or obj.get("building_id") not in valid_ids:
        continue
    chunk = tuple(int(v) for v in obj["chunk"].split("_"))
    mesh = obj.data
    mesh.calc_loop_triangles()
    for triangle in mesh.loop_triangles:
        mat = mesh.materials[triangle.material_index]
        before[chunk].add(mat.name)
        canonical = re.sub(r"^(style_.+)_t[012]$", r"\1_t1", mat.name)
        if canonical != mat.name:
            replacement = bpy.data.materials.get(canonical)
            assert replacement is not None, canonical
            mesh.materials[triangle.material_index] = replacement
            mat = replacement
            changed += 1
        a.MATS[mat.name] = mat
        key = (*chunk, mat.name)
        bk = chunks[chunk]
        for loop in triangle.loops:
            bk.pos[key].extend(obj.matrix_world @ mesh.vertices[mesh.loops[loop].vertex_index].co)
            bk.uv[key].extend(mesh.uv_layers.active.data[loop].uv)
        bk.tris[key] += 1
bpy.ops.file.pack_all()
bpy.ops.wm.save_as_mainfile(filepath=str(path), compress=True)
for chunk, bk in sorted(chunks.items()):
    name = "%d_%d" % chunk
    objs = a.objects_from(bk, "chunk_" + name)
    a.export(objs, OUT / "render" / (name + ".glb"))
    for obj in objs:
        bpy.data.objects.remove(obj, do_unlink=True)
    for c in index["chunks"]:
        if c["id"] == name:
            c.update(triangles=sum(bk.tris.values()), render_primitives=len(bk.pos))
index["render_triangles"] = sum(c["triangles"] for c in index["chunks"])
a.save_json(OUT / "section_index.json", index)
a.save_json(ROOT / "data/shuto/buildings/material_optimization.json",
            {"before_chunk_material_groups": sum(len(v) for v in before.values()),
             "after_chunk_material_groups": sum(len(bk.pos) for bk in chunks.values()),
             "triangles": index["render_triangles"], "multipart_ids_migrated": len(parts),
             "change": "Shared neutral tint for mid/far facade styles; close geometric facades keep four surface variations"})
print("optimized groups", sum(len(v) for v in before.values()), "->", sum(len(bk.pos) for bk in chunks.values()), flush=True)
