"""Generate the Shuto facade style texture sets (deterministic, PIL + numpy).

Each style is one repeating texture covering NB bays x NS storeys of one window design:
  textures/shuto/derived/facade_styles/<name>_base.jpg   wall colour + window glass/frames (sRGB)
  textures/shuto/derived/facade_styles/<name>_emit.jpg   per-window lit state (black = unlit)
  textures/shuto/derived/facade_styles/manifest.json     bay/storey metres, grid, kind, lit rules

Wall colour comes from clean, window-free regions of the supplied patches (facades_* cells,
concrete_cast, concrete_weathered, corrugated_metal). The crop is made seamless by mirroring
(A | flip(A) in both axes), so the tile wraps without a visible seam. Windows, frames, sills and
glass are drawn procedurally on top. Lit windows: 20-30 % of windows (warm 70 %, cool 30 %),
a share of them partly lit (blinds, dimmer), and a few dark storeys.

Run: python -I tools/shuto/make_facade_styles.py
"""
import json
import os

import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DERIVED = os.path.join(ROOT, "textures", "shuto", "derived")
OUT = os.path.join(DERIVED, "facade_styles")

SIZE = 2048       # texture size (px), covers NB x NS windows
NB, NS = 16, 12   # bays and storeys per texture
CROP = 256        # clean wall crop (px) before mirroring
PAD = 8
CONTENT = 627
ROOMS_DIR = os.path.join(ROOT, "textures", "window_rooms")
BLIND_FRAC = 0.3      # share of lit panes with drawn blinds (partly covered)
EMIT_LEVEL = 0.9      # emissive level of room content (the build multiplies by the material strength)
DARK_BASE = 0.22      # base colour of a lit pane relative to its content (glass in shadow)
LIT_FRAC = 0.25
WARM_FRAC = 0.7
PARTIAL_FRAC = 0.1
DARK_STOREY_FRAC = 0.05
SEED = 20021231

WARM = (1.0, 0.78, 0.46)
COOL = (0.72, 0.86, 1.0)

# name: wall source, window kind, bay m, storey m, glass tint (sRGB), frame (sRGB), lit probability
STYLES = [
    ("spandrel_midrise", ("facades_commercial", "gray_brown_office"), "spandrel", 4.0, 3.6, (0.07, 0.09, 0.12), (0.55, 0.56, 0.58), 0.25),
    ("office_ribbon", ("facades_commercial", "vertical_windows"), "ribbon", 4.0, 3.4, (0.06, 0.09, 0.13), (0.62, 0.63, 0.65), 0.25),
    ("curtain_mullion", ("concrete_cast", None), "curtain", 3.0, 3.6, (0.04, 0.08, 0.14), (0.45, 0.47, 0.50), 0.22),
    ("curtain_bronze", ("concrete_cast", None), "curtain", 3.2, 3.6, (0.09, 0.08, 0.06), (0.52, 0.45, 0.35), 0.22),
    ("apartment_balcony", ("facades_apartment", "beige_tiled"), "balcony", 3.5, 3.2, (0.06, 0.08, 0.10), (0.70, 0.68, 0.62), 0.26),
    ("apartment_concrete", ("facades_apartment", "gray_concrete"), "punched", 3.5, 3.2, (0.06, 0.07, 0.09), (0.40, 0.40, 0.42), 0.24),
    ("tile_older", ("facades_apartment", "tan_older"), "punched", 3.0, 3.3, (0.07, 0.08, 0.09), (0.66, 0.62, 0.55), 0.22),
    ("ivory_modern", ("facades_apartment", "ivory_modern"), "ribbon", 3.5, 3.3, (0.06, 0.08, 0.11), (0.60, 0.60, 0.58), 0.24),
    ("warehouse_clerestory", ("corrugated", None), "clerestory", 4.0, 3.6, (0.05, 0.07, 0.09), (0.35, 0.36, 0.38), 0.20),
    ("warehouse_roller", ("concrete_weathered", None), "roller", 4.0, 3.6, (0.05, 0.06, 0.08), (0.40, 0.40, 0.40), 0.20),
    ("shopfront_retail", ("facades_commercial", "retail_blank_signs"), "shopfront", 4.0, 3.6, (0.04, 0.06, 0.08), (0.30, 0.30, 0.32), 0.60),
    ("dark_office", ("concrete_weathered", None), "ribbon", 3.6, 3.4, (0.05, 0.08, 0.12), (0.45, 0.46, 0.48), 0.25),
]


