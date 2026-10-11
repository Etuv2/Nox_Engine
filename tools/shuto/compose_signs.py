"""Compose Shuto navigation signs deterministically from the road graph.

Every diverge node (a node with two or more outgoing carriageways) inside the bounding box gets:
  * an advance sign on the upstream carriageway (nominal 500 m before the split, or as far as
    the upstream carriageway allows, min 150 m), showing the branch destinations and the actual
    distance along the road, rounded down to 50 m;
  * a gore sign 40 m before the split with arrows for each branch.
Destinations come only from data/shuto/network.json: exit names from the IC facility linked to
the exit ramp (or the ramp's own name), junction branches from the route reached downstream. No
text is invented; branches without a verified name are reported and get an arrow-only panel
(listed in the output report as gaps).

Era note (reference 2002): the alphanumeric route codes (C1, K1, ...) were introduced on signs in
2017, so panels show route names and Japanese route numbers instead of codes.

Text is rasterised with Pillow over the delivered sign_panel_blanks substrates (derived patches)
using Noto Sans JP (SIL OFL 1.1, fonts/noto). A GLB with overhead gantries and textured panels is
written for the scene (pure python glTF writer, world coordinates).

Usage: python -I tools/shuto/compose_signs.py --bbox -2600,1500,0,6000 --name rep_section
"""
import argparse
import io
import json
import math
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
NETWORK = os.path.join(ROOT, 'data', 'shuto', 'network.json')
SUBSTRATE = os.path.join(ROOT, 'textures', 'shuto', 'derived', 'sign_panel_blanks_highway_green.png')
STEEL = os.path.join(ROOT, 'textures', 'shuto', 'derived', 'galvanized_steel_seamless.png')
FONT_BOLD = os.path.join(ROOT, 'fonts', 'noto', 'NotoSansJP-Bold.otf')
FONT_MED = os.path.join(ROOT, 'fonts', 'noto', 'NotoSansJP-Medium.otf')
PX_PER_M = 96
WHITE = (245, 247, 245)

# Route display names for the 2002 era (Japanese route number + name, English route name).
ROUTE_LABEL = {
    'C1': ('都心環状線', 'Inner Circular Route'),
    'C2': ('中央環状線', 'Central Circular Route'),
    'B': ('湾岸線', 'Bayshore Route'),
    '1-Haneda': ('1号羽田線', 'Route 1 Haneda'),
    '1-Ueno': ('1号上野線', 'Route 1 Ueno'),
    '2': ('2号目黒線', 'Route 2 Meguro'),
    '3': ('3号渋谷線', 'Route 3 Shibuya'),
    '4': ('4号新宿線', 'Route 4 Shinjuku'),
    '5': ('5号池袋線', 'Route 5 Ikebukuro'),
    '6-Mukojima': ('6号向島線', 'Route 6 Mukojima'),
    '6-Misato': ('6号三郷線', 'Route 6 Misato'),
    '7': ('7号小松川線', 'Route 7 Komatsugawa'),
    '9': ('9号深川線', 'Route 9 Fukagawa'),
    '11': ('11号台場線', 'Route 11 Daiba'),
}


def plan_len(points):
    return sum(math.hypot(b[0] - a[0], b[2] - a[2]) for a, b in zip(points, points[1:]))


def point_at(points, s):
    """Point and unit plan direction at arc length s."""
    acc = 0.0
    for a, b in zip(points, points[1:]):
        d = math.hypot(b[0] - a[0], b[2] - a[2])
        if d > 1e-6 and acc + d >= s:
            t = (s - acc) / d
            p = [a[i] + (b[i] - a[i]) * t for i in range(3)]
            return p, ((b[0] - a[0]) / d, (b[2] - a[2]) / d)
        acc += d
    a, b = points[-2], points[-1]
    d = max(math.hypot(b[0] - a[0], b[2] - a[2]), 1e-6)
    return list(b), ((b[0] - a[0]) / d, (b[2] - a[2]) / d)


def left_of(d):
    # NOX frame: facing (dx, dz) the car's left is (dz, -dx) (heading 0 = +Z, +X = left).
    return (d[1], -d[0])


