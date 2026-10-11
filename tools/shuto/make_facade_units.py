"""Derive storey x bay facade units from the padded facade patches.

Input : textures/shuto/derived/facades_<family>_<cell>.png (627 px content + 8 px replicated padding,
        written by extract_patches.py). Source patches are shared, never repeated whole.
Output: textures/shuto/derived/facade_units/facades_<family>_units.png       albedo atlas (sRGB)
        textures/shuto/derived/facade_units/facades_<family>_units_lit.png   per-window emissive atlas
        textures/shuto/derived/facade_units/manifest.json                    unit rectangles, metres

A unit is one storey (STOREY m, the measured storey pitch of its cell, see storey_pitch) by one bay
(BAY m for the family). A building quad of storey x bay size takes one unit, so the mapping never
stretches more than the bay rounding in build_scenery. Units carry 8 px edge-replicated padding
(mip-safe to about mip 3) and are packed with no shared gutters.

Lit windows: dark glass connected components (Otsu threshold per cell) are lit one by one, warm or
cool, seeded per family, so lit windows are per window, not per block. Bottom-storey units (ground
floor, shopfronts) are lit more often than upper storeys.

Run: python -I tools/shuto/make_facade_units.py
"""
import json
import os

import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DERIVED = os.path.join(ROOT, "textures", "shuto", "derived")
OUT = os.path.join(DERIVED, "facade_units")

PAD = 8
CONTENT = 627
STOREY = 3.3
BAY = {"apartment": 3.5, "commercial": 4.0, "industrial": 6.0}
CELLS = {
    "apartment": ["beige_tiled", "gray_concrete", "tan_older", "ivory_modern"],
    "commercial": ["curtain_wall", "retail_blank_signs", "vertical_windows", "gray_brown_office"],
    "industrial": ["warehouse_roller_door", "concrete_clerestory", "red_container", "teal_container"],
}
COLS = 6              # atlas columns
FAR_VARIANTS = 3      # repeating far-LOD tiles per family
MAX_UPPER_ROWS = 3    # upper storey rows kept per cell
MAX_BAYS = 3          # bays kept per storey row
BASE_TARGET_SRGB = 0.40
LIT_UPPER = 0.16
LIT_GROUND = 0.45
COOL = 0.30
MIN_GLASS_AREA = 400
WEAK_PERIOD = 0.3
NOMINAL_PX_PER_M = 40.0
WARM = np.array([1.0, 0.80, 0.52], dtype=np.float32)
COOLC = np.array([0.72, 0.86, 1.0], dtype=np.float32)


def load_cell(family, cell):
    im = Image.open(os.path.join(DERIVED, "facades_%s_%s.png" % (family, cell))).convert("RGB")
    a = np.asarray(im).astype(np.float32) / 255.0
    return a[PAD:PAD + CONTENT, PAD:PAD + CONTENT]


def otsu(lum):
    hist, _ = np.histogram(lum, bins=256, range=(0.0, 1.0))
    p = hist / hist.sum()
    w = np.cumsum(p)
    mu = np.cumsum(p * np.arange(256)) / 256.0
    mt = mu[-1]
    sb = (mt * w - mu) ** 2 / np.maximum(w * (1 - w), 1e-9)
    return (np.argmax(sb) + 0.5) / 256.0


def storey_pitch(glass_rows):
    """Storey pitch in px: first strong autocorrelation peak of the row glass profile."""
    g = glass_rows - glass_rows.mean()
    ac = np.correlate(g, g, mode="full")[len(g) - 1:]
    ac = ac / max(ac[0], 1e-9)
    lo, hi = 80, 200
    lag = lo + int(np.argmax(ac[lo:hi]))
    return lag, float(ac[lag])


def cell_maps(family, cell, rng):
    """Returns (albedo content 627x627x3, lit content 627x627x3, rows, bay_px, info)."""
    bay_m = BAY[family]
    a = load_cell(family, cell)
    lum = 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]
    t = otsu(lum)
    # Opening breaks thin grout/joint lines so they cannot join into one large lit component.
    glass = ndimage.binary_opening(lum < t, structure=np.ones((5, 5), dtype=bool))
    g_rows = glass.mean(axis=1)
    pitch, ac = storey_pitch(g_rows)
    if ac < WEAK_PERIOD:
        # No clear window rhythm (containers, roller doors): use the nominal storey scale.
        pitch = int(round(STOREY * NOMINAL_PX_PER_M))
    px_per_m = pitch / STOREY
    qpx = min(CONTENT, int(round(bay_m * px_per_m)))
    # Storey phase: the phase whose boundary rows carry the least glass (spandrels, not sashes).
    best = min(range(pitch), key=lambda p0: float(sum(g_rows[y] for y in range(p0, CONTENT, pitch))))
    rows = [(y0, y0 + pitch) for y0 in range(best, CONTENT - pitch + 1, pitch)]
    labels, n = ndimage.label(glass)
    lit_rgb = np.zeros((CONTENT, CONTENT, 3), dtype=np.float32)
    ground = rows[-1] if rows else None
    if n:
        sizes = ndimage.sum(glass, labels, index=np.arange(1, n + 1))
        for lbl in np.nonzero(sizes >= MIN_GLASS_AREA)[0] + 1:
            ys, _ = np.nonzero(labels == lbl)
            is_ground = ground is not None and ys.mean() >= ground[0]
            if rng.random() < (LIT_GROUND if is_ground else LIT_UPPER):
                col = COOLC if rng.random() < COOL else WARM
                lit_rgb[labels == lbl] = col * 0.75
    info = {"cell": cell, "storey_px": pitch, "bay_px": qpx, "px_per_m": round(px_per_m, 3),
            "storey_autocorr": round(ac, 3), "glass_fraction": round(float(glass.mean()), 3),
            "otsu": round(float(t), 3), "phase_px": best, "rows": len(rows)}
    return a, lit_rgb, rows, qpx, info


