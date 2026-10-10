"""Global height refinement for the Shuto road dataset (run after build_network.py).

build_network.py derives heights from OSM layer/bridge/tunnel tags and enforces grade limits per
carriageway. This stage solves all centreline heights together as one sparse least-squares
problem so the constraints that couple carriageways hold at the same time:

  * shared nodes: every carriageway end at a node uses the node's height variable;
  * smoothness: second differences along each carriageway and across node continuations
    (vertical-curve abruptness);
  * merge coupling: same-direction carriageways that overlap in plan (gores, tapers, parallel
    lanes before a merge) are pulled to the same height;
  * soft targets: the original heights (weak), at-grade and boundary points at street level and
    long-span bridge pins (strong);
  * inequalities, by iterative penalties: maximum grade (6 % main, 8 % ramps), deck-to-deck
    clearance >= 7 m where carriageways cross in plan at different levels, elevated decks
    >= ground + 5.5 m away from structure transitions.

Writes data/shuto/network.json and data/shuto/nox_road_network.json in place (the original is
kept as network.pre_refine.json) and a report data/shuto/refine_report.json.
Usage: python -I tools/shuto/refine_heights.py [--bbox xmin,zmin,xmax,zmax  (report only)]
"""
import argparse
import json
import math
import os
import shutil
import sys

import numpy as np
from scipy.sparse import csr_matrix
from scipy.sparse.linalg import lsqr

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DATA = os.path.join(ROOT, 'data', 'shuto')

W_TARGET = 0.05       # original heights (weak prior)
W_GROUND = 1.0        # at-grade points and boundary nodes -> street level
W_PIN = 4.0           # long-span bridge decks (y >= 40 m in the source data)
W_SMOOTH = 6.0        # per (m / (10 m)^2) of second difference, i.e. abruptness
W_COUPLE = 3.0        # same-direction overlap
W_INEQ = 8.0          # inequality penalty (grows per iteration)
CLEARANCE = 7.0       # deck-to-deck at crossings
ELEV_MIN = 5.5        # elevated deck above ground
MAX_GRADE = {'main': 0.06}
DEFAULT_GRADE = 0.08


