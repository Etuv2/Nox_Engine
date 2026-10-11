#ifndef NOX_INTERIOR_MAPPING_GLSL
#define NOX_INTERIOR_MAPPING_GLSL

// Window interior mapping: a parallax room box behind each window pane.
//
// Pane contract (all per material, with per-atlas defaults; nothing here assumes a pane size):
//   * The pane spans 2 * paneHalf metres along the tangent (image right) and the derived bitangent
//     cross(N, T) * tangent.w (image up). paneHalf <= 0 derives the size from the UV derivatives.
//   * Pane UV layout (grid, inset, extent): the pane occupies [cell/grid + inset, + extent] of a
//     grid x grid tiling of TEXCOORD_0, v growing downwards (glTF). grid <= 0 on the material uses
//     the atlas default (uInteriorPaneUV); grid <= 0 there too infers the layout, assuming the pane
//     is centred in its cell (a pane spanning the whole 0..1 UV square is grid 1).
//   The pane centre follows from the world position, so each window gets a stable id without
//   extra vertex data.
//
// Room source:
//   * Interior atlas (SceneRenderExtras::interiorAtlas, bound by GBufferPass): N rooms of F face
//     layers each (F and the face roles come from the atlas data). One texture-array layer per face,
//     so mip sampling never bleeds between faces. Baked rooms carry their own lighting.
//     uInteriorRoomData holds per-room light, the per-room mean radiance and one cumulative variant
//     weight row per style; the material style selects the row (clamped to the atlas row count).
//   * No atlas: a plain, neutral lit box (no furniture, no figures, no animation).
// Blinds, curtains, the sash frame and the mullion stay in the foreground for both sources and
// are controlled by the material's blinds probability (0 = none).
//
// Cheap path: interiorMode 2 (material "flat") or camera distance beyond the far distance shades
// the pane with the room's mean radiance (no ray-box, no atlas sample).
//
// Output is relative room radiance (about 0..1 for a fully lit room). The caller multiplies it by
// emissiveFactor and interiorStrength; the material emissiveStrength (cd/m^2) is applied by lighting.
// The glass itself is left to the material as a smooth dielectric.

// Face roles (InteriorAtlas::FaceRole).
const int NOX_ROLE_BACK = 0;
const int NOX_ROLE_LEFT = 1;
const int NOX_ROLE_RIGHT = 2;
const int NOX_ROLE_CEILING = 3;
const int NOX_ROLE_FLOOR = 4;
const int NOX_ROLE_COUNT = 5;

uniform sampler2DArray uInteriorAtlas;
uniform sampler2D uInteriorRoomData;   // RGBA32F, x = room: row 0 light, row 1 mean radiance (rgb) + back-wall luminance (a), row 2+s style s CDF (r)
uniform bool uInteriorAtlasReady = false;
uniform int uInteriorRoomCount = 0;
uniform int uInteriorStyleCount = 1;
uniform int uInteriorFacesPerRoom = 0;
uniform int uInteriorRoleSlot[NOX_ROLE_COUNT];               // face slot per role, -1 = not in the atlas
uniform float uInteriorFaceSize = 512.0;                     // texels per face layer edge
uniform vec4 uInteriorRoomShape = vec4(1.0, 1.4, 2.8, 3.2);  // side margin min/max, floor-to-ceiling min/max (m)
uniform vec3 uInteriorRoomJitter = vec3(0.1, 1.0, 1.0);      // depth jitter (fraction), sill scale min/max
uniform vec3 uInteriorPaneUV = vec3(0.0);                    // default pane UV layout: grid, inset, extent (grid 0 = infer)
uniform float uInteriorFarDistance = 0.0;                    // default flat-path distance (m), 0 = never