def main():
    os.makedirs(OUT, exist_ok=True)
    manifest = {"generator": "tools/shuto/make_facade_units.py", "padding_px": PAD, "content_px": CONTENT,
                "storey_m": STOREY, "bay_m": BAY, "atlas_columns": COLS, "families": {}}
    for family in ("apartment", "commercial", "industrial"):
        rng = np.random.default_rng(sum(ord(c) for c in family))
        tiles = []   # (cell, ground, albedo crop, lit crop, rect in content px)
        cell_infos = []
        for cell in CELLS[family]:
            a, lit, rows, qpx, info = cell_maps(family, cell, rng)
            cell_infos.append(info)
            if not rows:
                continue
            sel = [(i, r) for i, r in enumerate(rows)]
            n_bays = max(1, min(MAX_BAYS, CONTENT // qpx))
            for i, (y0, y1) in sel:
                ground = i == len(rows) - 1
                if not ground and i < len(rows) - MAX_UPPER_ROWS:
                    continue
                for bi in range(n_bays):
                    x0 = bi * qpx
                    x1 = x0 + qpx
                    tiles.append((cell, ground, a[y0:y1, x0:x1], lit[y0:y1, x0:x1], (x0, y0, x1, y1)))
        bay_m = BAY[family]
        cw = max(t[2].shape[1] for t in tiles) + 2 * PAD
        ch = max(t[2].shape[0] for t in tiles) + 2 * PAD
        rows_n = int(np.ceil(len(tiles) / COLS))
        W, H = COLS * cw, rows_n * ch
        alb = np.zeros((H, W, 3), dtype=np.float32)
        emi = np.zeros((H, W, 3), dtype=np.float32)
        units = []
        for k, (cell, ground, ca, cl, rect) in enumerate(tiles):
            r, c = divmod(k, COLS)
            ox, oy = c * cw, r * ch
            h, w = ca.shape[:2]
            alb[oy:oy + h + 2 * PAD, ox:ox + w + 2 * PAD] = np.pad(ca, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="edge")
            emi[oy:oy + h + 2 * PAD, ox:ox + w + 2 * PAD] = np.pad(cl, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="edge")
            units.append({"index": k, "cell": cell, "ground": ground, "src_rect_px": list(rect),
                          "rect_px": [ox + PAD, oy + PAD, ox + PAD + w, oy + PAD + h],
                          "metres": [bay_m, STOREY]})
        # Mean sRGB darkening (night albedo), as the previous build did.
        mean = float(alb[:rows_n * ch].mean())
        k = min(1.0, BASE_TARGET_SRGB / max(mean, 1e-3))
        alb = np.clip(alb * k, 0.0, 1.0)
        Image.fromarray((alb * 255 + 0.5).astype(np.uint8)).save(
            os.path.join(OUT, "facades_%s_units.png" % family))
        Image.fromarray((np.clip(emi, 0, 1) * 255 + 0.5).astype(np.uint8)).save(
            os.path.join(OUT, "facades_%s_units_lit.png" % family))
        manifest["families"][family] = {
            "atlas": "facades_%s_units.png" % family, "lit": "facades_%s_units_lit.png" % family,
            "size_px": [W, H], "albedo_mean_srgb_before": round(mean, 3), "albedo_factor": round(k, 4),
            "cells": cell_infos, "units": units}
        # Far tiles: three single-storey upper units, written unpadded so they can REPEAT (wrap)
        # vertically and horizontally on far masses. One repeating tile per bay keeps the far LOD
        # un-stretched and cheap.
        upper = [t for t in tiles if not t[1]]
        far = []
        for fi in range(FAR_VARIANTS):
            cell, _g, ca, cl, rect = upper[(fi * len(upper)) // FAR_VARIANTS]
            fa = np.clip(ca * k, 0.0, 1.0)
            fb = "facades_%s_far%d.png" % (family, fi)
            fl = "facades_%s_far%d_lit.png" % (family, fi)
            Image.fromarray((fa * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, fb))
            Image.fromarray((np.clip(cl, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, fl))
            far.append({"base": fb, "lit": fl, "cell": cell, "src_rect_px": list(rect),
                        "metres": [bay_m, STOREY]})
        manifest["families"][family]["far"] = far
        print(family, "atlas", W, H, "units", len(units), "albedo factor %.3f" % k)
    with open(os.path.join(OUT, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)


if __name__ == "__main__":
    main()
