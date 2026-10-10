#ifndef NOX_INTERIOR_MAPPING_GLSL
#define NOX_INTERIOR_MAPPING_GLSL

// Window interior mapping: a parallax room box behind each window pane.
//
// Geometry contract (window quads): the pane spans 2 * paneHalf metres (material interiorPane,
// default 0.8 x 1.0 = 1.6 x 2.0 m) along the tangent (image right) and the bitangent (image up). Its UV
// spans one 0.242 tile of a 4x4 grid starting at (tile * 0.25 + 0.004). The tile origin is recovered
// with floor(uv * 4), which gives the pane-local coordinates. The pane centre follows from the world
// position, so each window gets a stable id without extra vertex data.
//
// Room source:
//   * Interior atlas (SceneRenderExtras::interiorAtlas, bound by GBufferPass): N baked five-face rooms,
//     one texture-array layer per face, so trilinear/LOD sampling never bleeds between faces. Baked rooms
//     carry their own lighting and furniture: no procedural light, furniture or figures are added.
//     The variant is chosen per window from the stable id with the scene's per-style weights; the
//     material's interiorStyle selects the weight row.
//   * Fallback (no atlas): the procedural room below.
// Blinds, curtains, the sash frame and the mullion stay in the foreground for both sources.
//
// Output is relative room radiance (about 0..1 for a fully lit room). The caller multiplies it by
// emissiveFactor and interiorStrength; the material emissiveStrength (cd/m^2) is applied by lighting.
// The glass itself is left to the material as a smooth dielectric.

const float NOX_IM_TILE = 0.242;   // atlas UV span of one pane

// Interior atlas (GBufferPass binds it from SceneRenderExtras::interiorAtlas).
const int NOX_ROOM_FACES = 5;      // layer = room * 5 + face
const int NOX_FACE_BACK = 0;
const int NOX_FACE_LEFT = 1;
const int NOX_FACE_RIGHT = 2;
const int NOX_FACE_CEILING = 3;
const int NOX_FACE_FLOOR = 4;
const int NOX_ROOMS_MAX = 8;       // InteriorAtlas::kMaxRooms
const int NOX_STYLES_MAX = 4;      // InteriorAtlas::kMaxStyles
uniform sampler2DArray uInteriorAtlas;
uniform bool uInteriorAtlasReady = false;
uniform int uInteriorRoomCount = 0;
uniform float uInteriorFaceSize = 512.0;                       // texels per face layer edge
uniform vec3 uInteriorFaceMean[NOX_ROOMS_MAX * NOX_ROOM_FACES];  // mean radiance per face (distant fallback)
uniform vec3 uInteriorRoomLight[NOX_ROOMS_MAX];                // occupied brightness, chance on when unoccupied, that brightness
uniform float uInteriorStyleWeights[NOX_STYLES_MAX * NOX_ROOMS_MAX]; // [style * NOX_ROOMS_MAX + room]

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

// What the foreground layers need from either room source.
struct NoxIMRoomSample {
    vec3 color;      // room radiance through the pane
    vec3 average;    // stable distant average (no aliasing)
    float blindLit;  // light level on blinds just behind the glass
    float curtLit;   // light level on curtains
};

// ------------------------------------------------------------------------------------------------
// Atlas rooms
// ------------------------------------------------------------------------------------------------

// Weighted variant choice from the material style's weight row (stable per window id).
int NoxIMPickRoom(vec3 id, float style) {
    int row = clamp(int(style + 0.5), 0, NOX_STYLES_MAX - 1) * NOX_ROOMS_MAX;
    int count = clamp(uInteriorRoomCount, 1, NOX_ROOMS_MAX);
    float total = 0.0;
    for (int i = 0; i < count; ++i) total += max(uInteriorStyleWeights[row + i], 0.0);
    if (total <= 0.0) {
        return int(NoxIMRand(id, 40.0) * float(count)) % count;
    }
    float r = NoxIMRand(id, 40.0) * total;
    for (int i = 0; i < count; ++i) {
        r -= max(uInteriorStyleWeights[row + i], 0.0);
        if (r < 0.0) return i;
    }
    return count - 1;
}

