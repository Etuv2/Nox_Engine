"""Scripted route traversal for a Shuto section: generate covering events, drive them, report.

  python -I tools/shuto/traversal.py make --section rep_section
      Builds traversal events that together drive every carriageway inside the section bbox
      (both directions, all junction connectors and ramps). Each event chains carriageways along
      the directed graph and puts a checkpoint on every carriageway it uses, so the routed path
      cannot shortcut through another connector. Writes data/shuto/traversal_<section>.json and
      scenes/_shuto_traversal_<section>.json (the section scene with these events).

  python -I tools/shuto/traversal.py run --section rep_section [--only 3,5]
      Runs NOX once per event (NOX_DRIVE_AUTOPILOT=1, NOX_AUTOPILOT_EVENT=i, NOX_EXIT_ON_FINISH=1)
      and records checkpoints, finish, stuck/fall incidents and recoveries per event into
      data/shuto/traversal_<section>_results.json (+ logs/traversal/<i>.log).
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.dirname(__file__))
from make_activities import SECTIONS, plan_len, walk  # noqa: E402

EXE = os.environ.get('NOX_EXE') or os.path.join(ROOT, 'build', 'vs2022-x64', 'Release', 'Nox_Engine.exe')
MAX_EVENT_M = 3500.0
EDGE_MARGIN = 60.0


def load(section):
    net = json.load(open(os.path.join(ROOT, 'data', 'shuto', 'network.json'), encoding='utf-8'))
    x0, z0, x1, z1 = SECTIONS[section]['bbox']
    box = (x0 + EDGE_MARGIN, z0 + EDGE_MARGIN, x1 - EDGE_MARGIN, z1 - EDGE_MARGIN)
    return net, box


def inside(p, box):
    return box[0] <= p[0] <= box[2] and box[1] <= p[2] <= box[3]


def make(section):
    net, box = load(section)
    cw = {c['id']: c for c in net['carriageways']}
    succ, pred = {}, {}
    for c in net['carriageways']:
        succ.setdefault(c['from'], []).append(c['id'])
        pred.setdefault(c['to'], []).append(c['id'])
    # Carriageways to cover: drivable (connected) ones with a meaningful part inside the box.
    todo = sorted(c['id'] for c in net['carriageways']
                  if c.get('route') != 'PA' and sum(1 for p in c['points'] if inside(p, box)) >= 3)
    uncovered = set(todo)
    events = []
    while uncovered:
        seed = min(uncovered, key=lambda i: (cw[i]['kind'] != 'main', i))
        path = [seed]
        # Back up to a predecessor so the event starts with run-up (prefer uncovered, in box).
        back = 0.0
        while back < 400.0:
            ps = [p for p in pred.get(cw[path[0]]['from'], []) if any(inside(q, box) for q in cw[p]['points'])]
            if not ps:
                break
            p = sorted(ps, key=lambda i: (i not in uncovered, cw[i]['kind'] != 'main'))[0]
            if p in path:
                break
            path.insert(0, p)
            back += plan_len(cw[p]['points'])
        total = sum(plan_len(cw[i]['points']) for i in path)
        while total < MAX_EVENT_M:
            ns = [n for n in succ.get(cw[path[-1]]['to'], []) if any(inside(q, box) for q in cw[n]['points'])]
            if not ns:
                break
            n = sorted(ns, key=lambda i: (i not in uncovered, cw[i]['kind'] != 'main'))[0]
            if n in path:
                break
            path.append(n)
            total += plan_len(cw[n]['points'])
            if not any(inside(q, box) for q in cw[n]['points'][-3:]):
                break  # leaves the section
        # Polyline clipped to the box (keep the inside span).
        pts = []
        for i, cid in enumerate(path):
            P = cw[cid]['points']
            pts += P if i == 0 else P[1:]
        ins = [k for k, p in enumerate(pts) if inside(p, box)]
        if len(ins) < 3:
            uncovered -= set(path)
            continue
        pts = pts[ins[0]:ins[-1] + 1]
        L = plan_len(pts)
        if L < 120.0:
            uncovered -= set(path)
            continue
        start, start_h = walk(pts, 25.0)
        # One checkpoint per carriageway (its midpoint inside the box) plus spacing <= 400 m.
        cps = []
        for cid in path:
            P = [p for p in cw[cid]['points'] if inside(p, box)]
            if len(P) >= 2 and plan_len(P) > 30.0:
                q, _ = walk(P, plan_len(P) * 0.5)
                cps.append(q)
        end, _ = walk(pts, L - 5.0)
        cps.append(end)

        def s_of(q):
            best, acc, sbest = 1e18, 0.0, 0.0
            for a, b in zip(pts, pts[1:]):
                d = math.hypot(b[0] - a[0], b[2] - a[2])
                for t in (0.0, 0.5, 1.0):
                    x, z = a[0] + (b[0] - a[0]) * t, a[2] + (b[2] - a[2]) * t
                    e = (x - q[0]) ** 2 + (z - q[2]) ** 2 + (a[1] - q[1]) ** 2
                    if e < best:
                        best, sbest = e, acc + d * t
                acc += d
            return sbest
        cps = sorted({round(s_of(q), 1): q for q in cps}.items())
        merged = []
        for s, q in cps:
            if s < 60.0:
                continue
            if merged and s - merged[-1][0] < 50.0:
                continue
            if merged and s - merged[-1][0] > 400.0:
                k = merged[-1][0] + 300.0
                while k < s - 100.0:
                    merged.append((k, walk(pts, k)[0]))
                    k += 300.0
            merged.append((s, q))
        if not merged:
            uncovered -= set(path)
            continue
        idx = len(events)
        events.append({'id': f'traverse_{idx:03d}', 'type': 'SPRINT RACE', 'name': f'Traverse {idx:03d}', 'difficulty': 'Test',
                       'start': [round(v, 2) for v in start], 'start_heading_deg': round(start_h, 2), 'trigger_radius': 10.0,
                       'countdown': 1.0, 'checkpoints': [{'pos': [round(v, 2) for v in q], 'width': 18.0} for _, q in merged],
                       'route_carriageways': path, 'length_m': round(L, 1)})
        uncovered -= set(path)
    # Explicit validation routes (make_activities SECTIONS 'validation_routes'): driven end to end.
    lots = {}
    off_path = os.path.join(ROOT, 'models', 'shuto', section, 'deck_offsets.json')
    if os.path.exists(off_path):
        lots = {l['carriageway']: l for l in json.load(open(off_path, encoding='utf-8')).get('lots', [])}
    for vr in SECTIONS[section].get('validation_routes', []):
        path = [c for c in vr['path'] if c in cw]
        pts = []
        for i, cid in enumerate(path):
            P = cw[cid]['points']
            pts += P if i == 0 else P[1:]
        ins = [k for k, p in enumerate(pts) if inside(p, box)]
        pts = pts[ins[0]:ins[-1] + 1]
        L = plan_len(pts)
        start, start_h = walk(pts, 25.0)
        cps = []
        acc = 0.0
        for cid in path:
            P = [p for p in cw[cid]['points'] if inside(p, box)]
            if len(P) >= 2 and plan_len(P) > 30.0:
                cps.append(walk(P, plan_len(P) * 0.5)[0])
        for cid in path:
            if 'lot' in vr.get('extra_checkpoints', []) and cid in lots:
                cps.append(lots[cid]['pos'])
        cps.append(walk(pts, L - 5.0)[0])

        def s_of2(q):
            best, acc2, sbest = 1e18, 0.0, 0.0
            for a, b in zip(pts, pts[1:]):
                d = math.hypot(b[0] - a[0], b[2] - a[2])
                for t in (0.0, 0.25, 0.5, 0.75, 1.0):
                    x, z = a[0] + (b[0] - a[0]) * t, a[2] + (b[2] - a[2]) * t
                    e = (x - q[0]) ** 2 + (z - q[2]) ** 2
                    if e < best:
                        best, sbest = e, acc2 + d * t
                acc2 += d
            return sbest
        cps = sorted(((s_of2(q), q) for q in cps), key=lambda v: v[0])
        idx = len(events)
        # the lot checkpoint is on the bays, 5 m beside the PA lane centre: inside the 18 m gate
        events.append({'id': f"validate_{vr['id']}", 'type': 'SPRINT RACE', 'name': vr['name'], 'difficulty': 'Test',
                       'start': [round(v, 2) for v in start], 'start_heading_deg': round(start_h, 2), 'trigger_radius': 10.0,
                       'countdown': 1.0,
                       'checkpoints': [{'pos': [round(v, 2) for v in q], 'width': 18.0}
                                       for s, q in cps if s > 40.0],
                       'route_carriageways': path, 'length_m': round(L, 1)})
        print(f"validation route {vr['id']}: event {idx}, {L:.0f} m, {len(events[-1]['checkpoints'])} checkpoints")
    covered = sorted({c for e in events for c in e['route_carriageways']} & set(todo))
    doc = {'section': section, 'box_shrunk_by_m': EDGE_MARGIN, 'required': todo, 'covered': covered,
           'missing': sorted(set(todo) - set(covered)), 'events': events}
    out = os.path.join(ROOT, 'data', 'shuto', f'traversal_{section}.json')
    json.dump(doc, open(out, 'w', encoding='utf-8', newline='\n'), indent=1)
    scene = json.load(open(os.path.join(ROOT, 'scenes', f'shuto_{section}.json'), encoding='utf-8'))
    scene['game']['events'] = events
    json.dump(scene, open(os.path.join(ROOT, 'scenes', f'_shuto_traversal_{section}.json'), 'w', encoding='utf-8', newline='\n'), indent=1)
    print(f'{len(events)} events cover {len(covered)}/{len(todo)} carriageways '
          f'({sum(e["length_m"] for e in events) / 1000:.1f} km) -> {out}')


def roads_only_scene(section):
    """Private copy of the traversal scene without scenery meshes or scenery collision (road geometry
    and signs only), so road validation is not affected by scenery rebuilds."""
    src = os.path.join(ROOT, 'scenes', f'_shuto_traversal_{section}.json')
    scene = json.load(open(src, encoding='utf-8'))
    scene['nodes'] = [n for n in scene.get('nodes', []) if '/scenery/' not in n.get('path', '') and not n.get('name', '').endswith('_scenery')]
    game = scene.get('game', {})
    game['collision'] = dict(game.get('collision', {}), files=[f for f in game.get('collision', {}).get('files', []) if 'scenery' not in f])
    out = os.path.join(ROOT, 'scenes', f'_shuto_traversal_roads_{section}.json')
    json.dump(scene, open(out, 'w', encoding='utf-8', newline=chr(10)), indent=1)
    return os.path.relpath(out, ROOT).replace(chr(92), '/')


def run(section, only, roads=False):
    doc = json.load(open(os.path.join(ROOT, 'data', 'shuto', f'traversal_{section}.json'), encoding='utf-8'))
    scene = roads_only_scene(section) if roads else os.path.join('scenes', f'_shuto_traversal_{section}.json')
    os.makedirs(os.path.join(ROOT, 'logs', 'traversal'), exist_ok=True)
    results = []
    for i, ev in enumerate(doc['events']):
        if only and i not in only:
            continue
        env = dict(os.environ, NOX_DRIVE_AUTOPILOT='1', NOX_AUTOPILOT_EVENT=str(i), NOX_EXIT_ON_FINISH='1', NOX_AUTOPILOT_MAXKMH='100',
                   NOX_EXIT_AFTER_FRAMES=str(int(6000 + ev['length_m'] * 15)))
        log_path = os.path.join(ROOT, 'logs', 'traversal', f'{section}_{i:03d}.log')
        with open(log_path, 'w', encoding='utf-8', errors='ignore') as lf:
            try:
                subprocess.run([EXE, '--game', '--scene', scene, '--window', 'windowed', '--width', '640', '--height', '360'],
                               cwd=ROOT, env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=900)
                code = 'ok'
            except subprocess.TimeoutExpired:
                code = 'timeout'
        text = open(log_path, encoding='utf-8', errors='ignore').read()
        cps = len(re.findall(r'\[Race\] Checkpoint', text))
        r = {'event': i, 'id': ev['id'], 'length_m': ev['length_m'], 'carriageways': ev['route_carriageways'],
             'checkpoints_required': len(ev['checkpoints']), 'checkpoints_hit': cps,
             'finished': '[Race] Finished' in text, 'time_s': None, 'run': code,
             'stuck': re.findall(r'\[Traversal\] STUCK at \(([^)]*)\)', text),
             'fell': re.findall(r'\[Traversal\] FELL from (.*) at \(([^)]*)\)', text),
             'no_ground_recoveries': len(re.findall(r'No ground for 3 s', text)),
             'missed': len(re.findall(r'Missed checkpoint', text)),
             'crash': 'CrashHandler' in text or 'Unhandled exception' in text}
        m = re.search(r'\[Race\] Finished in ([\d.]+) s', text)
        if m:
            r['time_s'] = float(m.group(1))
        results.append(r)
        flag = 'PASS' if r['finished'] and not r['stuck'] and not r['fell'] and not r['no_ground_recoveries'] else 'FAIL'
        print(f"{flag} event {i:3d} {ev['length_m']:7.0f} m  cps {cps}/{len(ev['checkpoints'])}  stuck {len(r['stuck'])} "
              f"fell {len(r['fell'])} recov {r['no_ground_recoveries']} missed {r['missed']} {code}", flush=True)
    out = os.path.join(ROOT, 'data', 'shuto', f'traversal_{section}_roads_results.json' if roads else f'traversal_{section}_results.json')
    prev = []
    if only and os.path.exists(out):
        prev = [r for r in json.load(open(out, encoding='utf-8'))['results'] if r['event'] not in only]
    allr = sorted(prev + results, key=lambda r: r['event'])
    passed = [r for r in allr if r['finished'] and not r['stuck'] and not r['fell'] and not r['no_ground_recoveries']]
    json.dump({'section': section, 'events': len(doc['events']), 'passed': len(passed), 'results': allr},
              open(out, 'w', encoding='utf-8', newline='\n'), indent=1)
    print(f'{len(passed)}/{len(allr)} events passed -> {out}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['make', 'run'])
    ap.add_argument('--section', default='rep_section')
    ap.add_argument('--only', default='')
    ap.add_argument('--roads-only', action='store_true', help='run on the roads-only scene (no scenery)')
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    if a.cmd == 'make':
        make(a.section)
    else:
        run(a.section, {int(v) for v in a.only.split(',') if v}, roads=a.roads_only)


if __name__ == '__main__':
    main()
