"""Targeted geometry repairs of the solved Shuto network (run after refine_heights.py).

OSM centrelines of some ramps run on top of the main line they join or leave (Shiodome entrance
and exit: the ramp lane is drawn 1.5-2.5 m from the main centreline for 100-150 m), and the height
solver then has to choose between a stacked crossing (the Shiodome entrance passed 1-9 m above the
main line's lanes: a car on C1 would hit its slab) and a merge. The Shibaura PA lane had the main
line's 5.9 % grade along the whole parking area. These are data defects, repaired here at the data
level so the road graph (autopilot, checkpoints, map) and the generated geometry stay identical.

Operations (REPAIRS, applied in order, idempotent):
  separate  move a carriageway sideways (in the frame of a reference carriageway) so that its deck
            edge is at least `gap` m clear of the reference deck over an s range [s0, s1] (plan m
            along the moved carriageway); the shift is eased in and out over `ease` m.
  profile   replace the height profile over (s0, s1) by a piecewise-linear profile through control
            points [s, y | "keep"] (keep = the current solved height), smoothed over `smooth` m.
            Control points at the carriageway ends also move the shared graph node.
  lot       attach a parking-lot description to a PA facility (build_roads.py widens the PA lane's
            deck on that side into a parking apron with bays, a walkway and a rest building).

Writes data/shuto/network.json and data/shuto/nox_road_network.json in place (or to --out-dir),
and a report data/shuto/repair_report.json. The network records the applied repairs; a second run
on a repaired network is refused (the easing of `separate` is relative to the input geometry).
Usage: python -I tools/shuto/repair_network.py [--src-dir data/shuto] [--out-dir data/shuto]
"""
import argparse
import json
import math
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

REPAIRS = [
    {'op': 'separate', 'id': 'C1.CW.021', 'ref': 'C1.CW.018', 's0': 0.0, 's1': 290.0, 'gap': 0.6, 'ease': 15.0, 'side': 'L',
     'why': 'Shiodome entrance drawn over C1 (crosses the main line 1-9 m above its lanes, then runs on its '
            'left lane): it runs alongside on the left, as an added lane up to its merge taper'},
    {'op': 'profile', 'id': 'C1.CW.021', 's0': 0.0, 's1': 110.0, 'smooth': 20.0,
     'points': [[0.0, 3.05], [110.0, 'keep']],
     'why': 'entrance starts at street level (the solver had lifted it 7 m over the main line it crossed)'},
    {'op': 'separate', 'id': 'C1.CCW.023', 'ref': 'C1.CCW.024', 's0': 70.0, 's1': 440.0, 'gap': 0.4, 'ease': 30.0, 'side': 'L',
     'why': 'Shiodome exit drawn back over the C1 lanes after its diverge: kept alongside until it leaves'},
    {'op': 'profile', 'id': 'C1.CCW.023', 's0': 250.0, 's1': 1e9, 'smooth': 20.0,
     'points': [[250.0, 'keep'], [1e9, 3.05]],
     'why': 'exit ends at street level (it ended 2.7 m above the street)'},
    {'op': 'separate', 'id': '11.IN.002', 'ref': '11.IN.003', 's0': 45.0, 's1': 225.0, 'gap': 2.0, 'ease': 30.0,
     'why': 'Shibaura PA lane: a 2 m median with parapets from the inbound main line (the PA runs at its own grade)'},
    {'op': 'profile', 'id': '11.IN.002', 's0': 40.0, 's1': 228.0, 'smooth': 16.0,
     'points': [[40.0, 'keep'], [82.0, 23.45], [138.0, 22.35], [228.0, 'keep']],
     'why': 'Shibaura PA: 7-8 % entry and exit ramps and a 2 % parking section (was 5.9 % throughout)'},
    {'op': 'lot', 'facility': 'PA.w585884541', 'carriageway': '11.IN.002', 's0': 84.0, 's1': 136.0, 'side': 'L',
     'width': 13.5, 'taper': 12.0,
     'why': 'Shibaura PA parking apron on the PA lane (bays, walkway, rest building)'},
]


def plan_s(points):
    s = [0.0]
    for a, b in zip(points, points[1:]):
        s.append(s[-1] + math.hypot(b[0] - a[0], b[2] - a[2]))
    return s


