#ifndef NOX_INTERIOR_MAPPING_GLSL
#define NOX_INTERIOR_MAPPING_GLSL

// Window interior mapping: a procedural parallax room behind each window pane (no atlas sampling).
//
// Geometry contract (ProceduralStreetKit QuadUV windows): the pane is 1.6 m wide along the tangent
// (image right) and 2.0 m tall along the bitangent (image up). Its UV spans one 0.242 tile of the
// 4x4 atlas grid starting at (tile * 0.25 + 0.004). The tile origin is recovered with floor(uv * 4),
// which gives the pane-local coordinates. The pane centre follows from the world position, so each
// window gets a stable id without any extra vertex data.
//
// Output is relative room radiance (about 0..1 for a fully lit room). The caller multiplies it by
// emissiveFactor and interiorStrength; the material emissiveStrength (cd/m^2) is applied by lighting.
// The glass itself is left to the material as a smooth dielectric.

const float NOX_IM_HALF_W = 0.8;   // pane half width (m)
const float NOX_IM_HALF_H = 1.0;   // pane half height (m)
const float NOX_IM_TILE = 0.242;   // atlas UV span of one pane

float NoxIMHash(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float NoxIMRand(vec3 id, float k) {
    return NoxIMHash(id + vec3(k, k * 0.731, k * 1.913));
}

// Slab test for an axis-aligned box. Returns the entry distance, or -1 on a miss.
float NoxIMBox(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax, out float tExit) {
    vec3 inv = 1.0 / rd;
    vec3 t0 = (bmin - ro) * inv;
    vec3 t1 = (bmax - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    float tEnter = max(max(tmin.x, tmin.y), tmin.z);
    tExit = min(min(tmax.x, tmax.y), tmax.z);
    return (tExit >= max(tEnter, 0.0)) ? tEnter : -1.0;
}

struct NoxIMRoom {
    vec3 lampPos;   // ceiling lamp position (room space)
    float lampGain; // lamp-pool peak (0 when the room has no lamp)
    float lampR2;   // squared falloff radius of the lamp pool
    float fluor;    // flat overhead light level (offices)
    float ambient;  // faint wall glow (dark rooms still read as rooms)
    float tvGain;   // TV glow strength (0 when no TV)
    vec2 tvPos;     // TV centre on the back wall
    float depth;    // room depth (m)
};

float NoxIMLight(NoxIMRoom R, vec3 p) {
    vec3 q = p - R.lampPos;
    float pool = R.lampGain / (1.0 + dot(q, q) / R.lampR2);
    vec3 tq = p - vec3(R.tvPos, -R.depth);
    float tv = R.tvGain * exp(-dot(tq, tq) * 1.8);
    return R.ambient + pool + R.fluor + tv;
}

// Radiance seen through one window pane.
//   worldPos   point on the pane (world)
//   geoN       pane normal (world, outward)
//   tangentW   pane tangent (world, image right); tangentSign is the glTF handedness
//   uv         TEXCOORD_0 of the window quad (atlas tile UV)
//   camPos     eye position (world)
//   depthBase  room depth in metres (material interiorDepth)
//   lightBias  occupancy bias, 0 = mostly dark rooms, 1 = mostly lit rooms
//   blindBias  probability of blinds and curtains
//   style      0 apartment/office, 1 shop interior (shelf racks on the back wall)
//   seed       per-material variation
//   footprint  world size of one pixel (m); fine detail fades out beyond a few cm
//   timeS      seconds, for TV flicker
vec3 NoxInteriorWindow(vec3 worldPos, vec3 geoN, vec3 tangentW, float tangentSign, vec2 uv,
                       vec3 camPos, float depthBase, float lightBias, float blindBias,
                       float style, float seed, float footprint, float timeS) {
    vec3 N = normalize(geoN);
    vec3 T = normalize(tangentW - dot(tangentW, N) * N);
    vec3 B = normalize(cross(N, T) * tangentSign);

    // Pane-local coordinates: x right (-0.8..0.8), y up (-1..1), metres.
    vec2 tileMin = floor(uv * 4.0) * 0.25 + vec2(0.004);
    vec2 local01 = vec2((uv.x - tileMin.x) / NOX_IM_TILE,
                        (tileMin.y + NOX_IM_TILE - uv.y) / NOX_IM_TILE);
    local01 = clamp(local01, vec2(0.0), vec2(1.0));
    vec2 w = (local01 - 0.5) * vec2(2.0 * NOX_IM_HALF_W, 2.0 * NOX_IM_HALF_H);

    // Stable per-window id from the pane centre (exact up to float noise, centres sit on 0.2 m).
    vec3 paneCentre = worldPos - T * w.x - B * w.y;
    vec3 id = floor(paneCentre * 5.0 + 0.123) + vec3(seed * 7.31, seed * 3.17, seed * 5.53);

    // Ray into the room in pane space: x along T, y along B, z along the outward normal.
    // Viewed from the front, rd.z < 0 means the ray travels into the building.
    vec3 dir = normalize(worldPos - camPos);
    vec3 rd = vec3(dot(dir, T), dot(dir, B), dot(dir, N));
    rd.z = min(rd.z, -1e-3);
    if (abs(rd.x) < 1e-4) rd.x = (rd.x < 0.0) ? -1e-4 : 1e-4;
    if (abs(rd.y) < 1e-4) rd.y = (rd.y < 0.0) ? -1e-4 : 1e-4;

    float shop = step(0.5, style);

    // Room layout (half extents about the pane centre; floor sits near the sill).
    float roomW = 1.6 + 1.2 * NoxIMRand(id, 5.0);
    float roomH = 1.7 + 0.2 * NoxIMRand(id, 6.0);
    float roomD = depthBase * (0.75 + 0.5 * NoxIMRand(id, 7.0));

    // Occupancy and light source: lit rooms are a minority unless lightBias is high.
    float lit = step(NoxIMRand(id, 1.0), mix(0.08, 0.7, lightBias));
    float kind = NoxIMRand(id, 4.0);
    float poolOn = lit * (1.0 - step(0.55, kind));                 // table/floor lamp pool
    float fluorOn = lit * step(0.55, kind) * (1.0 - step(0.8, kind)); // flat office light

    NoxIMRoom R;
    R.lampPos = vec3((NoxIMRand(id, 8.0) - 0.5) * roomW, roomH * 0.8, -roomD * (0.3 + 0.3 * NoxIMRand(id, 9.0)));
    R.lampR2 = max(0.25 * roomD * roomD, 0.5);
    R.lampGain = poolOn * mix(0.5, 1.0, NoxIMRand(id, 2.0)) * 1.2;
    R.fluor = fluorOn * mix(0.35, 0.55, NoxIMRand(id, 2.5));
    R.ambient = mix(0.03, 0.09, NoxIMRand(id, 3.0)) + lit * 0.10;
    float tvOn = step(NoxIMRand(id, 12.0), 0.25 + 0.35 * lit);
    R.tvGain = tvOn * mix(0.25, 0.5, NoxIMRand(id, 13.0));
    R.tvPos = vec2((NoxIMRand(id, 14.0) - 0.5) * roomW * 0.8, -0.1);
    R.depth = roomD;

    float warm = step(0.5, NoxIMRand(id, 10.0));
    vec3 wallAlb = mix(vec3(0.6, 0.53, 0.44), vec3(0.48, 0.52, 0.58), warm) * mix(0.55, 0.9, NoxIMRand(id, 11.0));
    vec3 floorAlb = mix(vec3(0.3, 0.19, 0.12), vec3(0.27, 0.27, 0.29), step(0.5, NoxIMRand(id, 15.0)));
    float flick = 0.8 + 0.2 * NoxIMHash(vec3(floor(timeS * 9.0), id.x, id.y));
    vec3 tvCol = vec3(0.55, 0.7, 1.0) * flick * 0.8;

    // Furniture block (sofa, bed or desk) standing on the floor.
    float fHave = step(0.25, NoxIMRand(id, 16.0));
    float fx = (NoxIMRand(id, 17.0) - 0.5) * roomW * 1.1;
    float fz = -roomD * (0.45 + 0.3 * NoxIMRand(id, 18.0));
    vec3 fMin = vec3(fx - 0.75, -roomH, fz - 0.3);
    vec3 fMax = vec3(fx + 0.75, -roomH + mix(0.5, 0.9, NoxIMRand(id, 19.0)), fz + 0.3);
    vec3 furnAlb = mix(vec3(0.16, 0.1, 0.07), vec3(0.3, 0.3, 0.32), NoxIMRand(id, 20.0));
    float tBox;
    float tBoxExit;
    tBox = NoxIMBox(vec3(w, 0.0), rd, fMin, fMax, tBoxExit);
    tBox = (fHave > 0.5 && tBox > 0.0) ? tBox : 1e9;

    // Person silhouette standing in lit rooms (dropped at distance, where it would only alias).
    float detail = 1.0 - smoothstep(0.012, 0.04, footprint);
    float pHave = lit * step(0.55, NoxIMRand(id, 21.0)) * detail;
    float px = (NoxIMRand(id, 22.0) - 0.5) * roomW * 0.8;
    float pz = -roomD * (0.35 + 0.3 * NoxIMRand(id, 23.0));
    float ph = mix(1.55, 1.75, NoxIMRand(id, 24.0));
    float tp = pz / rd.z;
    vec2 pxy = w + rd.xy * tp;
    float body = step(abs(pxy.x - px), 0.22) * step(pxy.y, -roomH + 1.25) * step(-roomH, pxy.y);
    float head = step(length(pxy - vec2(px, -roomH + ph - 0.13)), 0.13);
    float personHit = pHave * max(body, head) * step(tp, 1e8);

    // Exit point of the room box: back wall, side wall, floor or ceiling.
    float tx = (rd.x > 0.0 ? (roomW - w.x) : (-roomW - w.x)) / rd.x;
    float ty = (rd.y > 0.0 ? (roomH - w.y) : (-roomH - w.y)) / rd.y;
    float tz = -roomD / rd.z;
    float tHit = min(tz, min(tx, ty));
    vec3 hp = vec3(w, 0.0) + rd * tHit;

    vec3 col;
    if (tz <= min(tx, ty)) {
        // Back wall: optional shop shelf rack, TV screen.
        vec3 alb = wallAlb;
        float rack = shop * step(0.45, fract(hp.y * 2.5)) * step(hp.y, 0.7);
        float items = step(0.5, fract(hp.x * 7.0 + floor(hp.y * 2.5) * 0.37));
        alb *= mix(1.0, 0.45 + 0.55 * items, rack * detail);
        float tvRect = tvOn * step(abs(hp.x - R.tvPos.x), 0.42) * step(abs(hp.y - R.tvPos.y), 0.26);
        col = mix(alb * NoxIMLight(R, hp), tvCol, tvRect);
    }
    else if (tx <= ty) {
        col = wallAlb * 0.92 * NoxIMLight(R, hp);
    }
    else {
        vec3 alb = (rd.y < 0.0) ? floorAlb : wallAlb * 1.05;
        col = alb * NoxIMLight(R, hp);
    }

    // Furniture in front of the back wall.
    if (tBox < tHit) {
        vec3 hb = vec3(w, 0.0) + rd * tBox;
        col = furnAlb * NoxIMLight(R, hb) * 0.7;
    }

    // Person silhouette (a near-black figure against the lit room).
    if (personHit > 0.5 && tp < min(tHit, tBox)) {
        col = vec3(0.012);
    }

    // Blinds: a plane just behind the glass, drawn down from the top, with slats.
    float tbz = -0.08 / rd.z;
    vec2 wb = w + rd.xy * tbz;
    float blindOn = step(NoxIMRand(id, 25.0), blindBias);
    float cover = mix(0.25, 1.0, NoxIMRand(id, 26.0));
    float covered = step(1.0 - 2.0 * cover, wb.y);
    float slat = fract(wb.y / 0.09);
    float gap = mix(0.15, 0.4, NoxIMRand(id, 27.0));
    float aa = smoothstep(0.004, 0.02, footprint);
    float blindMask = blindOn * covered * mix(1.0 - step(slat, gap), 0.6, aa);
    vec3 blindCol = mix(vec3(0.85, 0.8, 0.7), vec3(0.65, 0.68, 0.72), NoxIMRand(id, 28.0));
    float blindLit = NoxIMLight(R, vec3(wb, -0.3));
    blindCol *= 0.05 + 0.6 * min(blindLit, 1.5);
    col = mix(col, blindCol, blindMask);

    // Curtains: fabric at one or both pane edges, partly drawn back (open middle).
    float curtOn = step(NoxIMRand(id, 29.0), 0.2 + 0.6 * blindBias);
    float cw = mix(0.16, 0.42, NoxIMRand(id, 30.0));
    float cutL = step(wb.x, -NOX_IM_HALF_W + cw) * step(0.35, NoxIMRand(id, 31.0));
    float cutR = step(NOX_IM_HALF_W - cw, wb.x) * step(0.35, NoxIMRand(id, 32.0));
    float curtMask = curtOn * max(cutL, cutR);
    vec3 curtAlb = mix(vec3(0.45, 0.14, 0.1), vec3(0.2, 0.3, 0.32), NoxIMRand(id, 33.0));
    float fold = mix(0.9, 0.8 + 0.2 * sin(wb.x * 55.0), detail);
    float curtLit = 0.08 + 0.45 * min(NoxIMLight(R, vec3(wb, -0.2)), 1.5);
    col = mix(col, curtAlb * fold * curtLit, curtMask);

    // Sash frame and central mullion sit in the glass plane.
    float edge = min(NOX_IM_HALF_W - abs(w.x), NOX_IM_HALF_H - abs(w.y));
    float frame = 1.0 - smoothstep(0.025, 0.04, edge);
    float mullion = 1.0 - smoothstep(0.012, 0.022, abs(w.x));
    col *= 1.0 - 0.8 * max(frame, 0.8 * mullion);

    // Far away the room averages out to a soft glow instead of aliasing.
    vec3 avg = vec3(0.5, 0.45, 0.4) * (0.04 + 0.3 * lit);
    col = mix(avg, col, 0.4 + 0.6 * detail);

    return max(col, vec3(0.0));
}

#endif // NOX_INTERIOR_MAPPING_GLSL