float NoxIMHash(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float NoxIMRand(vec3 id, float k) {
    return NoxIMHash(id + vec3(k, k * 0.731, k * 1.913));
}

// What the foreground layers need from either room source.
struct NoxIMRoomSample {
    vec3 color;      // room radiance through the pane
    vec3 average;    // stable distant average (no aliasing)
    float blindLit;  // light level on blinds just behind the glass
    float curtLit;   // light level on curtains
};

// Ray-box exit of the room seen through the pane.
struct NoxIMHit {
    int role;        // NOX_ROLE_*
    vec2 fuv;        // face UV, textures/window_rooms/README.md "Room-space mapping"
    float tHit;      // distance behind the glass along the ray
    float faceSize;  // metres covered by the face's longer edge
    float cosI;      // incidence cosine on the face
    float depth01;   // 0 at the glass, 1 at the back wall
};

// Room box in pane space: x in [-W, W], y in [floorY, ceilY], z in [-D, 0].
NoxIMHit NoxIMTraceRoom(vec3 id, vec2 w, vec3 rd, vec2 paneHalf, float sill, float depthBase) {
    float W = paneHalf.x + mix(uInteriorRoomShape.x, uInteriorRoomShape.y, NoxIMRand(id, 5.0));
    float floorY = -(paneHalf.y + sill * mix(uInteriorRoomJitter.y, uInteriorRoomJitter.z, NoxIMRand(id, 6.0)));
    float ceilY = floorY + mix(uInteriorRoomShape.z, uInteriorRoomShape.w, NoxIMRand(id, 8.0));
    float D = depthBase * (1.0 + uInteriorRoomJitter.x * (2.0 * NoxIMRand(id, 7.0) - 1.0));

    float tx = (rd.x > 0.0 ? (W - w.x) : (-W - w.x)) / rd.x;
    float ty = (rd.y > 0.0 ? (ceilY - w.y) : (floorY - w.y)) / rd.y;
    float tz = -D / rd.z;
    NoxIMHit h;
    h.tHit = min(tz, min(tx, ty));
    vec3 hp = vec3(w, 0.0) + rd * h.tHit;

    float X = (hp.x + W) / (2.0 * W);             // left to right
    float Y = (ceilY - hp.y) / (ceilY - floorY);  // ceiling to floor
    float T = -hp.z / D;                          // front to back
    float H = ceilY - floorY;
    h.depth01 = clamp(T, 0.0, 1.0);
    if (tz <= min(tx, ty)) {
        h.role = NOX_ROLE_BACK;  h.fuv = vec2(X, Y); h.faceSize = max(2.0 * W, H); h.cosI = abs(rd.z);
    } else if (tx <= ty) {
        if (rd.x < 0.0) { h.role = NOX_ROLE_LEFT;  h.fuv = vec2(T, Y); }
        else            { h.role = NOX_ROLE_RIGHT; h.fuv = vec2(1.0 - T, Y); }
        h.faceSize = max(D, H); h.cosI = abs(rd.x);
    } else {
        if (rd.y > 0.0) { h.role = NOX_ROLE_CEILING; h.fuv = vec2(X, T); }
        else            { h.role = NOX_ROLE_FLOOR;   h.fuv = vec2(X, 1.0 - T); }
        h.faceSize = max(2.0 * W, D); h.cosI = abs(rd.y);
    }
    return h;
}

// ------------------------------------------------------------------------------------------------
// Atlas rooms
// ------------------------------------------------------------------------------------------------

// Weighted variant choice: first room whose cumulative weight exceeds a stable per-window random.
int NoxIMPickRoom(vec3 id, float style) {
    int count = max(uInteriorRoomCount, 1);
    int row = 2 + clamp(int(style + 0.5), 0, max(uInteriorStyleCount, 1) - 1);
    float r = NoxIMRand(id, 40.0);
    int lo = 0;
    int hi = count - 1;
    for (int k = 0; k < 10 && lo < hi; ++k) {   // 2^10 > InteriorAtlas::kMaxRooms
        int mid = (lo + hi) / 2;
        if (r < texelFetch(uInteriorRoomData, ivec2(mid, row), 0).r) hi = mid; else lo = mid + 1;
    }
    return lo;
}

// Brightness of the room: the bake is the room's radiance; scale it once. Unoccupied rooms stay
// nearly dark unless the room's light data says it is often on anyway (e.g. a TV-lit room).
float NoxIMAtlasBrightness(vec3 id, int room, float lit) {
    vec3 roomLight = texelFetch(uInteriorRoomData, ivec2(room, 0), 0).rgb;
    return lit > 0.5 ? roomLight.x * mix(0.75, 1.0, NoxIMRand(id, 41.0))
                     : (NoxIMRand(id, 42.0) < roomLight.y ? roomLight.z : 0.03);
}

// Mean radiance and blind lighting only (flat path).
NoxIMRoomSample NoxIMAtlasRoomFlat(vec3 id, float lit, float style) {
    int room = NoxIMPickRoom(id, style);
    float bright = NoxIMAtlasBrightness(id, room, lit);
    vec4 mean = texelFetch(uInteriorRoomData, ivec2(room, 1), 0);
    NoxIMRoomSample S;
    S.average = mean.rgb * bright;
    S.color = S.average;
    S.blindLit = mean.a * bright * 2.5;
    S.curtLit = 0.08 + 0.45 * min(S.blindLit, 1.5);
    return S;
}

// w: pane-local hit (m), rd: pane-space ray (z < 0 into the building), lit: occupancy (0/1),
// sill: floor distance below the pane's bottom edge (m).
NoxIMRoomSample NoxIMAtlasRoom(vec3 id, vec2 w, vec3 rd, vec2 paneHalf, float sill, float depthBase, float lit,
                               float style, float footprint, float camDist) {
    int room = NoxIMPickRoom(id, style);
    float bright = NoxIMAtlasBrightness(id, room, lit);
    vec4 mean = texelFetch(uInteriorRoomData, ivec2(room, 1), 0);
    NoxIMHit h = NoxIMTraceRoom(id, w, rd, paneHalf, sill, depthBase);

    vec3 c = mean.rgb;   // roles the atlas does not provide show the room's mean radiance
    int slot = uInteriorRoleSlot[h.role];
    if (slot >= 0) {
        float texel = 0.5 / uInteriorFaceSize;
        vec2 fuv = clamp(h.fuv, vec2(texel), vec2(1.0 - texel));
        // Explicit LOD: screen derivatives jump at face seams, so the footprint is propagated instead.
        // The pane footprint grows with the distance travelled behind the glass and the incidence angle.
        float fp = footprint * (camDist + h.tHit) / max(camDist, 1e-3) / max(h.cosI, 0.15);
        float lod = log2(max(fp * uInteriorFaceSize / h.faceSize, 1e-4));
        int layer = room * uInteriorFacesPerRoom + slot;
        c = textureLod(uInteriorAtlas, vec3(fuv, float(layer)), lod).rgb;
    }

    NoxIMRoomSample S;
    S.color = c * bright;
    S.average = mean.rgb * bright;
    S.blindLit = mean.a * bright * 2.5;
    S.curtLit = 0.08 + 0.45 * min(S.blindLit, 1.5);
    return S;
}

// ------------------------------------------------------------------------------------------------
// Neutral room (no atlas bound): plain lit box, one light level per face.
// ------------------------------------------------------------------------------------------------

NoxIMRoomSample NoxIMPlainRoom(vec3 id, vec2 w, vec3 rd, vec2 paneHalf, float sill, float depthBase, float lit,
                               bool flatOnly) {
    float bright = lit > 0.5 ? mix(0.75, 1.0, NoxIMRand(id, 41.0)) : 0.03;
    NoxIMRoomSample S;
    S.average = vec3(0.42) * bright;
    S.color = S.average;
    S.blindLit = 0.5 * bright;
    S.curtLit = 0.08 + 0.45 * min(S.blindLit, 1.5);
    if (flatOnly) return S;
    NoxIMHit h = NoxIMTraceRoom(id, w, rd, paneHalf, sill, depthBase);
    float shade = h.role == NOX_ROLE_BACK ? 0.5 : (h.role == NOX_ROLE_CEILING ? 0.62 : (h.role == NOX_ROLE_FLOOR ? 0.26 : 0.42));
    S.color = vec3(shade * mix(1.0, 0.75, h.depth01) * bright);
    return S;
}

// ------------------------------------------------------------------------------------------------

// Metres per unit of UV along u and v (planar quads with an affine UV mapping).
vec2 NoxIMMetresPerUV(vec3 worldPos, vec2 uv) {
    vec3 dpx = dFdx(worldPos);
    vec3 dpy = dFdy(worldPos);
    vec2 dux = dFdx(uv);
    vec2 duy = dFdy(uv);
    float det = dux.x * duy.y - dux.y * duy.x;
    if (abs(det) < 1e-14) return vec2(1.0);
    vec3 dPdu = (dpx * duy.y - dpy * dux.y) / det;
    vec3 dPdv = (dpy * dux.x - dpx * duy.x) / det;
    return vec2(length(dPdu), length(dPdv));
}

// Radiance seen through one window pane.
//   worldPos     point on the pane (world)
//   geoN         pane normal (world, outward)
//   tangentW     pane tangent (world, image right); tangentSign is the glTF handedness
//   uv           TEXCOORD_0 of the window quad
//   camPos       eye position (world)
//   paneHalf     pane half extents in metres (<= 0: derived from the UV derivatives)
//   paneUV       pane UV layout (grid, inset, extent); grid <= 0: atlas default; atlas grid <= 0:
//                inferred (pane centred in its cell, cell count from the pane's UV extent)
//   sill         floor distance below the pane's bottom edge in metres
//   depthBase    room depth in metres
//   lightBias    occupancy bias, 0 = mostly dark rooms, 1 = mostly lit rooms
//   blindBias    probability of blinds and curtains (0 = none)
//   style        atlas variant weight row
//   seed         per-material variation
//   mode         1 = parallax room, 2 = flat mean radiance only
//   farDistance  beyond this camera distance use the flat path (< 0: atlas default, 0: never)
//   footprint    world size of one pixel (m); fine detail fades out beyond a few cm
// Call in uniform control flow (UV derivatives).
vec3 NoxInteriorWindow(vec3 worldPos, vec3 geoN, vec3 tangentW, float tangentSign, vec2 uv,
                       vec3 camPos, vec2 paneHalf, vec3 paneUV, float sill, float depthBase, float lightBias,
                       float blindBias, float style, float seed, float mode, float farDistance, float footprint) {
    // Both branches depend on uniforms only, so the derivatives stay in uniform control flow.
    vec3 paneLayout = paneUV.x > 0.0 ? paneUV : uInteriorPaneUV;
    bool paneKnown = paneHalf.x > 0.0 && paneHalf.y > 0.0;
    if (paneLayout.x <= 0.0 || !paneKnown) {
        vec2 metresPerUV = NoxIMMetresPerUV(worldPos, uv);
        if (paneLayout.x <= 0.0) {
            float ext = paneKnown ? 2.0 * paneHalf.x / max(metresPerUV.x, 1e-6) : 1.0;
            float g = max(floor(1.0 / max(ext, 1e-3) + 0.5), 1.0);
            ext = min(ext, 1.0 / g);
            paneLayout = vec3(g, 0.5 * (1.0 / g - ext), ext);
        }
        if (!paneKnown) paneHalf = 0.5 * paneLayout.z * metresPerUV;
    }
    float grid = max(paneLayout.x, 1.0);
    float extent = max(paneLayout.z, 1e-4);

    vec3 N = normalize(geoN);
    vec3 T = normalize(tangentW - dot(tangentW, N) * N);
    vec3 B = normalize(cross(N, T) * tangentSign);

    // Pane-local coordinates: x right (-paneHalf.x..paneHalf.x), y up, metres. A single-cell layout
    // never repeats, so its pane edges (uv 0 and 1) cannot flip to a neighbouring cell.
    vec2 tileMin = (grid > 1.0 ? floor(uv * grid) / grid : vec2(0.0)) + vec2(paneLayout.y);
    vec2 local01 = vec2((uv.x - tileMin.x) / extent, (tileMin.y + extent - uv.y) / extent);
    local01 = clamp(local01, vec2(0.0), vec2(1.0));
    vec2 w = (local01 - 0.5) * 2.0 * paneHalf;

    // Stable per-window id from the pane centre. 1 m cells (offset away from round coordinates)
    // keep the id stable across the pane and frame to frame.
    vec3 paneCentre = worldPos - T * w.x - B * w.y;
    vec3 id = floor(paneCentre + 0.37) + vec3(seed * 7.31, seed * 3.17, seed * 5.53);

    // The room is built right-handed: x = cross(B, N), image right seen from outside. A mirrored UV
    // frame (tangent.w < 0, so T = -cross(B, N)) would otherwise show a mirrored room.
    float mirror = tangentSign < 0.0 ? -1.0 : 1.0;
    w.x *= mirror;

    vec3 toPane = worldPos - camPos;
    float camDist = length(toPane);
    float lit = step(NoxIMRand(id, 1.0), mix(0.08, 0.7, lightBias));
    bool atlas = uInteriorAtlasReady && uInteriorRoomCount > 0;

    // Blinds coverage (shared by both paths).
    float blindOn = step(NoxIMRand(id, 25.0), blindBias);
    float cover = mix(0.25, 1.0, NoxIMRand(id, 26.0));

    // Flat path: the material asks for it, or the pane is beyond the far distance.
    float far = farDistance >= 0.0 ? farDistance : uInteriorFarDistance;
    float flatMix = mode > 1.5 ? 1.0 : (far > 0.0 ? smoothstep(0.85 * far, far, camDist) : 0.0);
    if (flatMix >= 1.0) {
        NoxIMRoomSample F = atlas ? NoxIMAtlasRoomFlat(id, lit, style)
                                  : NoxIMPlainRoom(id, w, vec3(0.0, 0.0, -1.0), paneHalf, sill, depthBase, lit, true);
        return max(F.average * (1.0 - 0.6 * blindOn * cover), vec3(0.0));
    }

    // Ray into the room in pane space: x along T, y along B, z along the outward normal.
    // Viewed from the front, rd.z < 0 means the ray travels into the building.
    vec3 dir = toPane / max(camDist, 1e-4);
    vec3 rd = vec3(dot(dir, T) * mirror, dot(dir, B), dot(dir, N));
    rd.z = min(rd.z, -1e-3);
    if (abs(rd.x) < 1e-4) rd.x = (rd.x < 0.0) ? -1e-4 : 1e-4;
    if (abs(rd.y) < 1e-4) rd.y = (rd.y < 0.0) ? -1e-4 : 1e-4;

    float detail = 1.0 - smoothstep(0.012, 0.04, footprint);
    NoxIMRoomSample S = atlas
        ? NoxIMAtlasRoom(id, w, rd, paneHalf, sill, depthBase, lit, style, footprint, camDist)
        : NoxIMPlainRoom(id, w, rd, paneHalf, sill, depthBase, lit, false);
    vec3 col = S.color;

    // Blinds: a plane just behind the glass, drawn down from the top, with slats.
    float tbz = -0.08 / rd.z;
    vec2 wb = w + rd.xy * tbz;
    float covered = step(paneHalf.y - 2.0 * paneHalf.y * cover, wb.y);
    float slat = fract(wb.y / 0.09);
    float gap = mix(0.15, 0.4, NoxIMRand(id, 27.0));
    float aa = smoothstep(0.004, 0.02, footprint);
    float blindMask = blindOn * covered * mix(1.0 - step(slat, gap), 0.6, aa);
    vec3 blindCol = mix(vec3(0.85, 0.8, 0.7), vec3(0.65, 0.68, 0.72), NoxIMRand(id, 28.0));
    blindCol *= 0.05 + 0.6 * min(S.blindLit, 1.5);
    col = mix(col, blindCol, blindMask);

    // Curtains: fabric at one or both pane edges, partly drawn back (open middle).
    float curtOn = blindBias > 0.0 ? step(NoxIMRand(id, 29.0), 0.2 + 0.6 * blindBias) : 0.0;
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
    col = mix(col, S.average * (1.0 - 0.6 * blindOn * cover), flatMix);

    return max(col, vec3(0.0));
}

#endif // NOX_INTERIOR_MAPPING_GLSL