def load_cell(family, cell):
    im = Image.open(os.path.join(DERIVED, "facades_%s_%s.png" % (family, cell))).convert("RGB")
    a = np.asarray(im).astype(np.float32) / 255.0
    return a[PAD:PAD + CONTENT, PAD:PAD + CONTENT]


def load_plain(name):
    im = Image.open(os.path.join(DERIVED, name)).convert("RGB")
    return np.asarray(im).astype(np.float32) / 255.0


def source_for(src):
    if src[0] == "corrugated":
        return load_plain("corrugated_metal_seamless.png")
    if src[1] is None:
        return load_plain("%s.png" % src[0]) if src[0] != "concrete_weathered" else load_plain("concrete_weathered.png")
    return load_cell(src[0].replace("facades_", ""), src[1])


def otsu(lum):
    hist, _ = np.histogram(lum, bins=256, range=(0.0, 1.0))
    p = hist / hist.sum()
    w = np.cumsum(p)
    mu = np.cumsum(p * np.arange(256)) / 256.0
    sb = (mu[-1] * w - mu) ** 2 / np.maximum(w * (1 - w), 1e-9)
    return (np.argmax(sb) + 0.5) / 256.0


def clean_crop(src):
    """The window-free CROP x CROP region with the least glass (and a usable mean tone)."""
    lum = 0.299 * src[..., 0] + 0.587 * src[..., 1] + 0.114 * src[..., 2]
    glass = ndimage.binary_opening(lum < otsu(lum), structure=np.ones((5, 5), dtype=bool)).astype(np.float32)
    best = None
    H, W = glass.shape
    ii = glass.cumsum(0).cumsum(1)
    for y in range(0, H - CROP + 1, 16):
        for x in range(0, W - CROP + 1, 16):
            s = ii[y + CROP - 1, x + CROP - 1]
            if y: s -= ii[y - 1, x + CROP - 1]
            if x: s -= ii[y + CROP - 1, x - 1]
            if y and x: s += ii[y - 1, x - 1]
            frac = s / float(CROP * CROP)
            mean = src[y:y + CROP, x:x + CROP].mean()
            if mean < 0.12 or mean > 0.9:
                continue
            if best is None or frac < best[0]:
                best = (frac, y, x)
    if best is None:
        return src[:CROP, :CROP]
    _, y, x = best
    return src[y:y + CROP, x:x + CROP]


def seamless_wall(src, target_mean):
    """Mirror-tile the clean crop to SIZE px (A | flip(A) in both axes is seamless), tone-matched."""
    c = clean_crop(src)
    top = np.concatenate([c, c[:, ::-1]], axis=1)
    block = np.concatenate([top, top[::-1]], axis=0)  # 2*CROP square, seamless
    reps = SIZE // block.shape[0]
    wall = np.tile(block, (reps, reps, 1))
    m = wall.mean()
    wall = np.clip(wall * min(1.0, target_mean / max(m, 1e-3)), 0, 1)
    return wall.astype(np.float32)


def cell_rect(i, j, nb, ns):
    """Pixel rectangle (x0, y0, x1, y1) of bay i, storey j from the BOTTOM (image rows run top-down)."""
    x0 = int(round(i * SIZE / nb))
    x1 = int(round((i + 1) * SIZE / nb))
    y1 = int(round(SIZE - j * SIZE / ns))
    y0 = int(round(SIZE - (j + 1) * SIZE / ns))
    return x0, y0, x1, y1


def frac_rect(x0, y0, x1, y1, fx0, fx1, fy0, fy1):
    """Fractional sub-rectangle inside a cell; fy measured from the cell bottom (0 = sill)."""
    w, h = x1 - x0, y1 - y0
    return (x0 + int(round(fx0 * w)), y1 - int(round(fy1 * h)), x0 + int(round(fx1 * w)), y1 - int(round(fy0 * h)))


def fill(img, rect, color):
    x0, y0, x1, y1 = rect
    img[y0:y1, x0:x1] = color


def load_rooms():
    """Back-wall faces of the window_rooms atlases (pixel_rect, exclusive bounds), desaturated 30 %."""
    man = json.load(open(os.path.join(ROOMS_DIR, "atlas_manifest.json"), encoding="utf-8"))
    rooms = []
    for a in man["assets"]:
        x0, y0, x1, y1 = a["faces"]["back"]["pixel_rect"]
        im = np.asarray(Image.open(os.path.join(ROOMS_DIR, a["file"])).convert("RGB")).astype(np.float32) / 255.0
        face = im[y0:y1, x0:x1]
        lum = (0.299 * face[..., 0] + 0.587 * face[..., 1] + 0.114 * face[..., 2])[..., None]
        face = face * 0.7 + lum * 0.3
        rooms.append((a["file"], face, "tv" in a["file"]))
    return rooms


