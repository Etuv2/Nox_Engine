"""Writes a compact synthetic test network in the Shuto dataset contract.

Run with: python -I tools/shuto/make_test_network.py
Outputs (UTF-8, ASCII punctuation):
  data/shuto/test_network.json            contract format (docs/shuto/network_format.md)
  data/shuto/test_nox_road_network.json   NOX RoadNetwork graph (one node per centreline point)

Layout (NOX frame: +X east, +Y up, +Z south; metres). Ground y = 3.0.
  C1 mainline, elevated at y=15, 3% grades, 200 m radius right-hand curve.
    E carriageway (north side, left of travel) split at the merge and diverge nodes.
    W carriageway (south side) continuous.
  C1 access road + on-ramp from ground (6 percent), merging onto C1.E from the left.
  C1 off-ramp diverging to the left of C1.E, descending to a parking area loop (pa).
  C2 ground road S-bound at x=-360 crossing UNDER the mainline (stacked check), with a
     covered cut-and-cover section (tunnel floor at y=-5) between z=40 and z=580.
Carriageway ids: <route>.<dir>.<seq>. Node ids: N000001...
"""
import json
import math
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT_DIR = os.path.join(ROOT, "data", "shuto")

GROUND_Y = 3.0
DECK_Y = 15.0
MAIN_OFFSET = 7.0  # carriageway centreline distance from the spine (median axis)
ARC_R = 200.0


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def straight(p0, p1, step=1.0):
    length = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
    n = max(1, int(math.ceil(length / step)))
    return [(p0[0] + (p1[0] - p0[0]) * i / n, p0[1] + (p1[1] - p0[1]) * i / n) for i in range(n + 1)]


def arc(cx, cz, r, a0, a1, step=1.0):
    n = max(2, int(math.ceil(abs(a1 - a0) * r / step)))
    out = []
    for i in range(n + 1):
        a = a0 + (a1 - a0) * i / n
        out.append((cx + r * math.cos(a), cz + r * math.sin(a)))
    return out


def polyline_s(xy):
    s = [0.0]
    for i in range(1, len(xy)):
        s.append(s[-1] + math.hypot(xy[i][0] - xy[i - 1][0], xy[i][1] - xy[i - 1][1]))
    return s


def offset_list(xy, off):
    """Shift every point by off metres along its own left normal (left = Y x travel = (dz, -dx))."""
    out = []
    n = len(xy)
    for i in range(n):
        a = xy[max(0, i - 1)]
        b = xy[min(n - 1, i + 1)]
        dx, dz = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dx, dz) or 1.0
        dx /= length
        dz /= length
        out.append((xy[i][0] + off * dz, xy[i][1] - off * dx))
    return out


def spine_y(s):
    """Mainline deck height along the spine: flat, 3% climb, 3% descent, flat."""
    if s <= 300.0:
        return DECK_Y
    if s <= 500.0:
        return DECK_Y + 0.03 * (s - 300.0)
    if s <= 700.0:
        return DECK_Y + 6.0 - 0.03 * (s - 500.0)
    return DECK_Y


def build_spine():
    pts = []
    pts += straight((-600.0, 0.0), (-200.0, 0.0))
    pts += arc(-200.0, 200.0, ARC_R, -math.pi / 2.0, 0.0)[1:]
    pts += straight((0.0, 200.0), (0.0, 500.0))[1:]
    return pts


def bank_list(xy_pts, s_vals):
    """Superelevation from signed curvature (left turn positive): bank = clamp(12 * kappa_left)."""
    n = len(xy_pts)
    heads = []
    for i in range(n):
        a = xy_pts[max(0, i - 1)]
        b = xy_pts[min(n - 1, i + 1)]
        heads.append(math.atan2(b[1] - a[1], b[0] - a[0]))
    banks = []
    for i in range(n):
        a = max(0, i - 1)
        b = min(n - 1, i + 1)
        ds = s_vals[b] - s_vals[a]
        if ds <= 1e-6:
            banks.append(0.0)
            continue
        dth = heads[b] - heads[a]
        while dth > math.pi:
            dth -= 2 * math.pi
        while dth < -math.pi:
            dth += 2 * math.pi
        kappa_left = -dth / ds  # left turn = heading angle decreasing in (x, z)
        banks.append(round(clamp(12.0 * kappa_left, -0.06, 0.06), 4))
    return banks


