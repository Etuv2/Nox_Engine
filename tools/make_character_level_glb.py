#!/usr/bin/env python3
"""Writes the character demo level as a binary glTF (GLB): a ground plane, a 30 degree ramp, a staircase of 0.25 m
steps, a 3 m wall and a box. Y is up, units are metres. The same file is the render model and the mesh collider
(scenes/test_character_level.json), so what the player walks on is what is drawn.

Usage: python tools/make_character_level_glb.py <output directory>\n\nWrites level.glb (the level) and player.glb (a 0.8 m x 1.8 m x 0.8 m block standing on y = 0, the visual of the\ncharacter capsule of radius 0.4 m and height 1.8 m).
"""
import json
import math
import struct
import sys


def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def v_norm(a):
    length = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
    if length < 1e-12:
        return (0.0, 1.0, 0.0)
    return (a[0] / length, a[1] / length, a[2] / length)


class Mesh:
    """Flat-shaded triangle soup: every triangle gets its own three vertices and face normal."""

    def __init__(self):
        self.positions = []
        self.normals = []

    def tri(self, a, b, c, outward=None):
        # The winding is chosen so the face normal points along `outward` (renderer and lighting agree on the side).
        n = v_norm(v_cross(v_sub(b, a), v_sub(c, a)))
        if outward is not None and n[0] * outward[0] + n[1] * outward[1] + n[2] * outward[2] < 0.0:
            b, c = c, b
            n = (-n[0], -n[1], -n[2])
        for p in (a, b, c):
            self.positions.append(p)
            self.normals.append(n)

    def quad(self, a, b, c, d, outward=None):
        self.tri(a, b, c, outward)
        self.tri(a, c, d, outward)

    def box(self, lo, hi):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        p = lambda x, y, z: (x, y, z)
        # Top, bottom and four sides.
        self.quad(p(x0, y1, z0), p(x0, y1, z1), p(x1, y1, z1), p(x1, y1, z0), (0, 1, 0))
        self.quad(p(x0, y0, z0), p(x1, y0, z0), p(x1, y0, z1), p(x0, y0, z1), (0, -1, 0))
        self.quad(p(x0, y0, z1), p(x1, y0, z1), p(x1, y1, z1), p(x0, y1, z1), (0, 0, 1))
        self.quad(p(x0, y0, z0), p(x0, y1, z0), p(x1, y1, z0), p(x1, y0, z0), (0, 0, -1))
        self.quad(p(x1, y0, z0), p(x1, y1, z0), p(x1, y1, z1), p(x1, y0, z1), (1, 0, 0))
        self.quad(p(x0, y0, z0), p(x0, y0, z1), p(x0, y1, z1), p(x0, y1, z0), (-1, 0, 0))


def build_level():
    m = Mesh()
    # Ground: top at y = 0, 400 m square (the walk never reaches its edge in a demo run).
    m.quad((-200.0, 0.0, -200.0), (200.0, 0.0, -200.0), (200.0, 0.0, 200.0), (-200.0, 0.0, 200.0), (0, 1, 0))
    # 30 degree ramp on the +x side, from x = 10 (y = 0) to x = 20.
    t = math.tan(math.radians(30.0))
    m.quad((10.0, 0.0, -6.0), (20.0, 10.0 * t, -6.0), (20.0, 10.0 * t, 6.0), (10.0, 0.0, 6.0), (-0.5, 0.866, 0.0))
    # Staircase on the -x side: four 0.25 m risers, each 2 m deep, from x = -12 to x = -20.
    for i in range(4):
        top = 0.25 * (i + 1)
        m.box((-14.0 - 2.0 * i, 0.0, -6.0), (-12.0 - 2.0 * i, top, 6.0))
    # A 3 m wall across the +z side (too tall to step over) and a box to walk around.
    m.box((-22.0, 0.0, -8.0), (-21.0, 3.0, 8.0))
    m.box((2.0, 0.0, 4.0), (4.0, 1.0, 6.0))
    return m


def build_glb(mesh):
    count = len(mesh.positions)
    pos_bytes = b"".join(struct.pack("<3f", *p) for p in mesh.positions)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in mesh.normals)
    idx_bytes = b"".join(struct.pack("<I", i) for i in range(count))
    binary = pos_bytes + nrm_bytes + idx_bytes
    xs = [p[0] for p in mesh.positions]
    ys = [p[1] for p in mesh.positions]
    zs = [p[2] for p in mesh.positions]
    gltf = {
        "asset": {"version": "2.0", "generator": "nox tools/make_character_level_glb.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "Level", "mesh": 0}],
        "meshes": [{"name": "Level", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3",
             "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5125, "count": count, "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes), "byteLength": len(idx_bytes), "target": 34963},
        ],
        "buffers": [{"byteLength": len(binary)}],
    }
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    binary += b"\0" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(json_bytes) + 8 + len(binary)
    out = struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<I4s", len(json_bytes), b"JSON") + json_bytes
    out += struct.pack("<I4s", len(binary), b"BIN\0") + binary
    return out


def build_player():
    m = Mesh()
    m.box((-0.4, 0.0, -0.4), (0.4, 1.8, 0.4))
    return m


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out_dir = sys.argv[1].rstrip("/\\")
    for name, mesh in (("level.glb", build_level()), ("player.glb", build_player())):
        path = out_dir + "/" + name
        with open(path, "wb") as f:
            f.write(build_glb(mesh))
        print("wrote %s: %d triangles" % (path, len(mesh.positions) // 3))
    return 0


if __name__ == "__main__":
    sys.exit(main())
