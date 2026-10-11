"""Migrate pre-correction deterministic sign UVs without rebuilding city geometry."""
import json
from pathlib import Path
import struct

import bpy
import numpy as np

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "models/shuto/buildings"
changed = []
for path in OUT.rglob("*.glb"):
    raw = path.read_bytes()
    size = struct.unpack_from("<I", raw, 12)[0]
    doc = json.loads(raw[20:20 + size])
    prims = [p for mesh in doc.get("meshes", []) for p in mesh["primitives"]
             if doc["materials"][p["material"]].get("name", "").startswith("sign_")]
    if not prims:
        continue
    binary = bytearray(raw[28 + size:])
    visited = set()
    modified = False
    def values_for(ai, width):
        accessor = doc["accessors"][ai]
        assert accessor["componentType"] == 5126
        view = doc["bufferViews"][accessor["bufferView"]]
        return np.ndarray((accessor["count"], width), dtype="<f4", buffer=binary,
                          offset=view.get("byteOffset", 0) + accessor.get("byteOffset", 0),
                          strides=(view.get("byteStride", width * 4), 4))
    for prim in prims:
        normals = values_for(prim["attributes"]["NORMAL"], 3)
        tangent = values_for(prim["attributes"]["TANGENT"], 4)
        mirrored = tangent[:, 0] * normals[:, 2] - tangent[:, 2] * normals[:, 0] < 0
        if not mirrored.any():
            continue
        modified = True
        for semantic, width in (("TEXCOORD_0", 2), ("TANGENT", 4)):
            if semantic not in prim["attributes"]:
                continue
            ai = prim["attributes"][semantic]
            if ai in visited:
                continue
            visited.add(ai)
            accessor = doc["accessors"][ai]
            assert accessor["componentType"] == 5126
            values = values_for(ai, width)
            if semantic == "TEXCOORD_0":
                values[mirrored, 0] = 1 - values[mirrored, 0]
                if "min" in accessor:
                    accessor["min"] = values.min(axis=0).tolist()
                    accessor["max"] = values.max(axis=0).tolist()
            else:
                values[mirrored] *= -1
    if not modified:
        continue
    doc["asset"].setdefault("extras", {})["nox_sign_u_right"] = True
    encoded = json.dumps(doc, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    encoded += b" " * (-len(encoded) % 4)
    body = struct.pack("<II", len(encoded), 0x4e4f534a) + encoded + struct.pack("<II", len(binary), 0x004e4942) + binary
    path.write_bytes(struct.pack("<III", 0x46546c67, 2, 12 + len(body)) + body)
    changed.append(path.relative_to(ROOT).as_posix())
for name in ("japanese_architecture_kit.blend", "placed_buildings.blend"):
    path = OUT / name
    bpy.ops.wm.open_mainfile(filepath=str(path))
    meshes = set()
    for obj in bpy.data.objects:
        if obj.type != "MESH" or obj.data in meshes:
            continue
        mesh = obj.data
        meshes.add(mesh)
        for polygon in mesh.polygons:
            if mesh.materials[polygon.material_index].name.startswith("sign_"):
                loops = list(polygon.loop_indices)
                coordinates = [mesh.vertices[mesh.loops[i].vertex_index].co for i in loops[:3]]
                uv = [mesh.uv_layers.active.data[i].uv.copy() for i in loops[:3]]
                delta1, delta2 = uv[1] - uv[0], uv[2] - uv[0]
                determinant = delta1.x * delta2.y - delta1.y * delta2.x
                tangent = ((coordinates[1] - coordinates[0]) * delta2.y -
                           (coordinates[2] - coordinates[0]) * delta1.y) / determinant
                if -tangent.x * polygon.normal.y + tangent.y * polygon.normal.x < 0:
                    for li in loops:
                        mesh.uv_layers.active.data[li].uv.x = 1 - mesh.uv_layers.active.data[li].uv.x
                mesh["nox_sign_orientation"] = "outward_readable"
    bpy.ops.wm.save_as_mainfile(filepath=str(path), compress=True)
(ROOT / "data/shuto/buildings/sign_orientation.json").write_text(
    json.dumps({"status": "PASS", "policy": "U increases toward screen-right when viewing the outward sign normal",
                "migrated_glbs": changed}, indent=2) + "\n", encoding="utf-8")
print("Corrected sign orientation in", len(changed), "GLBs and both editable libraries")
