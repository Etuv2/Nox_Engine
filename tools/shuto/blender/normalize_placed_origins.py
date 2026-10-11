"""Repair editable placement origins from older builder runs without changing world geometry."""
import json
from pathlib import Path
import bpy
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parents[3]
plan = json.loads((ROOT / "data/shuto/buildings/placements.json").read_text(encoding="utf-8"))
buildings = {b["id"]: b for b in plan["buildings"]}
path = ROOT / "models/shuto/buildings/placed_buildings.blend"
bpy.ops.wm.open_mainfile(filepath=str(path))
changed = 0
for obj in bpy.data.objects:
    bid = obj.get("building_id")
    if obj.type != "MESH" or bid not in buildings or obj.location.length > 0.01:
        continue
    b = buildings[bid]
    pivot = Vector((b["obb"]["c"][0], -b["obb"]["c"][1], 2.9))
    obj.data.transform(Matrix.Translation(-pivot))
    obj.location = pivot
    changed += 1
bpy.ops.wm.save_as_mainfile(filepath=str(path), compress=True)
print("normalized placed origins", changed)