def ranges(total, parts):
    """parts: list of (d0, d1, type) with d0/d1 in metres; d1 None means end of carriageway."""
    out = []
    for d0, d1, t in parts:
        out.append({"s0": round(float(d0), 3), "s1": round(float(total if d1 is None else d1), 3), "type": t})
    return out


def make_cw(cid, route, direction, kind, frm, to, xy, ys, struct_parts, lanes, lane_w, sh_l, sh_r, speed,
            name_en, name_ja=""):
    s_vals = polyline_s(xy)
    total = s_vals[-1]
    pts = [[round(xy[i][0], 3), round(ys[i], 3), round(xy[i][1], 3)] for i in range(len(xy))]
    banks = bank_list(xy, s_vals)
    cw = {
        "id": cid,
        "route": route,
        "dir": direction,
        "kind": kind,
        "from": frm,
        "to": to,
        "lanes": lanes,
        "lane_width": lane_w,
        "shoulder_left": sh_l,
        "shoulder_right": sh_r,
        "speed_kmh": speed,
        "name_en": name_en,
        "name_ja": name_ja,
        "points": pts,
        "bank": banks,
        "structure": ranges(total, struct_parts(total)),
        "osm_ways": [],
    }
    return cw, total


def main():
    spine = build_spine()
    s_sp = polyline_s(spine)
    spine_ys = [spine_y(s) for s in s_sp]
    last = len(spine) - 1

    # Mainline E: offset to the left of travel (north on the straight), split at merge and diverge.
    mle = offset_list(spine, MAIN_OFFSET)
    i_merge = int(round(200.0))  # straight A: index == metres from x=-600
    i_div = int(round(300.0))
    merge_xy = mle[i_merge]
    div_xy = mle[i_div]

    # Mainline W: reversed spine, offset to its own left (south on the straight).
    rev_spine = list(reversed(spine))
    rev_s = [s_sp[last - i] for i in range(len(spine))]
    mlw = offset_list(rev_spine, MAIN_OFFSET)
    mlw_ys = [spine_y(rev_s[i]) for i in range(len(rev_spine))]

    # Explicit node positions (contract: nodes carry pos [x, y, z]).
    def node(nid, x, y, z, kind, exit_to=None):
        d = {"id": nid, "pos": [round(x, 3), round(y, 3), round(z, 3)], "kind": kind}
        if exit_to:
            d["exit"] = exit_to
        return d

    test_op = {"to": "Test feeder road", "operator": "synthetic"}
    nodes = [
        node("N000001", mle[0][0], spine_y(0.0), mle[0][1], "boundary", test_op),
        node("N000002", mle[last][0], spine_y(s_sp[last]), mle[last][1], "boundary", test_op),
        node("N000003", mlw[0][0], spine_y(rev_s[0]), mlw[0][1], "boundary", test_op),
        node("N000004", mlw[-1][0], spine_y(rev_s[-1]), mlw[-1][1], "boundary", test_op),
        node("N000005", merge_xy[0], DECK_Y, merge_xy[1], "merge"),
        node("N000006", div_xy[0], DECK_Y, div_xy[1], "diverge"),
    ]

    # Ramp geometry (straight lines in plan; vertical profile set by fraction along the length).
    access_a = (-690.0, -40.0)
    access_b = (-600.0, -40.0)
    ramp_end = (merge_xy[0], merge_xy[1])
    access_xy = straight(access_a, access_b)
    ramp_xy = straight(access_b, ramp_end)
    off_start = (div_xy[0], div_xy[1])
    off_end = (-266.0, -200.0)
    off_xy = straight(off_start, off_end)

    # PA loop: full left-turning circle starting and ending at the off-ramp end (tangent continuous).
    d_off = (off_end[0] - off_start[0], off_end[1] - off_start[1])
    dl = math.hypot(*d_off)
    d_off = (d_off[0] / dl, d_off[1] / dl)
    R_PA = 30.0
    left = (d_off[1], -d_off[0])
    cpa = (off_end[0] + R_PA * left[0], off_end[1] + R_PA * left[1])
    th0 = math.atan2(off_end[1] - cpa[1], off_end[0] - cpa[0])
    n_pa = 190
    pa_xy = []
    for i in range(n_pa + 1):
        th = th0 - 2.0 * math.pi * i / n_pa
        pa_xy.append((cpa[0] + R_PA * math.cos(th), cpa[1] + R_PA * math.sin(th)))
    nodes.append(node("N000007", access_a[0], GROUND_Y, access_a[1], "boundary", {"to": "Test feeder road", "operator": "synthetic"}))
    nodes.append(node("N000008", access_b[0], GROUND_Y, access_b[1], "split"))
    nodes.append(node("N000009", off_end[0], GROUND_Y, off_end[1], "split"))

    # Ground road C2 S-bound along x=-360 (crossing under the mainline), with covered section.
    g_xy = straight((-360.0, -380.0), (-360.0, 620.0))
    g_s = polyline_s(g_xy)
    g_ys = []
    for s in g_s:
        if s <= 420.0:
            y = GROUND_Y
        elif s <= 540.0:
            y = GROUND_Y - (GROUND_Y + 5.0) * (s - 420.0) / 120.0
        elif s <= 840.0:
            y = -5.0
        elif s <= 960.0:
            y = -5.0 + (GROUND_Y + 5.0) * (s - 840.0) / 120.0
        else:
            y = GROUND_Y
        g_ys.append(y)
    nodes.append(node("N000010", g_xy[0][0], GROUND_Y, g_xy[0][1], "boundary", {"to": "Test ground road north", "operator": "synthetic"}))
    nodes.append(node("N000011", g_xy[-1][0], GROUND_Y, g_xy[-1][1], "boundary", {"to": "Test ground road south", "operator": "synthetic"}))

    carriageways = []

    def main_lanes():
        return dict(lanes=2, lane_w=3.25, sh_l=0.75, sh_r=0.5, speed=80)

    def ramp_lanes():
        return dict(lanes=1, lane_w=3.5, sh_l=1.0, sh_r=0.5, speed=40)

    # Access road (ground).
    acc_ys = [GROUND_Y] * len(access_xy)
    cw, _ = make_cw("C1.E.000", "C1", "E", "jct", "N000007", "N000008", access_xy, acc_ys,
                    lambda t: [(0.0, None, "at_grade")], **dict(lanes=2, lane_w=3.25, sh_l=0.5, sh_r=0.5, speed=50),
                    name_en="Test access road")
    carriageways.append(cw)

    # On-ramp: 6 percent climb from ground to the mainline deck.
    L_ramp = polyline_s(ramp_xy)[-1]
    ramp_ys = [GROUND_Y + (DECK_Y - GROUND_Y) * (s / L_ramp) for s in polyline_s(ramp_xy)]
    cw, _ = make_cw("C1.E.004", "C1", "E", "ramp_on", "N000008", "N000005", ramp_xy, ramp_ys,
                    lambda t: [(0.0, 100.0, "embankment"), (100.0, None, "elevated")],
                    **ramp_lanes(), name_en="Test on-ramp")
    carriageways.append(cw)

    # Mainline E in three pieces split at merge and diverge.
    def elev(t):
        return [(0.0, None, "elevated")]

    # Mainline E pieces share the exact node points (same spine index), so their ribbons meet cleanly.
    e_ys = [spine_y(s_sp[i]) for i in range(len(spine))]
    seg_defs = [
        ("C1.E.001", "N000001", "N000005", 0, i_merge, "Test mainline E 1"),
        ("C1.E.002", "N000005", "N000006", i_merge, i_div, "Test mainline E 2"),
        ("C1.E.003", "N000006", "N000002", i_div, last, "Test mainline E 3"),
    ]
    for cid, frm, to, a, b, name in seg_defs:
        cw, _ = make_cw(cid, "C1", "E", "main", frm, to, mle[a:b + 1], e_ys[a:b + 1], elev,
                        **main_lanes(), name_en=name)
        carriageways.append(cw)

    # Off-ramp: elevated near the mainline, embankment near the parking area.
    L_off = polyline_s(off_xy)[-1]
    off_ys = [DECK_Y - (DECK_Y - GROUND_Y) * (s / L_off) for s in polyline_s(off_xy)]
    cw, _ = make_cw("C1.E.005", "C1", "E", "ramp_off", "N000006", "N000009", off_xy, off_ys,
                    lambda t: [(0.0, 100.0, "elevated"), (100.0, None, "embankment")],
                    **ramp_lanes(), name_en="Test off-ramp")
    carriageways.append(cw)

    # Parking area loop at ground level.
    cw, _ = make_cw("C1.E.006", "C1", "E", "pa", "N000009", "N000009", pa_xy, [GROUND_Y] * len(pa_xy),
                    lambda t: [(0.0, None, "at_grade")], lanes=1, lane_w=3.5, sh_l=0.5, sh_r=0.5, speed=20,
                    name_en="Test parking area loop")
    carriageways.append(cw)

    # Mainline W, continuous.
    cw, _ = make_cw("C1.W.001", "C1", "W", "main", "N000003", "N000004", mlw, mlw_ys, elev,
                    **main_lanes(), name_en="Test mainline W")
    carriageways.append(cw)

    # Ground road with covered section: at grade, cut descent, tunnel floor at y=-5, cut ascent.
    def g_struct(total):
        return [(0.0, 420.0, "at_grade"), (420.0, 540.0, "cut"), (540.0, 840.0, "tunnel"),
                (840.0, 960.0, "cut"), (960.0, None, "at_grade")]

    cw, _ = make_cw("C2.S.001", "C2", "S", "main", "N000010", "N000011", g_xy, g_ys, g_struct,
                    lanes=2, lane_w=3.25, sh_l=0.75, sh_r=0.5, speed=60, name_en="Test ground road south")
    carriageways.append(cw)

    # Mainline E / W carriageways keep their own structure ranges; ensure all ids are unique.
    ids = [c["id"] for c in carriageways]
    assert len(ids) == len(set(ids)), "duplicate carriageway id"

    # Parking area facility at the loop centre.
    facilities = [{
        "id": "PA.Test",
        "type": "pa",
        "name_en": "Test Parking Area",
        "name_ja": "",
        "pos": [round(cpa[0], 3), GROUND_Y, round(cpa[1], 3)],
        "carriageways": ["C1.E.006"],
        "notes": "Synthetic loop, entered from the C1.E.005 off-ramp. Dead-end loop for events.",
    }]

    network = {
        "format": "nox-shuto-network",
        "version": 1,
        "reference_date": "2002-12-31",
        "origin": {"lat": 35.6843, "lon": 139.7745, "name": "Synthetic test site (not Edobashi)"},
        "ground_y": GROUND_Y,
        "sources": [{
            "id": "synthetic",
            "license": "project-internal",
            "attribution": "Synthetic test geometry from tools/shuto/make_test_network.py",
            "extract": "none",
            "date": "2026-10-10",
        }],
        "nodes": nodes,
        "carriageways": carriageways,
        "facilities": facilities,
        "excluded": [],
    }

    # NOX graph: contract node ids at junctions, one node per other centreline point.
    nox_nodes = []
    nox_edges = []
    seen = set()
    for cw in carriageways:
        pts = cw["points"]
        ids_here = []
        for i, p in enumerate(pts):
            if i == 0:
                nid = cw["from"]
            elif i == len(pts) - 1:
                nid = cw["to"]
            else:
                nid = "%s.p%04d" % (cw["id"], i)
            ids_here.append(nid)
            if nid in seen:
                continue
            seen.add(nid)
            nox_nodes.append({"id": nid, "pos": p})
        half = cw["lanes"] * cw["lane_width"] * 0.5 + max(cw["shoulder_left"], cw["shoulder_right"])
        for i in range(len(pts) - 1):
            nox_edges.append({
                "a": ids_here[i], "b": ids_here[i + 1],
                "name": cw["name_en"], "carriageway": cw["id"],
                "oneway": True, "lanes": cw["lanes"], "lane_width": cw["lane_width"],
                "half_width": round(half, 3),
            })
    nox = {
        "traffic_side": "left",
        "band_walls": False,
        "sidewalk_width": 0.0,
        "road_half_width": 4.0,
        "nodes": nox_nodes,
        "edges": nox_edges,
    }

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "test_network.json"), "w", encoding="ascii", newline="\n") as f:
        json.dump(network, f, indent=1)
        f.write("\n")
    with open(os.path.join(OUT_DIR, "test_nox_road_network.json"), "w", encoding="ascii", newline="\n") as f:
        json.dump(nox, f)
        f.write("\n")

    print("carriageways:", len(carriageways), "nodes:", len(nodes))
    for cw in carriageways:
        print("  %-12s %-9s pts=%4d len=%.1f" % (cw["id"], cw["kind"], len(cw["points"]),
                                              polyline_s([(p[0], p[2]) for p in cw["points"]])[-1]))
    print("NOX nodes:", len(nox_nodes), "edges:", len(nox_edges))


if __name__ == "__main__":
    main()