class Graph:
    def __init__(self, net):
        self.net = net
        self.cw = {c['id']: c for c in net['carriageways']}
        self.out = {}
        self.inc = {}
        for c in net['carriageways']:
            self.out.setdefault(c['from'], []).append(c['id'])
            self.inc.setdefault(c['to'], []).append(c['id'])
        self.ic_of = {}
        for f in net.get('facilities', []):
            if f.get('type') == 'ic':
                for cid in f.get('carriageways', []):
                    self.ic_of.setdefault(cid, f)

    def downstream_main(self, cid, depth=0):
        """First main carriageway reached by following a branch (single successor chain)."""
        c = self.cw[cid]
        if c['kind'] == 'main' or depth > 6:
            return c
        nxt = self.out.get(c['to'], [])
        mains = [n for n in nxt if self.cw[n]['kind'] == 'main']
        if mains:
            return self.cw[mains[0]]
        if nxt:
            return self.downstream_main(nxt[0], depth + 1)
        return None

    def label(self, cid):
        """(lines_ja, lines_en, kind) for a branch, or None when no verified name exists."""
        c = self.cw[cid]
        if c['kind'] == 'ramp_off' or (c['kind'] == 'jct' and not self.out.get(c['to'])):
            f = self.ic_of.get(cid)
            name = (f or {}).get('name_ja') or ''
            name_en = (f or {}).get('name_en') or ''
            if not name and c.get('name_ja', '').endswith('出口'):
                name = c['name_ja']
            name = name.replace('出口', '').replace('入口', '').strip()
            if not name:
                return None
            return ([name], [name_en] if name_en else [], 'exit')
        main = self.downstream_main(cid)
        if main is None:
            return None
        if c['kind'] == 'ramp_off':
            return None
        ja, en = ROUTE_LABEL.get(main['route'], (None, None))
        if not ja:
            return None
        return ([ja], [en], 'route')


def font(path, px):
    return ImageFont.truetype(path, max(int(px), 8))


def draw_arrow(d, x, y, size, angle_deg, fill=WHITE):
    """Upward arrow rotated by angle (0 = straight ahead, + = to the right on the panel)."""
    a = math.radians(angle_deg)
    pts = [(0, -1.0), (0.55, -0.35), (0.2, -0.35), (0.2, 1.0), (-0.2, 1.0), (-0.2, -0.35), (-0.55, -0.35)]
    rot = [(px * math.cos(a) - py * math.sin(a), px * math.sin(a) + py * math.cos(a)) for px, py in pts]
    d.polygon([(x + px * size, y + py * size) for px, py in rot], fill=fill)


def compose_panel(entries, distance_m, width_m, height_m):
    """entries: list of (label, angle_deg). Returns a PIL image (RGB) for the panel."""
    W, H = int(width_m * PX_PER_M), int(height_m * PX_PER_M)
    sub = Image.open(SUBSTRATE).convert('RGB')
    img = Image.new('RGB', (W, H))
    for y in range(0, H, sub.height - 16):
        for x in range(0, W, sub.width - 16):
            img.paste(sub.crop((8, 8, sub.width - 8, sub.height - 8)), (x, y))
    d = ImageDraw.Draw(img)
    border = int(0.06 * PX_PER_M)
    d.rounded_rectangle([border, border, W - border, H - border], radius=int(0.18 * PX_PER_M), outline=WHITE, width=int(0.035 * PX_PER_M))
    cols = len(entries)
    col_w = (W - 2 * border) / cols
    for i, (lab, angle) in enumerate(entries):
        x0 = border + i * col_w
        cx = x0 + col_w / 2
        if i > 0:
            d.line([(x0, border * 2), (x0, H - border * 2)], fill=WHITE, width=int(0.03 * PX_PER_M))
        draw_arrow(d, x0 + 0.42 * PX_PER_M, H * 0.5, 0.32 * PX_PER_M, angle)
        if lab is None:
            continue
        ja, en, kind = lab
        tx0 = x0 + 0.85 * PX_PER_M
        tw = col_w - 1.0 * PX_PER_M
        y = border + 0.12 * PX_PER_M
        size = 0.42 * PX_PER_M
        for line in ja:
            f = font(FONT_BOLD, size)
            while d.textlength(line, font=f) > tw and size > 10:
                size *= 0.92
                f = font(FONT_BOLD, size)
            d.text((tx0, y), line, font=f, fill=WHITE)
            y += size * 1.2
        esize = 0.2 * PX_PER_M
        for line in en:
            f = font(FONT_MED, esize)
            while d.textlength(line, font=f) > tw and esize > 8:
                esize *= 0.92
                f = font(FONT_MED, esize)
            d.text((tx0, y), line, font=f, fill=WHITE)
            y += esize * 1.25
        if kind == 'exit':
            f = font(FONT_BOLD, 0.2 * PX_PER_M)
            d.text((tx0, H - border - 0.36 * PX_PER_M), '出口 EXIT', font=f, fill=WHITE)
    if distance_m:
        f = font(FONT_BOLD, 0.3 * PX_PER_M)
        txt = f'{distance_m}m'
        tw = d.textlength(txt, font=f)
        d.text((W - border - tw - 0.15 * PX_PER_M, H - border - 0.45 * PX_PER_M), txt, font=f, fill=WHITE)
    return img