def frame_at(ref, x, z):
    """Nearest point on ref's polyline: (lateral, unit tangent, plan point, index)."""
    P = ref['points']
    best = None
    for k in range(len(P) - 1):
        a, b = P[k], P[k + 1]
        dx, dz = b[0] - a[0], b[2] - a[2]
        L2 = dx * dx + dz * dz
        if L2 < 1e-12:
            continue
        t = max(0.0, min(1.0, ((x - a[0]) * dx + (z - a[2]) * dz) / L2))
        px, pz = a[0] + dx * t, a[2] + dz * t
        d2 = (x - px) ** 2 + (z - pz) ** 2
        if best is None or d2 < best[0]:
            L = math.sqrt(L2)
            best = (d2, dx / L, dz / L, px, pz, k)
    _, tx, tz, px, pz, k = best
    lx, lz = tz, -tx                      # left of travel (as build_roads.py)
    lat = (x - px) * lx + (z - pz) * lz
    return lat, (tx, tz), (px, pz), k


def half_widths(c):
    w = c['lanes'] * c.get('lane_width', 3.25) * 0.5
    return w + c.get('shoulder_left', 0.75), w + c.get('shoulder_right', 0.5)


def smoothstep(x):
    x = max(0.0, min(1.0, x))
    return x * x * (3.0 - 2.0 * x)


def op_separate(cw, op, log):
    c, ref = cw[op['id']], cw[op['ref']]
    P = c['points']
    S = plan_s(P)
    chl, chr_ = half_widths(c)
    rhl, rhr = half_widths(ref)
    moved = 0
    shifts = []
    for k, p in enumerate(P):
        if k == 0 or k == len(P) - 1:
            shifts.append(0.0)
            continue
        lat, (tx, tz), _, _ = frame_at(ref, p[0], p[2])
        side = 1.0 if lat >= 0.0 else -1.0
        if op.get('side'):
            side = 1.0 if op['side'] == 'L' else -1.0
        need = (rhl + chr_ if side > 0 else rhr + chl) + op['gap']
        s = S[k]
        w = smoothstep((s - op['s0']) / op['ease']) * smoothstep((op['s1'] - s) / op['ease']) \
            if op['s0'] < s < op['s1'] else 0.0
        want = side * need
        d = (want - lat) if (lat * side < need) else 0.0
        shifts.append(d * w)
    # light smoothing of the shift along the carriageway (no kinks between 10 m points)
    sm = shifts[:]
    for k in range(1, len(P) - 1):
        sm[k] = (shifts[k - 1] + 2.0 * shifts[k] + shifts[k + 1]) * 0.25
    for k in range(1, len(P) - 1):
        if abs(sm[k]) < 1e-3:
            continue
        _, (tx, tz), _, _ = frame_at(ref, P[k][0], P[k][2])
        lx, lz = tz, -tx
        P[k][0] = round(P[k][0] + lx * sm[k], 3)
        P[k][2] = round(P[k][2] + lz * sm[k], 3)
        moved += 1
    log.append({'op': 'separate', 'id': op['id'], 'ref': op['ref'], 'points_moved': moved,
                'max_shift_m': round(max((abs(v) for v in sm), default=0.0), 2)})
    return moved > 0


def op_profile(cw, nodes, op, log):
    c = cw[op['id']]
    P = c['points']
    S = plan_s(P)
    L = S[-1]
    ctrl = []
    for s, y in op['points']:
        s = min(max(s, 0.0), L)
        if y == 'keep':
            k = min(range(len(P)), key=lambda i: abs(S[i] - s))
            y = P[k][1]
        ctrl.append((s, float(y)))
    ctrl.sort()

    def lin(s):
        if s <= ctrl[0][0]:
            return ctrl[0][1]
        for (s0, y0), (s1, y1) in zip(ctrl, ctrl[1:]):
            if s <= s1:
                return y0 + (y1 - y0) * (s - s0) / max(s1 - s0, 1e-9)
        return ctrl[-1][1]
    s0, s1 = max(op['s0'], 0.0), min(op['s1'], L)
    half = op.get('smooth', 0.0) * 0.5
    changed = 0
    for k, p in enumerate(P):
        s = S[k]
        if not (s0 - 1e-6 <= s <= s1 + 1e-6):
            continue
        if half > 0.0:
            # moving average of the piecewise-linear profile, window shrunk toward the range ends
            h = min(half, s - s0, s1 - s) if (s0 < s < s1) else 0.0
            n = 9
            y = sum(lin(s - h + 2.0 * h * j / (n - 1)) for j in range(n)) / n if h > 0.0 else lin(s)
        else:
            y = lin(s)
        y = round(y, 3)
        if abs(y - p[1]) > 1e-4:
            p[1] = y
            changed += 1
    for end, k in (('from', 0), ('to', len(P) - 1)):
        nid = c[end]
        if nid in nodes and abs(nodes[nid]['pos'][1] - P[k][1]) > 1e-4:
            nodes[nid]['pos'][1] = P[k][1]
    grades = [abs(P[k + 1][1] - P[k][1]) / max(S[k + 1] - S[k], 1e-6) for k in range(len(P) - 1)
              if s0 - 10 <= S[k] <= s1 + 10]
    log.append({'op': 'profile', 'id': op['id'], 'points_changed': changed,
                'max_grade_pct': round(100.0 * max(grades, default=0.0), 2)})
    return changed > 0