// w: pane-local hit (m), rd: pane-space ray (z < 0 into the building), lit: occupancy (0/1),
// sill: floor distance below the pane's bottom edge (m).
NoxIMRoomSample NoxIMAtlasRoom(vec3 id, vec2 w, vec3 rd, vec2 paneHalf, float sill, float depthBase, float lit,
                               float style, float footprint, float camDist) {
    NoxIMRoomSample S;
    int room = NoxIMPickRoom(id, style);

    // Room box in pane space: x in [-W, W], y in [floorY, ceilY], z in [-D, 0]. Proportions stay
    // close to square walls so baked furniture is not visibly stretched.
    float W = paneHalf.x + mix(1.0, 1.4, NoxIMRand(id, 5.0));
    float floorY = -(paneHalf.y + sill * mix(0.95, 1.08, NoxIMRand(id, 6.0)));
    float ceilY = floorY + mix(3.4, 3.8, NoxIMRand(id, 8.0));
    float D = depthBase * (0.9 + 0.2 * NoxIMRand(id, 7.0));

    float tx = (rd.x > 0.0 ? (W - w.x) : (-W - w.x)) / rd.x;
    float ty = (rd.y > 0.0 ? (ceilY - w.y) : (floorY - w.y)) / rd.y;
    float tz = -D / rd.z;
    float tHit = min(tz, min(tx, ty));
    vec3 hp = vec3(w, 0.0) + rd * tHit;

    // Normalised room coordinates (textures/window_rooms/README.md, "Room-space mapping").
    float X = (hp.x + W) / (2.0 * W);          // left to right
    float Y = (ceilY - hp.y) / (ceilY - floorY); // ceiling to floor
    float Tz = -hp.z / D;                      // front to back
    float H = ceilY - floorY;

    int face;
    vec2 fuv;
    float faceSize;
    float cosI;
    if (tz <= min(tx, ty)) {
        face = NOX_FACE_BACK;  fuv = vec2(X, Y);           faceSize = max(2.0 * W, H); cosI = abs(rd.z);
    } else if (tx <= ty) {
        if (rd.x < 0.0) { face = NOX_FACE_LEFT;  fuv = vec2(Tz, Y); }
        else            { face = NOX_FACE_RIGHT; fuv = vec2(1.0 - Tz, Y); }
        faceSize = max(D, H); cosI = abs(rd.x);
    } else {
        if (rd.y > 0.0) { face = NOX_FACE_CEILING; fuv = vec2(X, Tz); }
        else            { face = NOX_FACE_FLOOR;   fuv = vec2(X, 1.0 - Tz); }
        faceSize = max(2.0 * W, D); cosI = abs(rd.y);
    }
    float texel = 0.5 / uInteriorFaceSize;
    fuv = clamp(fuv, vec2(texel), vec2(1.0 - texel));

    // Explicit LOD: screen derivatives jump at face seams, so the footprint is propagated instead.
    // The pane footprint grows with the distance travelled behind the glass and the incidence angle.
    float fp = footprint * (camDist + tHit) / max(camDist, 1e-3) / max(cosI, 0.15);
    float lod = log2(max(fp * uInteriorFaceSize / faceSize, 1e-4));
    int layer = room * NOX_ROOM_FACES + face;
    vec3 c = textureLod(uInteriorAtlas, vec3(fuv, float(layer)), lod).rgb;

    // Brightness: the bake is the room's radiance; scale it once. Unoccupied rooms stay nearly dark
    // unless the room's light data says it is often on anyway (e.g. a TV-lit room).
    vec3 roomLight = uInteriorRoomLight[room];
    float bright = lit > 0.5 ? roomLight.x * mix(0.75, 1.0, NoxIMRand(id, 41.0))
                             : (NoxIMRand(id, 42.0) < roomLight.y ? roomLight.z : 0.03);

    int base = room * NOX_ROOM_FACES;
    vec3 mean = uInteriorFaceMean[base + NOX_FACE_BACK] * 0.4
        + (uInteriorFaceMean[base + NOX_FACE_LEFT] + uInteriorFaceMean[base + NOX_FACE_RIGHT]) * 0.15
        + (uInteriorFaceMean[base + NOX_FACE_CEILING] + uInteriorFaceMean[base + NOX_FACE_FLOOR]) * 0.15;
    float light = dot(uInteriorFaceMean[base + NOX_FACE_BACK], vec3(0.2126, 0.7152, 0.0722)) * bright * 2.5;

    S.color = c * bright;
    S.average = mean * bright;
    S.blindLit = light;
    S.curtLit = 0.08 + 0.45 * min(light, 1.5);
    return S;
}