# ---------------------------------------------------------------------------------------------
# Minimal glTF (GLB) writer: textured quads and boxes in world space.
class GlbWriter:
    def __init__(self):
        self.bin = bytearray()
        self.j = {'asset': {'version': '2.0', 'generator': 'nox tools/shuto/compose_signs.py'},
                  'scene': 0, 'scenes': [{'nodes': []}], 'nodes': [], 'meshes': [], 'accessors': [],
                  'bufferViews': [], 'buffers': [], 'materials': [], 'textures': [], 'images': [],
                  'samplers': [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497},
                               {'magFilter': 9729, 'minFilter': 9987, 'wrapS': 33071, 'wrapT': 33071}]}

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        self.bin += data
        v = {'buffer': 0, 'byteOffset': off, 'byteLength': len(data)}
        if target:
            v['target'] = target
        self.j['bufferViews'].append(v)
        return len(self.j['bufferViews']) - 1

    def _acc(self, data, fmt, count, typ, ctype, target, mn=None, mx=None):
        view = self._view(data, target)
        a = {'bufferView': view, 'componentType': ctype, 'count': count, 'type': typ}
        if mn is not None:
            a['min'], a['max'] = mn, mx
        self.j['accessors'].append(a)
        return len(self.j['accessors']) - 1

    def material(self, name, png_bytes, clamp, roughness, metallic, emissive=0.0):
        buf = self._view(png_bytes)
        self.j['images'].append({'bufferView': buf, 'mimeType': 'image/png', 'name': name})
        self.j['textures'].append({'source': len(self.j['images']) - 1, 'sampler': 1 if clamp else 0})
        t = len(self.j['textures']) - 1
        m = {'name': name, 'pbrMetallicRoughness': {'baseColorTexture': {'index': t}, 'metallicFactor': metallic, 'roughnessFactor': roughness}}
        if emissive > 0.0:
            # Sign illumination (Shuto guide signs are externally lit at night): faint emission of the panel art.
            m['emissiveTexture'] = {'index': t}
            m['emissiveFactor'] = [1.0, 1.0, 1.0]
            m['extensions'] = {'KHR_materials_emissive_strength': {'emissiveStrength': emissive}}
            self.j.setdefault('extensionsUsed', [])
            if 'KHR_materials_emissive_strength' not in self.j['extensionsUsed']:
                self.j['extensionsUsed'].append('KHR_materials_emissive_strength')
        self.j['materials'].append(m)
        return len(self.j['materials']) - 1

    def mesh(self, name, pos, nrm, uv, idx, mat):
        n = len(pos)
        p = self._acc(struct.pack(f'<{3 * n}f', *[c for v in pos for c in v]), None, n, 'VEC3', 5126, 34962,
                      [min(v[i] for v in pos) for i in range(3)], [max(v[i] for v in pos) for i in range(3)])
        nn = self._acc(struct.pack(f'<{3 * n}f', *[c for v in nrm for c in v]), None, n, 'VEC3', 5126, 34962)
        t = self._acc(struct.pack(f'<{2 * n}f', *[c for v in uv for c in v]), None, n, 'VEC2', 5126, 34962)
        i = self._acc(struct.pack(f'<{len(idx)}I', *idx), None, len(idx), 'SCALAR', 5125, 34963)
        self.j['meshes'].append({'name': name, 'primitives': [{'attributes': {'POSITION': p, 'NORMAL': nn, 'TEXCOORD_0': t}, 'indices': i, 'material': mat}]})
        self.j['nodes'].append({'name': name, 'mesh': len(self.j['meshes']) - 1})
        self.j['scenes'][0]['nodes'].append(len(self.j['nodes']) - 1)

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.j['buffers'] = [{'byteLength': len(self.bin)}]
        js = json.dumps(self.j, separators=(',', ':'), ensure_ascii=True).encode('ascii')
        js += b' ' * ((-len(js)) % 4)
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, total))
            f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
            f.write(struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin))