def op_lot(net, op, log):
    for f in net['facilities']:
        if f['id'] == op['facility']:
            f['lot'] = {k: op[k] for k in ('carriageway', 's0', 's1', 'side', 'width', 'taper')}
            if op['carriageway'] not in f['carriageways']:
                f['carriageways'].append(op['carriageway'])
            log.append({'op': 'lot', 'facility': op['facility'], 'carriageway': op['carriageway']})
            return True
    raise SystemExit('facility not found: ' + op['facility'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src-dir', default='data/shuto')
    ap.add_argument('--out-dir', default=None)
    ap.add_argument('--force', action='store_true', help='apply again to an already repaired network')
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    src = os.path.join(ROOT, args.src_dir) if not os.path.isabs(args.src_dir) else args.src_dir
    dst = args.out_dir or src
    dst = os.path.join(ROOT, dst) if not os.path.isabs(dst) else dst
    os.makedirs(dst, exist_ok=True)
    net = json.load(open(os.path.join(src, 'network.json'), encoding='utf-8'))
    nox = json.load(open(os.path.join(src, 'nox_road_network.json'), encoding='utf-8'))
    if net.get('repairs') and not args.force:
        print('network already repaired (%s); run refine_heights.py first or pass --force' % net['repairs']['tool'])
        return 0
    cw = {c['id']: c for c in net['carriageways']}
    nodes = {n['id']: n for n in net['nodes']}
    log = []
    touched = set()
    for op in REPAIRS:
        if op['op'] == 'separate':
            if op_separate(cw, op, log):
                touched.add(op['id'])
        elif op['op'] == 'profile':
            if op_profile(cw, nodes, op, log):
                touched.add(op['id'])
        elif op['op'] == 'lot':
            op_lot(net, op, log)
        log[-1]['why'] = op['why']
    net['repairs'] = {'tool': 'tools/shuto/repair_network.py', 'ops': [o['op'] + ':' + o.get('id', o.get('facility', ''))
                                                                     for o in REPAIRS]}
    # Mirror into the NOX road graph: interior points are '<id>.pNNNN', ends are the shared nodes.
    pos = {}
    for cid in touched:
        P = cw[cid]['points']
        for j in range(1, len(P) - 1):
            pos['%s.p%04d' % (cid, j)] = P[j]
        for end, j in (('from', 0), ('to', len(P) - 1)):
            pos[cw[cid][end]] = nodes[cw[cid][end]]['pos']
    n_nox = 0
    for n in nox['nodes']:
        if n['id'] in pos:
            p = pos[n['id']]
            if any(abs(a - b) > 1e-4 for a, b in zip(n['pos'], p)):
                n['pos'] = [round(v, 3) for v in p]
                n_nox += 1
    # Carriageway end points follow their nodes (a node moved by one profile op).
    for c in net['carriageways']:
        for end, j in (('from', 0), ('to', -1)):
            npos = nodes[c[end]]['pos']
            if abs(c['points'][j][1] - npos[1]) > 1e-4 and c[end] in pos:
                c['points'][j][1] = npos[1]
    with open(os.path.join(dst, 'network.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(net, f, ensure_ascii=False, separators=(',', ':'))
    with open(os.path.join(dst, 'nox_road_network.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(nox, f, ensure_ascii=False, separators=(',', ':'))
    report = {'repairs': log, 'nox_nodes_updated': n_nox}
    with open(os.path.join(dst, 'repair_report.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(report, f, indent=1)
    for r in log:
        print({k: v for k, v in r.items() if k != 'why'})
    print('nox nodes updated: %d -> %s' % (n_nox, dst))


if __name__ == '__main__':
    main()
