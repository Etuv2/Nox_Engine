"""Generate activity data (events and places) for a Shuto section from the road graph.

Events are defined by a list of carriageway waypoints; the script finds the directed shortest
path through them on data/shuto/network.json, places checkpoints along it (3D positions, so gates
on stacked decks only trigger on their own level) and a start / spawn upstream of the first gate.
Places are parking areas (rest, meeting and photo spots) from the network facilities.

Output: data/shuto/activities_<section>.json (consumed by build_scene.py).
Usage: python -I tools/shuto/make_activities.py --section rep_section
"""
import argparse
import heapq
import json
import math
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

# Section definitions: events as waypoint carriageways (route is solved between them).
SECTIONS = {
    'rep_section': {
        'bbox': [-2600, 1500, 0, 6000],
        'spawn_back_m': 150.0,
        'events': [
            {'id': 'shuto_ginza_rainbow', 'type': 'SPRINT RACE', 'name': 'Ginza to Rainbow Bridge', 'difficulty': 'Medium',
             # Waypoints as world positions [x, y, z] (resolved to the nearest main carriageway on
             # that level, so events survive carriageway re-numbering when the data is rebuilt).
             'via_xyz': [[-650.0, -3.0, 1985.0, -45.0], [-900.0, 52.0, 5339.0, 69.0]], 'start_offset_m': 40.0,
             'checkpoint_spacing_m': 450.0, 'end_xyz': [-480.0, 45.0, 5480.0]},
        ],
        'places': ['Shibaura'],
        # Validation routes (traversal.py appends them to the covering events): explicit carriageway
        # chains through features that need a dedicated drive.
        'validation_routes': [
            {'id': 'pa_access', 'name': 'Shibaura PA access (bridge, PA lane, parking apron, re-entry)',
             'path': ['11.IN.004', '11.IN.002', '11.IN.001'], 'extra_checkpoints': ['lot']},
            {'id': 'rainbow_outbound', 'name': 'Rainbow Bridge outbound (Shibaura loop to Daiba)',
             'path': ['11.OUT.001', '11.OUT.003']},
        ],
    },
}


def plan_len(points):
    return sum(math.hypot(b[0] - a[0], b[2] - a[2]) for a, b in zip(points, points[1:]))


def walk(points, s):
    acc = 0.0
    for a, b in zip(points, points[1:]):
        d = math.hypot(b[0] - a[0], b[2] - a[2])
        if d > 1e-6 and acc + d >= s:
            t = (s - acc) / d
            return [a[i] + (b[i] - a[i]) * t for i in range(3)], math.degrees(math.atan2(b[0] - a[0], b[2] - a[2]))
        acc += d
    a, b = points[-2], points[-1]
    return list(b), math.degrees(math.atan2(b[0] - a[0], b[2] - a[2]))


def nearest_main(net, p, max_dy=6.0):
    """Main carriageway whose centreline passes closest to world point p = [x, y, z] (same level)."""
    best = None
    for c in net['carriageways']:
        if c['kind'] != 'main':
            continue
        P = c['points']
        for k, q in enumerate(P):
            if abs(q[1] - p[1]) > max_dy:
                continue
            if len(p) > 3:
                # Travel heading hint (deg, 0 = +Z, toward +X): reject the opposite carriageway.
                a_, b_ = P[max(k - 1, 0)], P[min(k + 1, len(P) - 1)]
                h = math.degrees(math.atan2(b_[0] - a_[0], b_[2] - a_[2]))
                if abs((h - p[3] + 180.0) % 360.0 - 180.0) > 60.0:
                    continue
            d = (q[0] - p[0]) ** 2 + (q[2] - p[2]) ** 2
            if best is None or d < best[0]:
                best = (d, c['id'])
    if best is None:
        raise SystemExit(f'no main carriageway near {p}')
    return best[1]


