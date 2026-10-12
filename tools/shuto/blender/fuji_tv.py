"""Reference-led Fuji TV headquarters: asymmetric towers, six bridges and mast clusters.

Measured silhouette ratios use the supplied waterfront photo, not the generic facade atlas.
Revision 3: pale cladding (panel bands share the aluminium material), 31 m podium, two-storey bridges at 46/73/97 m, sphere centre 106 m.
Overall campus extent is an artistic estimate; 123.45 m height and 32 m sphere are documented.
"""
import math
from mathutils import Vector

# Sphere centre above the base (32 m diameter). In the reference photo the sphere top is level with the
# office-wing roof equipment (123.45 m overall) and its centre just above the top sky bridge.
SPHERE_Y = 106.0
# Sky-bridge levels (bottom, height) measured against the 123.45 m wing in the reference photo.
BRIDGES = ((46.0, 6.0), (73.0, 6.0), (97.0, 6.5))
# Towers stand on a 31 m podium (about seven lower storeys in the reference photo).
PODIUM = 31.0


def build(b, lod, base, a):
    bk = a.g.Buckets()
    key = b["id"]
    cx, cz = b["obb"]["c"]
    ux, uz = b["obb"]["u"]

    def pt(x, z):
        x, z = x - 2, z - 16.5 + b.get("model_offset_m", 0)
        # Screen-right on the waterfront elevation is -u; front points along the northwest normal.
        return (cx - ux * x + uz * z, cz - uz * x - ux * z)

    def box(mat, x, z, w, d, y, h):
        a.add_box(bk, (key, mat), pt(x, z), (ux, uz), w, d, base + y, h)

    def face(mat, x0, x1, z, y0, y1, direction=1):
        a.panel(bk, (key, mat), pt(x0, z), pt(x1, z), base + y0, base + y1,
                (uz * direction, 0, -ux * direction))

    def beam(mat, p, q, w=0.65, d=0.65):
        p, q = Vector((*pt(p[0], p[2]), p[1])), Vector((*pt(q[0], q[2]), q[1]))
        # Rearrange local planar projection into NOX xyz.
        p, q = Vector((p.x, base + p.z, p.y)), Vector((q.x, base + q.z, q.y))
        axis = (q - p).normalized()
        side = axis.cross(Vector((0, 1, 0)))
        if side.length < 0.01:
            side = axis.cross(Vector((0, 0, 1)))
        side.normalize()
        up = side.cross(axis).normalized()
        verts = [v + side * s * w / 2 + up * t * d / 2 for v in (p, q) for s, t in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        center = (p + q) / 2
        for indices in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            points = [verts[i] for i in indices]
            want = sum(points, Vector()) / 4 - center
            a.g.quad_nox(bk, (key, mat), *points, (0, 0), (1, 0), (1, 1), (0, 1), want)

    def wing(x0, x1, z0, z1, y0, top, major_bays):
        ring = [pt(x0, z0), pt(x1, z0), pt(x1, z1), pt(x0, z1)]
        a.g.emit_roof(bk, (key, "fuji_aluminium"), ring, base + top, 4)
        a.bottom(bk, (key, "fuji_aluminium"), ring, base + y0)
        pitch = 4.1
        floors = max(1, round((top - y0) / pitch))
        pitch = (top - y0) / floors
        for edge, (p, q, length) in enumerate(a.g.wall_edges(ring)):
            normal = a.normal(p, q, ring)
            tangent = ((q[0] - p[0]) / length, (q[1] - p[1]) / length)
            bays = max(1, round(length / 3.7))
            def along(s):
                return (p[0] + tangent[0] * s, p[1] + tangent[1] * s)
            if x0 == -147 and edge == 2 and lod < 2:
                a.panel(bk, (key, "fuji_aluminium"), p, q, base + y0, base + 5, normal)
                a.panel(bk, (key, "fuji_glass"), p, q, base + 5, base + 21, normal)
                a.panel(bk, (key, "fuji_aluminium"), p, q, base + 21, base + top, normal)
                continue
            if lod == 2:
                # Distant silhouette retains floor ribbons and structural bay proportions.
                for f in range(floors):
                    yy = base + y0 + f * pitch
                    a.panel(bk, (key, "fuji_aluminium"), p, q, yy, yy + 0.8, normal)
                    a.panel(bk, (key, "fuji_glass"), p, q, yy + 0.8, yy + 3.0, normal)
                    a.panel(bk, (key, "fuji_aluminium"), p, q, yy + 3.0, yy + pitch, normal)
            else:
                for f in range(floors):
                    yy = base + y0 + f * pitch
                    for j in range(bays):
                        s0, s1 = (j + 0.12) * length / bays, (j + 0.88) * length / bays
                        for lo, hi in ((j * length / bays, s0), (s1, (j + 1) * length / bays)):
                            a.panel(bk, (key, "fuji_aluminium"), along(lo), along(hi), yy + 0.9, yy + 3.1, normal)
                        mat = "fuji_window_warm" if (j * 19 + f * 13 + edge * 7) % 11 in (0, 1) else "fuji_glass"
                        if lod == 0:
                            # Split each paired window with an actual aluminium sash.
                            middle = (s0 + s1) / 2
                            a.panel(bk, (key, mat), along(s0), along(middle - 0.045), yy + 0.9, yy + 3.1, normal)
                            a.panel(bk, (key, mat), along(middle + 0.045), along(s1), yy + 0.9, yy + 3.1, normal)
                            a.panel(bk, (key, "fuji_aluminium"), along(middle - 0.045), along(middle + 0.045),
                                    yy + 0.9, yy + 3.1, normal)
                        else:
                            a.panel(bk, (key, mat), along(s0), along(s1), yy + 0.9, yy + 3.1, normal)
                    # Thin horizontal aluminum floor bands, no painted shadow atlas.
                    for lo, hi in ((0, 0.9), (3.1, 3.35), (3.63, pitch)):
                        a.panel(bk, (key, "fuji_aluminium"), p, q, yy + lo, yy + hi, normal)
                    a.panel(bk, (key, "fuji_aluminium"), p, q, yy + 3.35, yy + 3.63, normal)
        # Large expressed bay grid on front/rear; it dominates the reference silhouette.
        spacing = (x1 - x0) / major_bays
        for j in range(major_bays + 1):
            x = x0 + j * spacing
            for z in (z0 - 0.35, z1 + 0.35):
                box("fuji_aluminium", x, z, 0.95, 0.8, y0, top - y0 + 1.0)
        for yy in (y0, top - 1.5, top):
            for z in (z0 - 0.45, z1 + 0.45):
                box("fuji_aluminium", (x0 + x1) / 2, z, x1 - x0 + 1.8, 1.1, yy, 0.65)

    # Office wing is narrower and taller; media wing is broad and slightly lower.
    wing(-118, -65, -31, 27, PODIUM, 119.0, 4)
    wing(35, 114, -31, 27, PODIUM, 110.0, 6)
    # Lower complex is wider than the towers and steps toward the waterfront.
    wing(-147, -78, 28, 57, 0, PODIUM, 4)
    wing(50, 151, 27, 63, 0, PODIUM - 3, 6)
    box("fuji_aluminium", -13, 5, 130, 57, 0, PODIUM - 2)
    podium = [pt(-84.5, 28.5), pt(52.5, 28.5), pt(52.5, 39.5), pt(-84.5, 39.5)]
    a.g.emit_roof(bk, (key, "fuji_aluminium"), podium, base + 15, 4)
    a.bottom(bk, (key, "fuji_aluminium"), podium, base)
    for edge, (p, q, _) in enumerate(a.g.wall_edges(podium)):
        n = a.normal(p, q, podium)
        bands = ((0, 4, "fuji_aluminium"), (4, 6.1, "fuji_glass"),
                 (6.1, 9, "fuji_aluminium"), (9, 11.1, "fuji_glass"),
                 (11.1, 15, "fuji_aluminium")) if edge == 2 else ((0, 15, "fuji_aluminium"),)
        for lo, hi, mat in bands:
            a.panel(bk, (key, mat), p, q, base + lo, base + hi, n)
    # Recessed front atrium and layered studio terraces.
    for x in range(-139, -85, 9):
        box("fuji_aluminium", x, 57.25, 0.65, 0.7, 5, 17)
    for x, w, top, z in ((-111, 71, PODIUM + 0.6, 44), (100, 103, PODIUM - 2.4, 47), (-13, 129, PODIUM - 1.5, 3)):
        box("fuji_aluminium", x, z, w, 28, top, 0.55)

    # Three front/rear corridor pairs, rather than a uniform scaffolding grid.
    for yy, depth in BRIDGES:
        for z in (-20, 20):
            corridor = [pt(-65.5, z - 4), pt(35.5, z - 4), pt(35.5, z + 4), pt(-65.5, z + 4)]
            a.g.emit_roof(bk, (key, "fuji_aluminium"), corridor, base + yy + depth, 4)
            a.bottom(bk, (key, "fuji_aluminium"), corridor, base + yy)
            for edge, (p, q, _) in enumerate(a.g.wall_edges(corridor)):
                n = a.normal(p, q, corridor)
                if edge % 2:
                    a.panel(bk, (key, "fuji_aluminium"), p, q, base + yy, base + yy + depth, n)
                    continue
                # Two-storey bridge: two glazed bands between aluminium floor plates.
                for lo, hi, mat in ((0, 0.9, "fuji_aluminium"), (0.9, 2.6, "fuji_glass"), (2.6, 3.4, "fuji_aluminium"),
                                    (3.4, depth - 0.9, "fuji_glass"), (depth - 0.9, depth, "fuji_aluminium")):
                    a.panel(bk, (key, mat), p, q, base + yy + lo, base + yy + hi, n)
    # Expressed columns in four-shaft clusters, with open sky between the corridors.
    for x in (-118, -65, -12, 12, 35, 74, 114):
        for z in (-28, 28):
            top = 120.5 if x <= -65 else 114.0 if x < 35 else 111.5
            if lod == 2:
                box("fuji_aluminium", x, z, 2.0, 2.0, PODIUM, top - PODIUM)
                continue
            for dx in (-0.65, 0.65):
                for dz in (-0.65, 0.65):
                    box("fuji_aluminium", x + dx, z + dz, 0.5, 0.5, PODIUM, top - PODIUM)
            for yy in range(int(PODIUM) + 2, int(top), 8):
                box("fuji_aluminium", x, z, 2.25, 2.25, yy, 0.3)

    # The observatory is offset toward the left wing and supported above the middle bridge.
    sx, sz = -37, 21
    for x in (sx - 9, sx + 9):
        for z in (sz - 6, sz + 6):
            box("fuji_aluminium", x, z, 1.25, 1.25, 79, SPHERE_Y - 15 - 79)
    if lod < 2:
        for z in (sz - 6, sz + 6):
            beam("fuji_aluminium", (sx - 9, 80, z), (sx + 9, SPHERE_Y - 15.5, z), 0.45, 0.45)
            beam("fuji_aluminium", (sx + 9, 80, z), (sx - 9, SPHERE_Y - 15.5, z), 0.45, 0.45)
    sphere_c = pt(sx, sz)
    a.SPHERE_CENTERS[key] = Vector((sphere_c[0], -sphere_c[1], base + SPHERE_Y))
    rings, sides = (40, 80) if lod == 0 else (24, 48) if lod == 1 else (12, 24)
    phis = sorted(set([math.pi * i / rings for i in range(rings + 1)] + [math.acos(1 / 16), math.acos(-3 / 16)]))
    def sphere(phi, j):
        theta = 2 * math.pi * j / sides
        return (sphere_c[0] + 16 * math.sin(phi) * math.cos(theta), base + SPHERE_Y + 16 * math.cos(phi),
                sphere_c[1] + 16 * math.sin(phi) * math.sin(theta))
    for i in range(len(phis) - 1):
        mid_y = 16 * math.cos((phis[i] + phis[i + 1]) / 2)
        belt = -3 < mid_y < 1
        for j in range(sides):
            # Observation band shares the campus glass material (one fewer draw call).
            mat = "fuji_glass" if belt and j % 3 != 0 else "fuji_sphere_titanium"
            p00, p10 = sphere(phis[i], j), sphere(phis[i], j + 1)
            p11, p01 = sphere(phis[i + 1], j + 1), sphere(phis[i + 1], j)
            want = tuple((p00[k] + p11[k]) / 2 - (sphere_c[0], base + SPHERE_Y, sphere_c[1])[k] for k in range(3))
            if i == 0:
                a.g.tri_nox(bk, (key, mat), p00, p11, p01, (0, 0), (1, 1), (0, 1), want)
            elif i == len(phis) - 2:
                a.g.tri_nox(bk, (key, mat), p00, p10, p11, (0, 0), (1, 0), (1, 1), want)
            else:
                a.g.quad_nox(bk, (key, mat), p00, p10, p11, p01, (0, 0), (1, 0), (1, 1), (0, 1), want)
    if lod < 2:
        # Roof equipment and antenna silhouettes, within the documented overall height.
        for x, z, y in ((-113, -22, 119.7), (-73, 17, 119.7), (108, -19, 111), (85, 20, 111)):
            box("fuji_aluminium", x, z, 2.4, 2.4, y, 1.0)
            box("fuji_aluminium", x, z, 0.16, 0.16, y + 1, min(123.3 - y - 1, 6))
        # Grand stairs and tube escalator create the diagonal circulation in front of the studios.
        for step in range(32 if lod == 0 else 16):
            steps = 32 if lod == 0 else 16
            fraction = step / steps
            box("fuji_aluminium", 31 - 58 * fraction, 52, 58 / steps + 0.02, 10, 0, 1 + (PODIUM - 3) * fraction)
        beam("fuji_aluminium", (31, 2, 59), (-27, PODIUM - 1, 59), 2.7, 2.7)
        beam("fuji_glass", (31, 3.5, 59), (-27, PODIUM + 0.5, 59), 1.7, 1.2)
    return bk
