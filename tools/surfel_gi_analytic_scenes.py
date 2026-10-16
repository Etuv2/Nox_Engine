#!/usr/bin/env python3
"""Analytic test scenes for the surfel GI estimator (plan section 13).

Writes three scenes into an output directory (default: %TEMP%/nox_surfel_E/analytic):

  furnace.json   Closed box, every wall albedo A and emission LE, no lights, no sky.
                 Incident irradiance on a wall: E = pi * LE / (1 - A). The composite
                 surf_term is giResponse * E, so with giResponse = albedo / pi it is
                 A * LE / (1 - A) (the outgoing radiance from the wall is LE / (1 - A)).
  whitesky.json  One upward-facing plane of albedo A under a uniform sky of radiance
                 L = SKY_TEXEL * skyScale, where skyScale = iblIntensity * diffuseIBLScale
                 clamped to 0.25 (0.03 with the defaults). Irradiance: E = pi * L.
                 Expected surf_term: A * L.
  whitesky_sky.hdr  Uniform RGBE environment map (value SKY_TEXEL).

Usage: python tools/surfel_gi_analytic_scenes.py [OUT_DIR]
The engine only opens scene files inside the project root, so the default OUT_DIR is
<repo>/build/surfel_gi_analytic (git-ignored) and the scenes refer to their assets by repo-relative path.
"""
import json
import os
import struct
import sys

ALBEDO = 0.5
EMISSION = 0.25          # furnace wall emission LE (radiance)
SKY_TEXEL = 10.0         # white-sky environment radiance before the sky scale
SKY_SCALE = 0.03         # iblIntensity 0.1 * diffuseIBLScale 0.3 (engine default)
HALF = 3.0               # furnace half size, metres
# KHR_materials_specular factor for the analytic materials. 0 gives F0 = 0, so the Fresnel energy term kD in the
# composite is exactly 1 and the expected values below hold exactly. None keeps the engine default material.
SPECULAR_FACTOR = None


def _pad(data: bytes, alignment: int, fill: bytes) -> bytes:
    rem = len(data) % alignment
    return data if rem == 0 else data + fill * (alignment - rem)


def write_glb(path, positions, normals, indices, material, extensions_used=("KHR_materials_emissive_strength",)):
    """Minimal GLB with one mesh, one primitive and one material (see the glTF 2.0 spec)."""
    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
    idx_bytes = _pad(idx_bytes, 4, b"\x00")
    binary = pos_bytes + nrm_bytes + idx_bytes
    mins = [min(p[i] for p in positions) for i in range(3)]
    maxs = [max(p[i] for p in positions) for i in range(3)]
    gltf = {
        "asset": {"version": "2.0"},
        "extensionsUsed": list(extensions_used),
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                                    "indices": 2, "material": 0}]}],
        "materials": [material],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes),
             "byteLength": len(indices) * 2, "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
             "min": mins, "max": maxs},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
    }
    json_bytes = _pad(json.dumps(gltf, separators=(",", ":")).encode("utf-8"), 4, b" ")
    bin_chunk = _pad(binary, 4, b"\x00")
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_chunk)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(json_bytes), b"JSON"))
        f.write(json_bytes)
        f.write(struct.pack("<I4s", len(bin_chunk), b"BIN\x00"))
        f.write(bin_chunk)


def _quad_triangles(corners, inward_normal):
    """Two triangles for a quad (corners in order), wound so the geometric normal points inward."""
    p0, p1, p2, p3 = corners
    tris = [(0, 1, 2), (0, 2, 3)]
    out = []
    for a, b, c in tris:
        ab = [corners[b][i] - corners[a][i] for i in range(3)]
        ac = [corners[c][i] - corners[a][i] for i in range(3)]
        cross = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]]
        dot = sum(cross[i] * inward_normal[i] for i in range(3))
        out.append((a, b, c) if dot >= 0 else (a, c, b))
    return out


def build_furnace_mesh():
    positions, normals, indices = [], [], []
    h = HALF
    for axis in range(3):
        for sign in (-1.0, 1.0):
            outward = [0.0, 0.0, 0.0]
            outward[axis] = sign
            inward = [-x for x in outward]
            u_axis, v_axis = [(axis + 1) % 3, (axis + 2) % 3]
            corners = []
            for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                p = [0.0, 0.0, 0.0]
                p[axis] = sign * h
                p[u_axis] = su * h
                p[v_axis] = sv * h
                corners.append(tuple(p))
            base = len(positions)
            positions.extend(corners)
            normals.extend([tuple(inward)] * 4)
            for tri in _quad_triangles(corners, inward):
                indices.extend(base + i for i in tri)
    return positions, normals, indices


def build_plane_mesh(size=40.0, height=0.0):
    corners = [(-size, height, -size), (size, height, -size), (size, height, size), (-size, height, size)]
    up = [0.0, 1.0, 0.0]
    tris = _quad_triangles(corners, up)
    indices = [i for tri in tris for i in tri]
    return list(corners), [tuple(up)] * 4, indices


def write_uniform_hdr(path, value, width=64, height=32):
    """Radiance RGBE (uncompressed scanlines; first pixel byte is not 2, so no RLE)."""
    mant, exp = _frexp(value)
    mantissa = int(mant * 256.0)
    pixel = bytes([mantissa, mantissa, mantissa, exp + 128])
    header = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (height, width)
    with open(path, "wb") as f:
        f.write(header)
        f.write(pixel * (width * height))


def _frexp(value):
    import math
    m, e = math.frexp(value)
    return m, e