def shortest(cw, out, src, dst):
    """Carriageway-level Dijkstra from the end of src to the start of dst (both included)."""
    dist = {src: 0.0}
    prev = {}
    pq = [(0.0, src)]
    while pq:
        d, c = heapq.heappop(pq)
        if c == dst:
            break
        if d > dist.get(c, 1e18):
            continue
        for n in out.get(cw[c]['to'], []):
            nd = d + plan_len(cw[n]['points'])
            if nd < dist.get(n, 1e18):
                dist[n] = nd
                prev[n] = c
                heapq.heappush(pq, (nd, n))
    if dst not in dist:
        raise SystemExit(f'no route {src} -> {dst}')
    path = [dst]
    while path[-1] != src:
        path.append(prev[path[-1]])
    return path[::-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--section', default='rep_section')
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    cfg = SECTIONS[args.section]
    net = json.load(open(os.path.join(ROOT, 'data', 'shuto', 'network.json'), encoding='utf-8'))
    cw = {c['id']: c for c in net['carriageways']}
    out = {}
    for c in net['carriageways']:
        out.setdefault(c['from'], []).append(c['id'])

    events, spawn = [], None
    for ev in cfg['events']:
        via = [nearest_main(net, p) for p in ev['via_xyz']]
        if 'end_xyz' in ev:
            last = cw[via[-1]]
            ex, ey, ez = ev['end_xyz']
            k = min(range(len(last['points'])), key=lambda i: (last['points'][i][0] - ex) ** 2 + (last['points'][i][2] - ez) ** 2)
            ev['end_s'] = {via[-1]: plan_len(last['points'][:k + 1])}
        path = [via[0]]
        for a, b in zip(via, via[1:]):
            path += shortest(cw, out, a, b)[1:]
        pts = []
        for i, cid in enumerate(path):
            p = cw[cid]['points']
            if cid in ev.get('end_s', {}):
                # Truncate the last carriageway at end_s metres.
                s_end = ev['end_s'][cid]
                q, acc = [p[0]], 0.0
                for a, b in zip(p, p[1:]):
                    d = math.hypot(b[0] - a[0], b[2] - a[2])
                    if acc + d > s_end:
                        break
                    q.append(b)
                    acc += d
                p = q
            pts += p if i == 0 else p[1:]
        total = plan_len(pts)
        start, start_h = walk(pts, ev['start_offset_m'])
        cps = []
        s = ev['start_offset_m'] + ev['checkpoint_spacing_m']
        while s < total - ev['checkpoint_spacing_m'] * 0.5:
            p, _ = walk(pts, s)
            cps.append({'pos': [round(v, 2) for v in p], 'width': 16.0})
            s += ev['checkpoint_spacing_m']
        p, _ = walk(pts, total - 1.0)
        cps.append({'pos': [round(v, 2) for v in p], 'width': 16.0})
        events.append({'id': ev['id'], 'type': ev['type'], 'name': ev['name'], 'difficulty': ev['difficulty'],
                       'start': [round(v, 2) for v in start], 'start_heading_deg': round(start_h, 2), 'trigger_radius': 10.0,
                       'countdown': 3.0, 'checkpoints': cps, 'route_carriageways': path, 'length_m': round(total, 1)})
        if spawn is None:
            # Spawn upstream on the first carriageway (or its predecessor) so the event marker is ahead.
            first = cw[path[0]]
            back = max(0.0, ev['start_offset_m'] - 30.0)
            sp, sh = walk(first['points'], back)
            spawn = {'position': [round(v, 2) for v in sp], 'heading_deg': round(sh, 2)}
        print(f"event {ev['id']}: {len(path)} carriageways, {total:.0f} m, {len(cps)} checkpoints")

    # Parking aprons generated by build_roads.py (deck_offsets.json "lots"): the place sits on the bays.
    lots = {}
    off_path = os.path.join(ROOT, 'models', 'shuto', args.section, 'deck_offsets.json')
    if os.path.exists(off_path):
        lots = {l['facility']: l for l in json.load(open(off_path, encoding='utf-8')).get('lots', [])}
    places = []
    for f in net.get('facilities', []):
        if f.get('type') != 'pa':
            continue
        name = (f.get('name_en') or '') + ' ' + (f.get('name_ja') or '')
        if not any(k.lower() in name.lower() or k in name for k in cfg['places']) and '芝浦' not in name:
            continue
        x0, z0, x1, z1 = cfg['bbox']
        if not (x0 <= f['pos'][0] <= x1 and z0 <= f['pos'][2] <= z1):
            continue
        pos = lots[f['id']]['pos'] if f['id'] in lots else f['pos']
        places.append({'id': f['id'], 'type': 'PARKING AREA', 'name': f.get('name_en') or f.get('name_ja'), 'name_ja': f.get('name_ja', ''),
                       'pos': [round(v, 2) for v in pos], 'radius': 40.0 if f['id'] not in lots else 30.0,
                       'activities': ['rest', 'meet', 'photo'], 'outline': f.get('outline', [])})
    doc = {'section': args.section, 'source': 'data/shuto/network.json (OSM ODbL)', 'spawn': spawn, 'events': events, 'places': places}
    path = os.path.join(ROOT, 'data', 'shuto', f'activities_{args.section}.json')
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=1)
    print(f'{len(places)} places -> {path}')


if __name__ == '__main__':
    main()