class MeshBuilder:
    def __init__(self):
        self.pos, self.nrm, self.uv, self.idx = [], [], [], []

    def quad(self, a, b, c, d, n, uvs):
        base = len(self.pos)
        self.pos += [a, b, c, d]
        self.nrm += [n] * 4
        self.uv += uvs
        self.idx += [base, base + 1, base + 2, base, base + 2, base + 3]

    def box(self, center, ax, ay, az, hx, hy, hz, tile=1.0):
        """Oriented box: ax, ay, az unit axes, h* half extents. UVs in metres / tile."""
        def P(sx, sy, sz):
            return [center[i] + ax[i] * hx * sx + ay[i] * hy * sy + az[i] * hz * sz for i in range(3)]
        faces = [((1, 0, 0), ax, (0, 1, 0), (0, 0, 1), hy, hz), ((-1, 0, 0), [-v for v in ax], (0, 0, 1), (0, 1, 0), hz, hy),
                 ((0, 1, 0), ay, (0, 0, 1), (1, 0, 0), hz, hx), ((0, -1, 0), [-v for v in ay], (1, 0, 0), (0, 0, 1), hx, hz),
                 ((0, 0, 1), az, (1, 0, 0), (0, 1, 0), hx, hy), ((0, 0, -1), [-v for v in az], (0, 1, 0), (1, 0, 0), hy, hx)]
        for s, n, u, v, hu, hv in faces:
            def C(du, dv):
                return P(s[0] + u[0] * du + v[0] * dv, s[1] + u[1] * du + v[1] * dv, s[2] + u[2] * du + v[2] * dv)
            uu, vv = 2 * hu / tile, 2 * hv / tile
            self.quad(C(-1, -1), C(1, -1), C(1, 1), C(-1, 1), n, [[0, 0], [uu, 0], [uu, vv], [0, vv]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bbox', default='-2600,1500,0,6000')
    ap.add_argument('--name', default='rep_section')
    ap.add_argument('--advance', type=float, default=500.0)
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    x0, z0, x1, z1 = [float(v) for v in args.bbox.split(',')]
    net = json.load(open(NETWORK, encoding='utf-8'))
    g = Graph(net)
    nodes = {n['id']: n for n in net['nodes']}
    out_dir_tex = os.path.join(ROOT, 'textures', 'shuto', 'signs', args.name)
    os.makedirs(out_dir_tex, exist_ok=True)
    out_glb_dir = os.path.join(ROOT, 'models', 'shuto', args.name)
    os.makedirs(out_glb_dir, exist_ok=True)

    signs, gaps = [], []
    glb = GlbWriter()
    steel = open(STEEL, 'rb').read() if os.path.exists(STEEL) else None
    steel_mat = glb.material('gantry_galvanized', steel, False, 0.45, 0.9) if steel else None
    steel_mesh = MeshBuilder()

    for nid, outs in sorted(g.out.items()):
        if len(outs) < 2:
            continue
        p = nodes[nid]['pos']
        if not (x0 <= p[0] <= x1 and z0 <= p[2] <= z1):
            continue
        ins = g.inc.get(nid, [])
        if not ins:
            continue
        up = max(ins, key=lambda c: (g.cw[c]['kind'] == 'main', plan_len(g.cw[c]['points'])))
        upc = g.cw[up]
        # Branch directions 60 m past the split, relative to the upstream direction at the split.
        _, d_up = point_at(upc['points'], plan_len(upc['points']) - 0.5)
        entries = []
        for b in outs:
            bc = g.cw[b]
            q, _ = point_at(bc['points'], min(60.0, plan_len(bc['points'])))
            v = (q[0] - p[0], q[2] - p[2])
            L = math.hypot(*v) or 1.0
            v = (v[0] / L, v[1] / L)
            lft = left_of(d_up)
            side = v[0] * lft[0] + v[1] * lft[1]          # + = branch to the left
            ang = -math.degrees(math.asin(max(-1.0, min(1.0, side)))) * 0.9
            lab = g.label(b)
            if lab is None:
                gaps.append({'node': nid, 'branch': b, 'reason': 'no verified destination name'})
            entries.append((side, lab, ang, b))
        entries.sort(key=lambda e: -e[0])                   # left to right on the panel
        L_up = plan_len(upc['points'])
        placements = []
        if L_up >= 150.0:
            dist = min(args.advance, L_up - 20.0)
            placements.append(('advance', dist, int(dist // 50 * 50)))
        if L_up >= 60.0:
            placements.append(('gore', 40.0, 0))
        for kind, back, shown in placements:
            sid = f'{args.name}.{nid}.{kind}'
            width = 3.2 * len(entries)
            height = 2.2
            img = compose_panel([(e[1], e[2]) for e in entries], shown if kind == 'advance' else 0, width, height)
            fn = f'{nid}_{kind}.png'
            img.save(os.path.join(out_dir_tex, fn))
            pos, d = point_at(upc['points'], L_up - back)
            lanes_w = upc.get('lanes', 2) * upc.get('lane_width', 3.25)
            span = lanes_w + upc.get('shoulder_left', 0.75) + upc.get('shoulder_right', 0.5) + 1.2
            clear = 5.0 + height * 0.5 + 0.3                  # 5.0 m vertical clearance to the panel bottom
            lft = left_of(d)
            right = (-lft[0], -lft[1])
            fwd = (d[0], 0.0, d[1])
            # Panel facing the driver (normal = -fwd), centred over the carriageway.
            c = [pos[0], pos[1] + clear, pos[2]]
            hw, hh = width * 0.5, height * 0.5
            R3 = (right[0], 0.0, right[1])
            A = [c[0] - R3[0] * hw, c[1] - hh, c[2] - R3[2] * hw]
            B = [c[0] + R3[0] * hw, c[1] - hh, c[2] + R3[2] * hw]
            C = [c[0] + R3[0] * hw, c[1] + hh, c[2] + R3[2] * hw]
            D = [c[0] - R3[0] * hw, c[1] + hh, c[2] - R3[2] * hw]
            buf = io.BytesIO()
            img.save(buf, 'PNG')
            mat = glb.material('sign_' + sid, buf.getvalue(), True, 0.55, 0.0, emissive=0.6)
            mb = MeshBuilder()
            n = [-fwd[0], 0.0, -fwd[2]]
            mb.quad(A, B, C, D, n, [[0, 1], [1, 1], [1, 0], [0, 0]])
            mb.quad(B, A, D, C, fwd, [[0.02, 0.02], [0.02, 0.02], [0.02, 0.02], [0.02, 0.02]])
            glb.mesh('sign_' + sid, mb.pos, mb.nrm, mb.uv, mb.idx, mat)
            # Gantry: two posts outside the shoulders and a beam above the panel. Posts are only built
            # where the carriageway is at grade (base on the ground, not in the air beside an elevated
            # or bridge deck, and not in a cut or tunnel). Elevated panels have no gantry (documented gap).
            s_here = L_up - back
            stype = 'at_grade'
            for rng in upc.get('structure', []):
                if rng['s0'] <= s_here < rng['s1']:
                    stype = rng['type']
            on_grade = stype == 'at_grade' and abs(pos[1] - float(net.get('ground_y', 3.0))) <= 1.5
            if steel_mat is not None and on_grade:
                for sgn in (-1.0, 1.0):
                    base = [pos[0] + R3[0] * span * 0.5 * sgn, pos[1], pos[2] + R3[2] * span * 0.5 * sgn]
                    steel_mesh.box([base[0], base[1] + (clear + hh) * 0.5, base[2]], list(R3), [0, 1, 0], list(fwd), 0.18, (clear + hh) * 0.5, 0.18)
                steel_mesh.box([c[0] + fwd[0] * 0.25, c[1] + hh - 0.2, c[2] + fwd[2] * 0.25], list(R3), [0, 1, 0], list(fwd), span * 0.5, 0.2, 0.15)
            signs.append({'id': sid, 'kind': kind, 'node': nid, 'carriageway': up, 's': round(L_up - back, 2),
                          'pos': [round(v, 3) for v in c], 'heading_deg': round(math.degrees(math.atan2(d[0], d[1])), 2),
                          'size_m': [width, height], 'texture': os.path.relpath(os.path.join(out_dir_tex, fn), ROOT).replace('\\', '/'),
                          'distance_shown_m': shown,
                          'branches': [{'carriageway': e[3], 'side': 'left' if e[0] > 0.2 else ('right' if e[0] < -0.2 else 'ahead'),
                                        'text_ja': e[1][0] if e[1] else [], 'text_en': e[1][1] if e[1] else []} for e in entries]})
    if steel_mat is not None and steel_mesh.pos:
        glb.mesh('gantries', steel_mesh.pos, steel_mesh.nrm, steel_mesh.uv, steel_mesh.idx, steel_mat)
    glb_path = os.path.join(out_glb_dir, 'signs.glb')
    glb.write(glb_path)
    report = {'bbox': [x0, z0, x1, z1], 'signs': signs, 'gaps': gaps, 'glb': os.path.relpath(glb_path, ROOT).replace('\\', '/'),
              'fonts': ['Noto Sans JP Bold/Medium (SIL OFL 1.1)'], 'substrate': 'textures/shuto/derived/sign_panel_blanks_highway_green.png'}
    out_json = os.path.join(ROOT, 'data', 'shuto', f'signs_{args.name}.json')
    with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    print(f'{len(signs)} sign panels, {len(gaps)} branches without a verified name -> {out_json}')
    for gp in gaps:
        print('  gap:', gp)


if __name__ == '__main__':
    main()