def pane_content(rooms, rng, w, h):
    """A room crop (random scale and position, optional mirror) resized to the pane, and a tint."""
    # Uniform over the rooms; the dim TV room is only one of six, so cool blue stays a minority.
    file, face, is_tv = rooms[rng.integers(len(rooms))]
    H, W = face.shape[:2]
    s_ = rng.uniform(0.55, 1.0)
    cw, ch = max(8, int(W * s_)), max(8, int(H * s_))
    x0 = int(rng.integers(0, W - cw + 1))
    y0 = int(rng.integers(0, H - ch + 1))
    crop = face[y0:y0 + ch, x0:x0 + cw]
    im = Image.fromarray((np.clip(crop, 0, 1) * 255).astype(np.uint8)).resize((max(2, w), max(2, h)), Image.BILINEAR)
    arr = np.asarray(im).astype(np.float32) / 255.0
    if rng.random() < 0.5:
        arr = arr[:, ::-1]
    if is_tv:
        tint = np.array([0.55, 0.72, 1.0], dtype=np.float32)
    elif rng.random() < 0.7:
        tint = np.array([1.0, 0.84, 0.62], dtype=np.float32)   # warm
    else:
        tint = np.array([0.78, 0.9, 1.0], dtype=np.float32)    # cool
    return arr, tint


