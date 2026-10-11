"""Index identical vertex records without moving geometry or merging UV/normal seams."""
import copy
import json
from pathlib import Path
import struct

import numpy as np

from validate_buildings import OUT, ROOT, accessor, read


def optimize(path):
    doc, original = read(path)
    if doc["asset"].get("extras", {}).get("nox_indexed_vertices"):
        return None
    assert not doc.get("animations") and not doc.get("skins")
    assert all("uri" in image for image in doc.get("images", []))
    old_accessors = doc["accessors"]
    new_accessors, views, binary = [], [], bytearray()
    before, after, planar_faces = 0, 0, 0

    def append(values, template, target):
        binary.extend(b"\0" * (-len(binary) % 4))
        payload = values.tobytes(order="C")
        vi = len(views)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(payload), "target": target})
        binary.extend(payload)
        ai = len(new_accessors)
        acc = copy.deepcopy(template)
        acc.pop("byteOffset", None)
        acc.pop("sparse", None)
        acc.update(bufferView=vi, count=len(values))
        new_accessors.append(acc)
        return ai

    for mesh in doc["meshes"]:
        for primitive in mesh["primitives"]:
            assert not primitive.get("targets") and primitive.get("mode", 4) == 4
            attributes = primitive["attributes"]
            arrays = {name: accessor(doc, original, ai).copy() for name, ai in attributes.items()}
            original_indices = accessor(doc, original, primitive["indices"]).ravel().copy()
            if path.parent.name in {"collision", "context_collision"}:
                positions = arrays["POSITION"].astype(float)
                faces = original_indices.reshape(-1, 3)
                horizontal = np.ptp(positions[faces, 1], axis=1) < 1e-5
                floor_y = positions[:, 1].min()
                for face_index in np.flatnonzero(horizontal):
                    face = faces[face_index]
                    direction = -1 if abs(positions[face, 1].mean() - floor_y) < 1e-4 else 1
                    cross_y = np.cross(positions[face[1]] - positions[face[0]], positions[face[2]] - positions[face[0]])[1]
                    if cross_y * direction < 0:
                        faces[face_index, 1], faces[face_index, 2] = faces[face_index, 2], faces[face_index, 1]
                    arrays["NORMAL"][face] = [0, direction, 0]
                    planar_faces += 1
                for semantic in ("TANGENT", "TEXCOORD_0"):
                    arrays.pop(semantic, None)
                    attributes.pop(semantic, None)
            records = np.concatenate([values.view(np.uint8).reshape(len(values), -1) for values in arrays.values()], axis=1)
            _, keep, remap = np.unique(records, axis=0, return_index=True, return_inverse=True)
            count = len(keep)
            before += len(records)
            after += count
            indices = remap[original_indices].astype("<u2" if count <= 65536 else "<u4").reshape(-1, 1)
            for name, old_index in list(attributes.items()):
                attributes[name] = append(arrays[name][keep], old_accessors[old_index], 34962)
            primitive["indices"] = append(indices, {"componentType": 5123 if count <= 65536 else 5125,
                                                    "type": "SCALAR", "min": [int(indices.min())],
                                                    "max": [int(indices.max())]}, 34963)
    binary.extend(b"\0" * (-len(binary) % 4))
    doc["accessors"], doc["bufferViews"], doc["buffers"] = new_accessors, views, [{"byteLength": len(binary)}]
    doc["asset"].setdefault("extras", {})["nox_indexed_vertices"] = True
    encoded = json.dumps(doc, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    encoded += b" " * (-len(encoded) % 4)
    body = struct.pack("<II", len(encoded), 0x4e4f534a) + encoded + struct.pack("<II", len(binary), 0x004e4942) + binary
    old_bytes = path.stat().st_size
    path.write_bytes(struct.pack("<III", 0x46546c67, 2, 12 + len(body)) + body)
    return {"path": path.relative_to(ROOT).as_posix(), "vertices_before": before, "vertices_after": after,
            "bytes_before": old_bytes, "bytes_after": path.stat().st_size, "planar_collision_faces_normalized": planar_faces}


def main():
    report = ROOT / "data/shuto/buildings/vertex_optimization.json"
    entries = {e["path"]: e for e in json.loads(report.read_text(encoding="utf-8"))["files"]} if report.exists() else {}
    changed = 0
    for path in sorted(OUT.rglob("*.glb")):
        entry = optimize(path)
        if entry:
            entries[entry["path"]] = entry
            changed += 1
    value = {"policy": "Exact byte-identical vertex attributes within each material primitive; UV/normal/tangent seams retained",
             "files": [entries[key] for key in sorted(entries)]}
    report.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    print("Indexed", changed, "GLBs; vertices", sum(e["vertices_before"] for e in entries.values()),
          "->", sum(e["vertices_after"] for e in entries.values()))


if __name__ == "__main__":
    main()