def half_widths(c):
    w = c['lanes'] * c['lane_width'] * 0.5
    return w + c.get('shoulder_left', 0.75), w + c.get('shoulder_right', 0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bbox', default='-2600,1500,0,6000')
    ap.add_argument('--iterations', type=int, default=16)
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    path = os.path.join(DATA, 'network.json')
    net = json.load(open(path, encoding='utf-8'))
    gy = float(net.get('ground_y', 3.0))
    cws = net['carriageways']
    nodes = {n['id']: n for n in net['nodes']}

    # ---- Variables: one per node, one per interior centreline point.
    var_of_node = {nid: i for i, nid in enumerate(nodes)}
    nvar = len(var_of_node)
    pts, var, cw_index, s_along = [], [], [], []
    per_cw = []
    for ci, c in enumerate(cws):
        P = c['points']
        idx = []
        s = 0.0
        for k, p in enumerate(P):
            if k > 0:
                s += math.hypot(P[k][0] - P[k - 1][0], P[k][2] - P[k - 1][2])
            if k == 0:
                v = var_of_node[c['from']]
            elif k == len(P) - 1:
                v = var_of_node[c['to']]
            else:
                v = nvar
                nvar += 1
            idx.append(len(pts))
            pts.append(p)
            var.append(v)
            cw_index.append(ci)
            s_along.append(s)
        per_cw.append(idx)
    y0 = np.zeros(nvar)
    cnt = np.zeros(nvar)
    for k, p in enumerate(pts):
        y0[var[k]] += p[1]
        cnt[var[k]] += 1
    y0 /= np.maximum(cnt, 1)
    print(f'{len(cws)} carriageways, {len(pts)} points, {nvar} height variables')

    def structure_at(c, s):
        for r in c.get('structure', []):
            if r['s0'] - 1e-6 <= s <= r['s1'] + 1e-6:
                return r['type'], min(s - r['s0'], r['s1'] - s)
        return 'at_grade', 0.0

    rows, cols, vals, rhs = [], [], [], []

    def add(terms, b, w):
        r = len(rhs)
        for v, a in terms:
            rows.append(r)
            cols.append(v)
            vals.append(a * w)
        rhs.append(b * w)

    # ---- Soft targets.
    boundary_vars = {var_of_node[n['id']] for n in net['nodes'] if n.get('kind') == 'boundary'}
    for v in range(nvar):
        add([(v, 1.0)], y0[v], W_PIN if y0[v] >= 40.0 else W_TARGET)
    for v in boundary_vars:
        add([(v, 1.0)], y0[v], W_GROUND)
    for ci, c in enumerate(cws):
        for k in per_cw[ci]:
            st, _ = structure_at(c, s_along[k])
            if st == 'at_grade':
                add([(var[k], 1.0)], gy, W_GROUND * 0.3)

    # ---- Smoothness along carriageways (second difference normalised to 10 m spacing).
    def second_diff(a, b, c_, sa, sb, sc):
        h1, h2 = max(sb - sa, 0.5), max(sc - sb, 0.5)
        # Curvature of the profile: 2 * ((c-b)/h2 - (b-a)/h1) / (h1+h2), scaled to per 10 m^2.
        k = 2.0 / (h1 + h2) * 100.0
        return [(a, k / h1), (b, -k / h1 - k / h2), (c_, k / h2)]
    for ci, c in enumerate(cws):
        idx = per_cw[ci]
        for j in range(1, len(idx) - 1):
            a, b, d = idx[j - 1], idx[j], idx[j + 1]
            add(second_diff(var[a], var[b], var[d], s_along[a], s_along[b], s_along[d]), 0.0, W_SMOOTH)
    # Across nodes: every (incoming, outgoing) pair of the same route or the only continuation.
    incoming, outgoing = {}, {}
    for ci, c in enumerate(cws):
        outgoing.setdefault(c['from'], []).append(ci)
        incoming.setdefault(c['to'], []).append(ci)
    for nid in nodes:
        for a in incoming.get(nid, []):
            for b in outgoing.get(nid, []):
                if cws[a]['route'] != cws[b]['route'] and len(outgoing.get(nid, [])) > 1:
                    continue
                ia, ib = per_cw[a], per_cw[b]
                if len(ia) < 2 or len(ib) < 2:
                    continue
                pa, pn, pb = ia[-2], ia[-1], ib[1]
                sa = -(s_along[ia[-1]] - s_along[ia[-2]])
                add(second_diff(var[pa], var[pn], var[pb], sa, 0.0, s_along[ib[1]]), 0.0, W_SMOOTH)

    # ---- Spatial hash of points for overlap / crossing pairs.
    cell = 20.0
    grid = {}
    for k, p in enumerate(pts):
        grid.setdefault((math.floor(p[0] / cell), math.floor(p[2] / cell)), []).append(k)

    def tangent(k):
        ci = cw_index[k]
        idx = per_cw[ci]
        j = idx.index(k) if len(idx) < 64 else None
        if j is None:
            j = k - idx[0]
        a = pts[idx[max(j - 1, 0)]]
        b = pts[idx[min(j + 1, len(idx) - 1)]]
        dx, dz = b[0] - a[0], b[2] - a[2]
        L = math.hypot(dx, dz) or 1.0
        return dx / L, dz / L

    couple_pairs, cross_pairs = [], []
    seen = set()
    for k, p in enumerate(pts):
        ck = cw_index[k]
        hk = sum(half_widths(cws[ck])) * 0.5
        tk = tangent(k)
        gx, gz = math.floor(p[0] / cell), math.floor(p[2] / cell)
        best = {}
        for ix in (gx - 1, gx, gx + 1):
            for iz in (gz - 1, gz, gz + 1):
                for m in grid.get((ix, iz), ()):
                    cm = cw_index[m]
                    if cm == ck or var[m] == var[k]:
                        continue
                    d = math.hypot(pts[m][0] - p[0], pts[m][2] - p[2])
                    if d < best.get(cm, (1e9, -1))[0]:
                        best[cm] = (d, m)
        for cm, (d, m) in best.items():
            hm = sum(half_widths(cws[cm])) * 0.5
            if d > hk + hm:
                continue
            key = (min(var[k], var[m]), max(var[k], var[m]))
            if key in seen:
                continue
            seen.add(key)
            tm = tangent(m)
            dot = tk[0] * tm[0] + tk[1] * tm[1]
            dy = abs(y0[var[k]] - y0[var[m]])
            if dot > 0.7 and dy < 5.5:
                couple_pairs.append((var[k], var[m]))
            elif dot < 0.7 or dy >= 5.5:
                cross_pairs.append((var[k], var[m], dot))
    for a, b in couple_pairs:
        add([(a, 1.0), (b, -1.0)], 0.0, W_COUPLE)
    print(f'{len(couple_pairs)} coupled overlap pairs, {len(cross_pairs)} crossing/stacked pairs')

    base_rows = (list(rows), list(cols), list(vals), list(rhs))

    # Grade pairs and elevated minimums.
    grade_pairs = []
    elevated = []
    for ci, c in enumerate(cws):
        g = MAX_GRADE.get(c['kind'], DEFAULT_GRADE)
        idx = per_cw[ci]
        for j in range(len(idx) - 1):
            a, b = idx[j], idx[j + 1]
            ds = s_along[b] - s_along[a]
            if ds > 1e-3:
                grade_pairs.append((var[a], var[b], ds, g))
        for k in idx:
            st, inside = structure_at(c, s_along[k])
            if st in ('elevated', 'bridge') and inside > 60.0:
                elevated.append(var[k])

    y = y0.copy()
    w_ineq = W_INEQ
    active_clear, active_grade, active_elev = {}, {}, set()
    for it in range(args.iterations + 1):
        R, C, V, B = (list(x) for x in base_rows)
        rows, cols, vals, rhs = R, C, V, B
        # Clearance: the pair keeps its current order; push apart when closer than CLEARANCE.
        n_clear = 0
        for a, b, _ in cross_pairs:
            up, lo = (a, b) if y[a] >= y[b] else (b, a)
            if y0[up] - y0[lo] < 1.5 and abs(y[up] - y[lo]) < 1.5:
                continue  # same level crossing (at-grade junction / data merge), not a stack
            if y[up] - y[lo] < CLEARANCE or (up, lo) in active_clear:
                active_clear[(up, lo)] = True
                add([(up, 1.0), (lo, -1.0)], CLEARANCE, w_ineq)
                n_clear += 1
        n_grade = 0
        for a, b, ds, g in grade_pairs:
            d = y[b] - y[a]
            if abs(d) > g * ds or (a, b) in active_grade:
                active_grade[(a, b)] = True
                add([(b, 1.0), (a, -1.0)], math.copysign(min(abs(d), g * ds), d), w_ineq)
                n_grade += 1
        for a, b in couple_pairs:
            if abs(y[a] - y[b]) > 0.2:
                add([(a, 1.0), (b, -1.0)], 0.0, w_ineq)
        for v in elevated:
            if y[v] < gy + ELEV_MIN or v in active_elev:
                active_elev.add(v)
                add([(v, 1.0)], max(y[v], gy + ELEV_MIN), w_ineq * 0.5)
        A = csr_matrix((vals, (rows, cols)), shape=(len(rhs), nvar))
        sol = lsqr(A, np.asarray(rhs), x0=y, atol=1e-9, btol=1e-9, iter_lim=4000)
        y = sol[0]
        viol_g = sum(1 for a, b, ds, g in grade_pairs if abs(y[b] - y[a]) > g * ds * 1.02 + 0.02)
        viol_c = sum(1 for a, b, _ in cross_pairs
                     if not (y0[max(a, b, key=lambda v: y[v])] - y0[min(a, b, key=lambda v: y[v])] < 1.5 and abs(y[a] - y[b]) < 1.5)
                     and abs(y[a] - y[b]) < CLEARANCE - 0.05)
        viol_m = sum(1 for a, b in couple_pairs if abs(y[a] - y[b]) > 0.25)
        print(f'iter {it}: active clear={n_clear} grade={n_grade} elev={len(active_elev)}  ->  grade viol={viol_g} '
              f'clearance viol={viol_c} coupling>0.25m={viol_m}')
        w_ineq *= 1.6

    # ---- Hard pass. The least-squares solution trades constraints against each other; driving
    # needs grades and merge coupling exactly. Alternate (a) merge coupling as a projection
    # (overlapping same-direction samples share their mean height), (b) node relaxation so every
    # carriageway's ends are compatible with its grade limit, (c) forward/backward grade sweeps
    # along each carriageway. Clearance conflicts that remain are reported, not hidden.
    def hard_grade_pass():
        for _ in range(200):
            moved = 0.0
            for ci, c in enumerate(cws):
                idx = per_cw[ci]
                g = MAX_GRADE.get(c['kind'], DEFAULT_GRADE) * 0.98
                L = s_along[idx[-1]]
                va, vb = var[idx[0]], var[idx[-1]]
                excess = abs(y[vb] - y[va]) - g * L
                if excess > 1e-3:
                    sgn = 1.0 if y[vb] > y[va] else -1.0
                    y[va] += sgn * excess * 0.5
                    y[vb] -= sgn * excess * 0.5
                    moved += excess
            if moved < 1e-3:
                break
        for ci, c in enumerate(cws):
            idx = per_cw[ci]
            if len(idx) < 3:
                continue
            g = MAX_GRADE.get(c['kind'], DEFAULT_GRADE) * 0.995
            for _ in range(4):
                for j in range(1, len(idx) - 1):
                    ds = s_along[idx[j]] - s_along[idx[j - 1]]
                    pv, v = var[idx[j - 1]], var[idx[j]]
                    y[v] = min(max(y[v], y[pv] - g * ds), y[pv] + g * ds)
                for j in range(len(idx) - 2, 0, -1):
                    ds = s_along[idx[j + 1]] - s_along[idx[j]]
                    nv, v = var[idx[j + 1]], var[idx[j]]
                    y[v] = min(max(y[v], y[nv] - g * ds), y[nv] + g * ds)

    def violations():
        vg = sum(1 for a, b, ds, g in grade_pairs if abs(y[b] - y[a]) > g * ds * 1.02 + 0.02)
        vc = sum(1 for a, b, _ in cross_pairs
                 if not (y0[max(a, b, key=lambda v: y[v])] - y0[min(a, b, key=lambda v: y[v])] < 1.5 and abs(y[a] - y[b]) < 1.5)
                 and abs(y[a] - y[b]) < CLEARANCE - 0.05)
        vm = sum(1 for a, b in couple_pairs if abs(y[a] - y[b]) > 0.25)
        return vg, vc, vm

    for outer in range(16):
        if outer > 0:
            for a_, b_ in couple_pairs:
                m = 0.5 * (y[a_] + y[b_])
                y[a_] = m
                y[b_] = m
        hard_grade_pass()
        viol_g, viol_c, viol_m = violations()
        print(f'hard pass {outer}: grade viol={viol_g} clearance viol={viol_c} coupling>0.25m={viol_m}')

    # ---- Write back.
    shutil.copyfile(path, os.path.join(DATA, 'network.pre_refine.json'))
    for k, p in enumerate(pts):
        p[1] = round(float(y[var[k]]), 3)
    for nid, n in nodes.items():
        n['pos'][1] = round(float(y[var_of_node[nid]]), 3)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(net, f, ensure_ascii=False, separators=(',', ':'))
    nox_path = os.path.join(DATA, 'nox_road_network.json')
    nox = json.load(open(nox_path, encoding='utf-8'))
    point_y = {}
    for ci, c in enumerate(cws):
        for j, k in enumerate(per_cw[ci]):
            point_y[f"{c['id']}.p{j:04d}"] = pts[k][1]
    changed = 0
    for n in nox['nodes']:
        if n['id'] in nodes:
            n['pos'][1] = nodes[n['id']]['pos'][1]
            changed += 1
        elif n['id'] in point_y:
            n['pos'][1] = point_y[n['id']]
            changed += 1
    with open(nox_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(nox, f, ensure_ascii=False, separators=(',', ':'))

    # ---- Report.
    report = {
        'variables': nvar, 'coupled_pairs': len(couple_pairs), 'crossing_pairs': len(cross_pairs),
        'grade_violations': int(viol_g), 'clearance_violations': int(viol_c), 'coupling_over_0_25m': int(viol_m),
        'max_change_m': round(float(np.max(np.abs(y - y0))), 3), 'nox_nodes_updated': changed,
    }
    with open(os.path.join(DATA, 'refine_report.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(report, f, indent=1)
    print(json.dumps(report))



if __name__ == '__main__':
    main()