// ------------------------------------------------------------------------------------------------
// Procedural rooms (fallback when the atlas pack is not available)
// ------------------------------------------------------------------------------------------------

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

NoxIMRoomSample NoxIMProceduralRoom(vec3 id, vec2 w, vec3 rd, vec2 paneHalf, float depthBase, float lit,
                                    float style, float footprint, float timeS) {
    NoxIMRoomSample S;
    float shop = step(0.5, style) * (1.0 - step(1.5, style));
    float detail = 1.0 - smoothstep(0.012, 0.04, footprint);

    // Room layout (half extents about the pane centre; floor sits near the sill).
    float roomW = 1.6 + 1.2 * NoxIMRand(id, 5.0);
    float roomH = 1.7 + 0.2 * NoxIMRand(id, 6.0);
    float roomD = depthBase * (0.75 + 0.5 * NoxIMRand(id, 7.0));

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
    float tBoxExit;
    float tBox = NoxIMBox(vec3(w, 0.0), rd, fMin, fMax, tBoxExit);
    tBox = (fHave > 0.5 && tBox > 0.0) ? tBox : 1e9;

    // Person silhouette standing in lit rooms (dropped at distance, where it would only alias).
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

    float tbz = -0.08 / rd.z;
    vec2 wb = w + rd.xy * tbz;
    S.color = col;
    S.average = vec3(0.5, 0.45, 0.4) * (0.04 + 0.3 * lit);
    S.blindLit = NoxIMLight(R, vec3(wb, -0.3));
    S.curtLit = 0.08 + 0.45 * min(NoxIMLight(R, vec3(wb, -0.2)), 1.5);
    return S;
}

// ------------------------------------------------------------------------------------------------

