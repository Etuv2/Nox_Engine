"""Validate exported geometry, parallax pane contract, resources and placement IDs."""
import argparse
import json
import math
from pathlib import Path
import struct
from urllib.parse import unquote

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "models/shuto/buildings"
VERIFIED_IMAGES = set()


def read(path):
    data = path.read_bytes()
    magic, version, length = struct.unpack_from("<III", data)
    assert magic == 0x46546C67 and version == 2 and length == len(data), str(path)
    size, kind = struct.unpack_from("<II", data, 12)
    assert kind == 0x4E4F534A
    doc = json.loads(data[20:20 + size].decode("utf-8"))
    bsize, bkind = struct.unpack_from("<II", data, 20 + size)
    assert bkind == 0x004E4942
    binary = data[28 + size:28 + size + bsize]
    return doc, binary


def accessor(doc, binary, index):
    acc = doc["accessors"][index]
    view = doc["bufferViews"][acc["bufferView"]]
    dtype = {5126: "<f4", 5125: "<u4", 5123: "<u2", 5121: "u1"}[acc["componentType"]]
    count = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    size = np.dtype(dtype).itemsize
    return np.ndarray((acc["count"], count), dtype=dtype, buffer=binary,
                      offset=view.get("byteOffset", 0) + acc.get("byteOffset", 0),
                      strides=(view.get("byteStride", size * count), size))


def check(path):
    doc, binary = read(path)
    textures = []
    for img in doc.get("images", []):
        if "uri" in img:
            uri = img["uri"]
            assert not uri.startswith(("/", "file:", "http:")) and ":" not in uri, uri
            resource = path.parent / unquote(uri)
            assert resource.is_file(), str(resource)
            if resource.resolve() not in VERIFIED_IMAGES:
                with Image.open(resource) as image:
                    image.verify()
                VERIFIED_IMAGES.add(resource.resolve())
            textures.append(resource.resolve())
    triangles, panes, zeros = 0, 0, 0
    lo, hi = np.full(3, np.inf), np.full(3, -np.inf)
    for mesh in doc.get("meshes", []):
        for prim in mesh["primitives"]:
            attrs = prim["attributes"]
            pos = accessor(doc, binary, attrs["POSITION"])
            assert np.isfinite(pos).all(), str(path)
            lo, hi = np.minimum(lo, pos.min(axis=0)), np.maximum(hi, pos.max(axis=0))
            assert "NORMAL" in attrs
            normals = accessor(doc, binary, attrs["NORMAL"])
            assert np.isfinite(normals).all()
            assert np.allclose(np.linalg.norm(normals, axis=1), 1, atol=1e-3), str(path)
            indices = accessor(doc, binary, prim["indices"]).reshape(-1)
            assert len(indices) % 3 == 0 and indices.max() < len(pos)
            faces = indices.reshape(-1, 3)
            cross = np.cross(pos[faces[:, 1]] - pos[faces[:, 0]], pos[faces[:, 2]] - pos[faces[:, 0]])
            area = np.linalg.norm(cross, axis=1)
            zeros += int((area < 1e-8).sum())
            triangles += len(faces)
            material = doc.get("materials", [])[prim["material"]]
            if material.get("name", "").startswith("sign_"):
                tangent = accessor(doc, binary, attrs["TANGENT"])
                right_dot = tangent[:, 0] * normals[:, 2] - tangent[:, 2] * normals[:, 0]
                assert (right_dot > 0.99).all(), "mirrored outward shop sign: " + str(path)
            if material.get("extras", {}).get("nox", {}).get("interior"):
                assert "TANGENT" in attrs and "TEXCOORD_0" in attrs
                tangent = accessor(doc, binary, attrs["TANGENT"])
                bitangent = np.cross(normals, tangent[:, :3]) * tangent[:, 3:4]
                assert (bitangent[:, 1] > 0.99).all(), "pane bitangent must be image-up: " + str(path)
                uv = accessor(doc, binary, attrs["TEXCOORD_0"])
                local = (uv - (np.floor(uv * 4) * 0.25 + 0.004)) / 0.242
                assert local.min() >= -1e-4 and local.max() <= 1.0001
                pane = np.asarray(material["extras"]["nox"]["interior"]["pane"])
                local[:, 1] = 1 - local[:, 1]
                w = (local - 0.5) * 2 * pane
                center = pos - tangent[:, :3] * w[:, :1] - bitangent * w[:, 1:2]
                assert np.max(np.linalg.norm(center[faces] - center[faces[:, :1]], axis=2)) < 2e-3, "unstable room seed"
                panes += len(faces) // 2
            elif material.get("name", "").startswith("wall_"):
                assert material.get("emissiveFactor", [0, 0, 0]) == [0, 0, 0], "emissive wall"
    assert zeros == 0, "%s: %d zero-area triangles" % (path, zeros)
    if triangles == 12 and path.parent.name in {"kit", "sources", "restricted_local"} and (path.name.endswith("_col.glb") or "_LOD2" in path.name):
        center = (lo + hi) / 2
        for mesh in doc["meshes"]:
            for primitive in mesh["primitives"]:
                positions = accessor(doc, binary, primitive["attributes"]["POSITION"])
                normals = accessor(doc, binary, primitive["attributes"]["NORMAL"])
                assert (np.sum((positions - center) * normals, axis=1) > 0).all(), "inward box proxy normal: " + str(path)
    return {"path": path.relative_to(ROOT).as_posix(), "triangles": triangles, "parallax_panes": panes,
            "bounds": [lo.tolist(), hi.tolist()], "external_images": len(textures), "bytes": path.stat().st_size}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", action="store_true")
    args = parser.parse_args()
    paths = [OUT / "probe/apartment_06.glb"] if args.probe else sorted(OUT.rglob("*.glb"))
    checks = [check(path) for path in paths]
    if args.probe:
        lo, hi = np.asarray(checks[0]["bounds"])
        assert np.allclose(lo[1], 0, atol=1e-4) and 21.9 < hi[1] < 22.1
        assert checks[0]["parallax_panes"] == 84
    else:
        plan = json.loads((ROOT / "data/shuto/buildings/placements.json").read_text(encoding="utf-8"))
        ids = [b["id"] for b in plan["buildings"]]
        assert len(ids) == len(set(ids))
        for b in plan["buildings"]:
            assert [b["cx"], b["cz"]] == [math.floor(x / 500) for x in b["chunk_anchor"]]
    report = ROOT / "data/shuto/buildings" / ("probe_validation.json" if args.probe else "export_validation.json")
    report.write_text(json.dumps({"status": "PASS", "checks": checks}, indent=2) + "\n", encoding="utf-8")
    print("PASS", len(checks), "GLBs", sum(c["triangles"] for c in checks), "triangles", sum(c["parallax_panes"] for c in checks), "parallax panes")


if __name__ == "__main__":
    main()