def camera(position, yaw, pitch):
    return {"far": 50.0, "fov": 55.0, "mouse_sensitivity": 0.1, "movement_speed": 2.5,
            "near": 0.05, "pitch": pitch, "position": position, "yaw": yaw}


def write_scene(path, name, cam, model_path, skybox, exposure=1.0):
    scene = {
        "camera": cam,
        "exposure": exposure,
        "exposure_mode": "manual",
        "gamma": 2.2,
        "nodes": [{
            "components": {"MeshRenderer": {"culling": "inherit",
                                            "exclude_from_reflection_capture": False,
                                            "model": model_path,
                                            "render_whole_model": False}},
            "path": model_path,
            "position": [0.0, 0.0, 0.0],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [1.0, 1.0, 1.0],
            "type": "model",
        }, {
            # A zero-intensity sun. An enabled light stops the engine registering its default MainDirectionalLight
            # (Core.cpp), which otherwise lights the interior hits of the furnace through its shadow map (the shadow
            # lookup treats points outside the cascades as lit). The analytic tests are sun-free.
            "components": {"Light": {"cast_shadows": False, "color": [1.0, 1.0, 1.0], "enabled": True,
                                     "intensity": 0.0, "range": 10.0, "source_radius": 0.0}},
            "light": {"castsShadows": False, "color": [1.0, 1.0, 1.0], "enabled": True, "intensity": 0.0,
                      "shadowSize": 2048, "splitLambda": 0.8, "type": "directional"},
            "name": "NoSun",
            "position": [0.0, 3.5, 2.0],
            "rotation": [-65.0, 0.0, 0.0],
            "type": "light",
        }],
        "scene_name": name,
        "skybox": skybox,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, indent=2)


def main(argv):
    global ALBEDO, EMISSION, SKY_TEXEL, SPECULAR_FACTOR
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("out", nargs="?", default=None, help="output directory (default build/surfel_gi_analytic)")
    parser.add_argument("--albedo", type=float, default=ALBEDO)
    parser.add_argument("--emission", type=float, default=EMISSION)
    parser.add_argument("--sky-texel", type=float, default=SKY_TEXEL)
    parser.add_argument("--plane-height", type=float, default=0.0)
    parser.add_argument("--specular-factor", type=float, default=None,
                        help="KHR_materials_specular factor (0 removes the Fresnel term kD)")
    args = parser.parse_args(argv[1:])
    ALBEDO, EMISSION, SKY_TEXEL = args.albedo, args.emission, args.sky_texel
    SPECULAR_FACTOR = args.specular_factor
    spec_used = ("KHR_materials_emissive_strength",) + (("KHR_materials_specular",) if SPECULAR_FACTOR is not None else ())
    def with_spec(material):
        if SPECULAR_FACTOR is not None:
            material["extensions"]["KHR_materials_specular"] = {"specularFactor": SPECULAR_FACTOR}
        return material
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = args.out if args.out else os.path.join(repo, "build", "surfel_gi_analytic")
    os.makedirs(out, exist_ok=True)
    # Engine-relative paths: the scene and its assets must sit under the project root.
    rel_dir = os.path.relpath(os.path.abspath(out), repo).replace("\\", "/")
    ab = lambda name: os.path.join(out, name)
    rel = lambda name: f"{rel_dir}/{name}"

    furnace_glb = ab("furnace.glb")
    pos, nrm, idx = build_furnace_mesh()
    write_glb(furnace_glb, pos, nrm, idx, with_spec({
        "name": "furnace",
        "doubleSided": True,
        "emissiveFactor": [1.0, 1.0, 1.0],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": EMISSION}},
        "pbrMetallicRoughness": {"baseColorFactor": [ALBEDO, ALBEDO, ALBEDO, 1.0],
                                 "metallicFactor": 0.0, "roughnessFactor": 1.0},
    }), extensions_used=spec_used)
    write_scene(ab("furnace.json"), "surfel_gi_analytic_furnace",
                camera([0.0, 0.0, 0.0], -90.0, 0.0), rel("furnace.glb"), "")

    plane_glb = ab("whitesky_plane.glb")
    pos, nrm, idx = build_plane_mesh(height=args.plane_height)
    write_glb(plane_glb, pos, nrm, idx, with_spec({
        "name": "plane",
        "doubleSided": True,
        "extensions": {},
        "pbrMetallicRoughness": {"baseColorFactor": [ALBEDO, ALBEDO, ALBEDO, 1.0],
                                 "metallicFactor": 0.0, "roughnessFactor": 1.0},
    }), extensions_used=spec_used)
    sky_hdr = ab("whitesky_sky.hdr")
    write_uniform_hdr(sky_hdr, SKY_TEXEL)
    write_scene(ab("whitesky.json"), "surfel_gi_analytic_whitesky",
                camera([0.0, 6.0, 6.0], -90.0, -45.0), rel("whitesky_plane.glb"), rel("whitesky_sky.hdr"))

    print(f"wrote {out}")
    print(f"furnace: albedo={ALBEDO} emission={EMISSION} expected surf_term={ALBEDO * EMISSION / (1 - ALBEDO):.6f}"
          f" (E={3.141592653589793 * EMISSION / (1 - ALBEDO):.6f})")
    sky_l = SKY_TEXEL * SKY_SCALE
    print(f"whitesky: albedo={ALBEDO} sky L={sky_l:.6f} expected surf_term={ALBEDO * sky_l:.6f}"
          f" (E={3.141592653589793 * sky_l:.6f})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
