#!/usr/bin/env python3
"""Blockiness metric for the surfel GI term (plan section 21).

Works on the final-frame dumps of tools/surfel_gi_metrics.py runs. A static camera is time-averaged over
the analysis window first, which removes the per-frame ray noise and leaves the spatial structure.

  python tools/surfel_gi_blockiness.py --term-dir DIR/surf_term --off-dir DIR/off \
      [--cellmap-dir DIR/cellmap] [--window 200:260] [--json PATH]

Metrics (all on the time-averaged linear term, luminance Y, lit pixels from the off run):
  hp_rel        mean |Y - blur(Y, sigma 2)| / mean Y. High-pass energy relative to the signal.
  fine_rel      mean |Y - blur(Y, 1.5)| / mean Y: surfel-scale detail (blotches).
  mid_rel       mean |blur(Y, 1.5) - blur(Y, 6)| / mean Y: cell-scale structure (steps and seams).
  grad_rel      mean |dY| between horizontal and vertical neighbours / mean Y.
  boundary_p99 / interior_p99   99th percentile of the same gradients on boundary / interior pairs. Steps show up
                here even when the mean is dominated by smooth shading.
  seam_ratio    (cell-boundary gradient) / (interior gradient), needs --cellmap-dir. The cell map is a
                run with NOX_SURFEL_GI_DEBUG_VIEW=6 (grid cell colours, gather scale 1.0). A pixel pair
                straddles a boundary when its cell colours differ. Above 1 means a seam at cell borders.
Noise biases every number upward and seam_ratio towards 1, so compare runs of the same scene and window.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def srgb_to_linear(img):
    v = img.astype(np.float32) / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def window_paths(run_dir, lo, hi):
    paths = []
    for p in sorted(Path(run_dir).glob("final_*.png")):
        n = int(p.stem.split("_")[1])
        if lo <= n <= hi:
            paths.append(p)
    if not paths:
        raise SystemExit(f"no dumps in {run_dir} for frames {lo}..{hi}")
    return paths


def time_average(paths, linear=True):
    acc = None
    for p in paths:
        img = np.asarray(Image.open(p).convert("RGB"))
        val = srgb_to_linear(img) if linear else img.astype(np.float32) / 255.0
        acc = val if acc is None else acc + val
    return acc / len(paths)


def gaussian_blur(y, sigma):
    radius = int(round(3 * sigma))
    x = np.arange(-radius, radius + 1, dtype=np.float32)
    k = np.exp(-(x * x) / (2 * sigma * sigma))
    k /= k.sum()
    out = np.apply_along_axis(lambda r: np.convolve(r, k, mode="same"), 1, y)
    out = np.apply_along_axis(lambda c: np.convolve(c, k, mode="same"), 0, out)
    return out


def cell_ids_from_colors(colors):
    """Quantised colour as an id: pixels of one cell share the same debug colour."""
    q = np.round(colors * 255.0).astype(np.int32)
    return (q[..., 0] << 16) | (q[..., 1] << 8) | q[..., 2]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--term-dir", required=True)
    ap.add_argument("--off-dir", required=True)
    ap.add_argument("--cellmap-dir", default="")
    ap.add_argument("--window", default="200:260")
    ap.add_argument("--json", default="")
    ap.add_argument("--edge-threshold", type=float, default=0.02,
                    help="display-luma step in the off image above which a pixel pair counts as a geometric edge")
    args = ap.parse_args(argv)
    lo, hi = (int(v) for v in args.window.split(":"))

    term = time_average(window_paths(args.term_dir, lo, hi))
    off_disp = time_average(window_paths(args.off_dir, lo, hi), linear=False)
    lum = 0.2126 * off_disp[..., 0] + 0.7152 * off_disp[..., 1] + 0.0722 * off_disp[..., 2]
    lit = (lum > 0.02) & (lum < 0.97)

    y = term @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    mean_y = float(y[lit].mean())

    hp = np.abs(y - gaussian_blur(y, 2.0))
    hp_rel = float(hp[lit].mean() / mean_y)
    # Band split: surfel-scale (sigma 1.5 detail) and cell-scale (between sigma 1.5 and 6) energy.
    b15 = gaussian_blur(y, 1.5)
    fine_rel = float(np.abs(y - b15)[lit].mean() / mean_y)
    mid_rel = float(np.abs(b15 - gaussian_blur(y, 6.0))[lit].mean() / mean_y)

    gx = np.abs(y[:, 1:] - y[:, :-1])
    gy = np.abs(y[1:, :] - y[:-1, :])
    off_gx = np.abs(lum[:, 1:] - lum[:, :-1])
    off_gy = np.abs(lum[1:, :] - lum[:-1, :])
    lit_x = lit[:, 1:] & lit[:, :-1] & (off_gx < args.edge_threshold)
    lit_y = lit[1:, :] & lit[:-1, :] & (off_gy < args.edge_threshold)
    grad_rel = float((gx[lit_x].sum() + gy[lit_y].sum()) / (lit_x.sum() + lit_y.sum()) / mean_y)

    result = {"term_dir": args.term_dir, "window": [lo, hi], "lit_pixels": int(lit.sum()),
              "pairs_kept_x": int(lit_x.sum()), "pairs_kept_y": int(lit_y.sum()),
              "mean_term_luma_lit": mean_y, "hp_rel": hp_rel, "fine_rel": fine_rel, "mid_rel": mid_rel, "grad_rel": grad_rel}

    if args.cellmap_dir:
        cell_colors = np.median(np.stack([np.asarray(Image.open(p).convert("RGB")).astype(np.float32) / 255.0
                                          for p in window_paths(args.cellmap_dir, lo, hi)]), axis=0)
        cid = cell_ids_from_colors(cell_colors)
        bx = (cid[:, 1:] != cid[:, :-1]) & lit_x
        by = (cid[1:, :] != cid[:-1, :]) & lit_y
        ix = (~(cid[:, 1:] != cid[:, :-1])) & lit_x
        iy = (~(cid[1:, :] != cid[:-1, :])) & lit_y
        result["edge_threshold"] = args.edge_threshold
        seam_num = gx[bx].sum() + gy[by].sum()
        seam_den = bx.sum() + by.sum()
        int_num = gx[ix].sum() + gy[iy].sum()
        int_den = ix.sum() + iy.sum()
        boundary_grad = float(seam_num / max(seam_den, 1) / mean_y)
        interior_grad = float(int_num / max(int_den, 1) / mean_y)
        seam_vals = np.concatenate([gx[bx], gy[by]]) / mean_y
        int_vals = np.concatenate([gx[ix], gy[iy]]) / mean_y
        result.update({"boundary_p99_rel": float(np.percentile(seam_vals, 99)),
                       "interior_p99_rel": float(np.percentile(int_vals, 99)),
                       "cell_count_in_view": int(np.unique(cid[lit]).size),
                       "boundary_grad_rel": boundary_grad, "interior_grad_rel": interior_grad,
                       "seam_ratio": boundary_grad / max(interior_grad, 1e-9)})

    text = json.dumps(result, indent=2)
    print(text)
    if args.json:
        Path(args.json).write_text(text + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