// Radiance seen through one window pane.
//   worldPos   point on the pane (world)
//   geoN       pane normal (world, outward)
//   tangentW   pane tangent (world, image right); tangentSign is the glTF handedness
//   uv         TEXCOORD_0 of the window quad (atlas tile UV)
//   camPos     eye position (world)
//   paneHalf   pane half extents in metres (material interiorPane)
//   sill       floor distance below the pane's bottom edge in metres (material interiorSill)
//   depthBase  room depth in metres (material interiorDepth)
//   lightBias  occupancy bias, 0 = mostly dark rooms, 1 = mostly lit rooms
//   blindBias  probability of blinds and curtains
//   style      atlas variant weight row (scene data); procedural rooms draw shelf racks for style 1
//   seed       per-material variation
//   footprint  world size of one pixel (m); fine detail fades out beyond a few cm
//   timeS      seconds, for TV flicker (procedural rooms)
vec3 NoxInteriorWindow(vec3 worldPos, vec3 geoN, vec3 tangentW, float tangentSign, vec2 uv,
                       vec3 camPos, vec2 paneHalf, float sill, float depthBase, float lightBias, float blindBias,
                       float style, float seed, float footprint, float timeS) {
    vec3 N = normalize(geoN);
    vec3 T = normalize(tangentW - dot(tangentW, N) * N);
    vec3 B = normalize(cross(N, T) * tangentSign);

    // Pane-local coordinates: x right (-paneHalf.x..paneHalf.x), y up, metres.
    vec2 tileMin = floor(uv * 4.0) * 0.25 + vec2(0.004);
    vec2 local01 = vec2((uv.x - tileMin.x) / NOX_IM_TILE,
                        (tileMin.y + NOX_IM_TILE - uv.y) / NOX_IM_TILE);
    local01 = clamp(local01, vec2(0.0), vec2(1.0));
    vec2 w = (local01 - 0.5) * 2.0 * paneHalf;

    // Stable per-window id from the pane centre. Panes are >= 3 m apart, so 1 m cells (offset away
    // from the 0.1 m layout grid) give every window its own cell and keep the id stable frame to frame.
    vec3 paneCentre = worldPos - T * w.x - B * w.y;
    vec3 id = floor(paneCentre + 0.37) + vec3(seed * 7.31, seed * 3.17, seed * 5.53);

    // Ray into the room in pane space: x along T, y along B, z along the outward normal.
    // Viewed from the front, rd.z < 0 means the ray travels into the building.
    vec3 toPane = worldPos - camPos;
    float camDist = length(toPane);
    vec3 dir = toPane / max(camDist, 1e-4);
    vec3 rd = vec3(dot(dir, T), dot(dir, B), dot(dir, N));
    rd.z = min(rd.z, -1e-3);
    if (abs(rd.x) < 1e-4) rd.x = (rd.x < 0.0) ? -1e-4 : 1e-4;
    if (abs(rd.y) < 1e-4) rd.y = (rd.y < 0.0) ? -1e-4 : 1e-4;

    float detail = 1.0 - smoothstep(0.012, 0.04, footprint);
    // Occupancy: lit rooms are a minority unless lightBias is high.
    float lit = step(NoxIMRand(id, 1.0), mix(0.08, 0.7, lightBias));

    NoxIMRoomSample S = (uInteriorAtlasReady && uInteriorRoomCount > 0)
        ? NoxIMAtlasRoom(id, w, rd, paneHalf, sill, depthBase, lit, style, footprint, camDist)
        : NoxIMProceduralRoom(id, w, rd, paneHalf, depthBase, lit, style, footprint, timeS);
    vec3 col = S.color;

    // Blinds: a plane just behind the glass, drawn down from the top, with slats.
    float tbz = -0.08 / rd.z;
    vec2 wb = w + rd.xy * tbz;
    float blindOn = step(NoxIMRand(id, 25.0), blindBias);
    float cover = mix(0.25, 1.0, NoxIMRand(id, 26.0));
    float covered = step(paneHalf.y - 2.0 * paneHalf.y * cover, wb.y);
    float slat = fract(wb.y / 0.09);
    float gap = mix(0.15, 0.4, NoxIMRand(id, 27.0));
    float aa = smoothstep(0.004, 0.02, footprint);
    float blindMask = blindOn * covered * mix(1.0 - step(slat, gap), 0.6, aa);
    vec3 blindCol = mix(vec3(0.85, 0.8, 0.7), vec3(0.65, 0.68, 0.72), NoxIMRand(id, 28.0));
    blindCol *= 0.05 + 0.6 * min(S.blindLit, 1.5);
    col = mix(col, blindCol, blindMask);

    // Curtains: fabric at one or both pane edges, partly drawn back (open middle).
    float curtOn = step(NoxIMRand(id, 29.0), 0.2 + 0.6 * blindBias);
    float cw = mix(0.16, 0.42, NoxIMRand(id, 30.0));
    float cutL = step(wb.x, -paneHalf.x + cw) * step(0.35, NoxIMRand(id, 31.0));
    float cutR = step(paneHalf.x - cw, wb.x) * step(0.35, NoxIMRand(id, 32.0));
    float curtMask = curtOn * max(cutL, cutR);
    vec3 curtAlb = mix(vec3(0.45, 0.14, 0.1), vec3(0.2, 0.3, 0.32), NoxIMRand(id, 33.0));
    float fold = mix(0.9, 0.8 + 0.2 * sin(wb.x * 55.0), detail);
    col = mix(col, curtAlb * fold * S.curtLit, curtMask);

    // Sash frame and central mullion sit in the glass plane.
    float edge = min(paneHalf.x - abs(w.x), paneHalf.y - abs(w.y));
    float frame = 1.0 - smoothstep(0.025, 0.04, edge);
    float mullion = 1.0 - smoothstep(0.012, 0.022, abs(w.x));
    col *= 1.0 - 0.8 * max(frame, 0.8 * mullion);

    // Far away the room averages out to a soft glow instead of aliasing.
    col = mix(S.average, col, 0.4 + 0.6 * detail);

    return max(col, vec3(0.0));
}

#endif // NOX_INTERIOR_MAPPING_GLSL
