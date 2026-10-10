"""Top-down plot of collision GLBs for debugging (ground by height, walls red, ceilings blue).

Usage: python -I tools/shuto/plot_collision.py out.png xmin zmin xmax zmax ymin ymax glb [glb ...]
Optional track: --track log.txt (lines with 'pos=(x,z)') draws the car path.
"""
import json
import math
import re
import struct
import sys

from PIL import Image, ImageDraw


def load_tris(path):
    b = open(path, 'rb').read()
    jl = struct.unpack('<I', b[12:16])[0]
    j = json.loads(b[20:20 + jl])
    off = 20 + jl
    bl = struct.unpack('<I', b[off:off + 4])[0]
    bin_ = b[off + 8:off + 8 + bl]

    def acc(i):
        a = j['accessors'][i]
        v = j['bufferViews'][a['bufferView']]
        o = v.get('byteOffset', 0) + a.get('byteOffset', 0)
        n = a['count']
        if a['type'] == 'VEC3':
            return [struct.unpack_from('<3f', bin_, o + k * (v.get('byteStride') or 12)) for k in range(n)]
        fmt = {5121: 'B', 5123: 'H', 5125: 'I'}[a['componentType']]
        return list(struct.unpack_from(f'<{n}{fmt}', bin_, o))

    def mat(n):
        if 'matrix' in n:
            m = n['matrix']
            return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
        t = n.get('translation', [0, 0, 0])
        s = n.get('scale', [1, 1, 1])
        q = n.get('rotation', [0, 0, 0, 1])
        x, y, z, w = q
        R = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
             [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
             [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
        return [[R[r][0] * s[0], R[r][1] * s[1], R[r][2] * s[2], t[r]] for r in range(3)] + [[0, 0, 0, 1]]

    def mul(a, b_):
        return [[sum(a[r][k] * b_[k][c] for k in range(4)) for c in range(4)] for r in range(4)]

    tris = []

    def visit(ni, M):
        n = j['nodes'][ni]
        M = mul(M, mat(n))
        if 'mesh' in n:
            for p in j['meshes'][n['mesh']]['primitives']:
                P = acc(p['attributes']['POSITION'])
                P = [tuple(M[r][0] * v[0] + M[r][1] * v[1] + M[r][2] * v[2] + M[r][3] for r in range(3)) for v in P]
                I = acc(p['indices']) if 'indices' in p else list(range(len(P)))
                for k in range(0, len(I) - 2, 3):
                    tris.append((P[I[k]], P[I[k + 1]], P[I[k + 2]]))
        for c in n.get('children', []):
            visit(c, M)
    I4 = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    for ni in j['scenes'][j.get('scene', 0)]['nodes']:
        visit(ni, I4)
    return tris


def main():
    args = sys.argv[1:]
    track = None
    if '--track' in args:
        i = args.index('--track')
        track = args[i + 1]
        del args[i:i + 2]
    out = args[0]
    x0, z0, x1, z1, y0, y1 = [float(v) for v in args[1:7]]
    W = 1200
    H = int(W * (z1 - z0) / (x1 - x0))
    img = Image.new('RGB', (W, H), (20, 20, 24))
    d = ImageDraw.Draw(img)
    S = lambda p: ((p[0] - x0) / (x1 - x0) * W, (p[2] - z0) / (z1 - z0) * H)
    walls = []
    for g in args[7:]:
        for a, b, c in load_tris(g):
            if max(a[0], b[0], c[0]) < x0 or min(a[0], b[0], c[0]) > x1 or max(a[2], b[2], c[2]) < z0 or min(a[2], b[2], c[2]) > z1:
                continue
            e1 = [b[i] - a[i] for i in range(3)]
            e2 = [c[i] - a[i] for i in range(3)]
            n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
            L = math.sqrt(sum(v * v for v in n)) or 1
            ny = n[1] / L
            ym = (a[1] + b[1] + c[1]) / 3
            if ym < y0 or ym > y1:
                continue
            if abs(ny) >= 0.6 and ny > 0:
                t = max(0.0, min(1.0, (ym - y0) / max(y1 - y0, 1e-3)))
                d.polygon([S(a), S(b), S(c)], fill=(int(40 + 180 * t), int(120 + 100 * t), int(60 + 40 * (1 - t))))
            elif abs(ny) < 0.6:
                walls.append((a, b, c))
    for a, b, c in walls:
        d.polygon([S(a), S(b), S(c)], outline=(255, 40, 40))
    if track:
        pts = []
        for line in open(track, encoding='utf-8', errors='ignore'):
            m = re.search(r'pos=\((-?[\d.]+),(-?[\d.]+)\)', line)
            if m:
                pts.append((float(m.group(1)), 0, float(m.group(2))))
        for p, q in zip(pts, pts[1:]):
            d.line([S(p), S(q)], fill=(80, 180, 255), width=3)
    img.save(out)
    print('wrote', out, W, H)


if __name__ == '__main__':
    main()