def blinds(arr, rng):
    """Partly drawn blinds: horizontal slats over the top part of the pane (darker stripes)."""
    h = arr.shape[0]
    out = arr.copy()
    cover = rng.uniform(0.3, 0.85)
    slat = max(3, h // 9)
    top = int(h * cover)
    for y in range(0, top, slat):
        out[y:y + max(1, slat // 2)] *= 0.35
    return out


def draw_style(name, kind, wall, glass, frame, lit_p, rng, rooms):
    base = wall.copy()
    emit = np.zeros((SIZE, SIZE, 3), dtype=np.float32)
    glass = np.array(glass, dtype=np.float32)
    frame = np.array(frame, dtype=np.float32)
    dark_storeys = set(rng.choice(NS, size=max(1, int(round(NS * DARK_STOREY_FRAC * 4))), replace=False).tolist())
    for j in range(NS):
        for i in range(NB):
            x0, y0, x1, y1 = cell_rect(i, j, NB, NS)
            if kind == "punched":
                g = frac_rect(x0, y0, x1, y1, 0.22, 0.78, 0.30, 0.80)
            elif kind == "ribbon":
                g = frac_rect(x0, y0, x1, y1, 0.05, 0.95, 0.35, 0.80)
            elif kind == "curtain":
                g = frac_rect(x0, y0, x1, y1, 0.02, 0.98, 0.03, 0.97)
            elif kind == "spandrel":
                g = frac_rect(x0, y0, x1, y1, 0.08, 0.92, 0.32, 0.80)
            elif kind == "balcony":
                g = frac_rect(x0, y0, x1, y1, 0.20, 0.80, 0.30, 0.72)
            elif kind == "clerestory":
                g = frac_rect(x0, y0, x1, y1, 0.04, 0.96, 0.70, 0.92)
            elif kind == "roller":
                g = frac_rect(x0, y0, x1, y1, 0.22, 0.78, 0.30, 0.80)
            elif kind == "shopfront":
                g = frac_rect(x0, y0, x1, y1, 0.04, 0.96, 0.10, 0.88)
            else:
                g = frac_rect(x0, y0, x1, y1, 0.22, 0.78, 0.30, 0.80)
            gx0, gy0, gx1, gy1 = g
            if kind == "roller" and j == 0 and i % 2 == 0:
                # Roller door on alternate ground-floor bays: grey panel with horizontal grooves.
                door = frac_rect(x0, y0, x1, y1, 0.08, 0.92, 0.0, 0.78)
                dx0, dy0, dx1, dy1 = door
                base[dy0:dy1, dx0:dx1] = np.array([0.42, 0.43, 0.44], dtype=np.float32)
                for gy in range(dy0, dy1, 9):
                    base[gy:gy + 2, dx0:dx1] *= 0.7
                continue
            if kind == "shopfront" and j == 0:
                # Ground floor: large glazing, mullions at bay edges, a sign band across the top.
                fill(base, (x0, y0, x1, y0 + int(0.10 * (y1 - y0))), frame * 0.5)
                cell = frac_rect(x0, y0, x1, y1, 0.04, 0.96, 0.10, 0.88)
            fill(base, (gx0 - 2, gy0 - 2, gx1 + 2, gy1 + 2), frame)
            # glass: dark, slightly varied tint, faint reflection band
            tint = glass * (1.0 + rng.uniform(-0.25, 0.25))
            glass_rgb = np.broadcast_to(tint, (gy1 - gy0, gx1 - gx0, 3)).copy()
            grad = np.linspace(0.05, 0.0, gy1 - gy0)[:, None, None]
            glass_rgb = glass_rgb + grad
            if kind == "ribbon" or kind == "spandrel":
                mid = (gx0 + gx1) // 2
                glass_rgb[:, max(0, mid - gx0 - 1):max(0, mid - gx0 + 1)] = frame * 0.8
            if kind == "curtain":
                mid = (gy0 + gy1) // 2
                glass_rgb[max(0, mid - gy0 - 1):max(0, mid - gy0 + 1), :] = frame * 0.8
            base[gy0:gy1, gx0:gx1] = glass_rgb
            if kind == "spandrel":
                fill(base, (x0 + 2, y1 - int(0.26 * (y1 - y0)), x1 - 2, y1 - 2), wall[y0:y1, x0:x1].mean(axis=(0, 1)) * 0.55)
            if kind == "balcony":
                rail = frac_rect(x0, y0, x1, y1, 0.0, 1.0, 0.72, 0.76)
                fill(base, rail, frame * 0.6)
            if kind == "clerestory":
                fill(base, frac_rect(x0, y0, x1, y1, 0.0, 1.0, 0.0, 0.66), wall[y0:y1, x0:x1].mean(axis=(0, 1)) * 0.7)
            # lit state
            if j in dark_storeys:
                continue
            r = rng.random()
            if r < lit_p:
                content, tint = pane_content(rooms, rng, gx1 - gx0, gy1 - gy0)
                if rng.random() < BLIND_FRAC:
                    content = blinds(content, rng)
                emit[gy0:gy1, gx0:gx1] = content * tint * EMIT_LEVEL
                base[gy0:gy1, gx0:gx1] = content * tint * DARK_BASE
    return base, emit


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = np.random.default_rng(SEED)
    corr = load_plain("corrugated_metal_seamless.png")
    conc_w = load_plain("concrete_weathered.png")
    conc_c = load_plain("concrete_cast.png")
    rooms = load_rooms()
    manifest = {"generator": "tools/shuto/make_facade_styles.py", "size_px": SIZE, "bays": NB, "storeys": NS,
                "wall_source_note": "clean window-free crops, mirror-seamless; windows drawn procedurally",
                "styles": []}
    for name, src, kind, bay_m, storey_m, glass, frame, lit_p in STYLES:
        if src[0] == "corrugated":
            wall_src = corr
        elif src[0] == "concrete_cast":
            wall_src = conc_c
        elif src[0] == "concrete_weathered":
            wall_src = conc_w
        else:
            wall_src = source_for(src)
        target = 0.36 if kind != "curtain" else 0.22
        wall = seamless_wall(wall_src, target)
        base, emit = draw_style(name, kind, wall, glass, frame, lit_p, rng, rooms)
        Image.fromarray((np.clip(base, 0, 1) * 255 + 0.5).astype(np.uint8)).save(
            os.path.join(OUT, "%s_base.jpg" % name), quality=88)
        Image.fromarray((np.clip(emit, 0, 1) * 255 + 0.5).astype(np.uint8)).save(
            os.path.join(OUT, "%s_emit.jpg" % name), quality=88)
        lit_cells = float((emit.sum(axis=2) > 0).mean())
        manifest["styles"].append({"name": name, "kind": kind, "bay_m": bay_m, "storey_m": storey_m,
                                   "base": "%s_base.jpg" % name, "emit": "%s_emit.jpg" % name,
                                   "wall_source": list(src), "lit_probability": lit_p,
                                   "emissive_area_fraction": round(lit_cells, 4)})
        print(name, "lit area %.3f" % lit_cells)
    # Shopfront ground storeys reuse the shopfront style; no separate file.
    with open(os.path.join(OUT, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)


if __name__ == "__main__":
    main()
