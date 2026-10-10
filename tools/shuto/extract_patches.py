#!/usr/bin/env python3
"""Extract standalone, padded texture patches from the Shuto texture pack.

Reads textures/shuto/manifest.json (relative to the repository root) and writes:
  textures/shuto/derived/<asset>_<cell>.png   atlas cell patches (content crop + padding)
  textures/shuto/derived/<surface>.png        surface tiles (copied, RGB, no padding)
  textures/shuto/derived/<surface>_seamless.png  where opposite-edge error is notable
  textures/shuto/derived/derived_manifest.json   provenance, rects, metrics, PBR scalars

Originals are never modified. Only the -v2 tunnel and wear files are used.

Rules implemented:
  * Atlas cells: opaque atlases use the manifest pixel_rect (exclusive right/bottom).
    Alpha atlases crop to the alpha >= 16 content bounds (computed here, and
    compared with the manifest values).
  * Padding: at least 8 px on every side, RGB edge-extended from the content.
  * Low-alpha fringe: straight-alpha pixels with alpha < 32 get RGB replaced by a
    dilation of the nearest pixels with alpha >= 32, so filtering with or without
    premultiplication does not pull saturated colour from invisible texels.
    Alpha values themselves are unchanged. Padding alpha is 0 for alpha atlases.
  * Seamless surfaces: two-pass offset-and-cross-blend (x then y) with a
    half-width feather. Metrics are recorded before and after.

Usage: python extract_patches.py [--repo ROOT] [--pad 8]
Requires: numpy, Pillow. Run with -I when the input directory is untrusted.
"""

import argparse
import hashlib
import json
import os
import statistics
import sys

import numpy as np
from PIL import Image

ALPHA_CONTENT_THRESHOLD = 16
FRINGE_ALPHA = 32
SURFACE_KINDS = {"surface"}
ATLAS_KINDS = {"atlas", "alpha_atlas"}
SEAM_MAE_LIMIT = 2.5       # RGB units (0-255) of opposite-edge MAE
SEAM_RATIO_LIMIT = 1.5     # seam MAE relative to median adjacent-column MAE


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fill_low_alpha_rgb(rgb, alpha):
    """Replace RGB under alpha < FRINGE_ALPHA with dilated colour from valid texels."""
    valid = alpha >= FRINGE_ALPHA
    out = rgb.astype(np.float32).copy()
    known = valid.copy()
    h, w = alpha.shape
    changed = int((~valid).sum())
    for _ in range(max(h, w)):
        if known.all():
            break
        src = np.where(known[..., None], out, 0.0)
        cnt = known.astype(np.float32)
        ssum = np.zeros_like(out)
        csum = np.zeros((h, w), np.float32)
        sp = np.pad(src, ((1, 1), (1, 1), (0, 0)))
        cp = np.pad(cnt, 1)
        for dy in range(3):
            for dx in range(3):
                if dy == 1 and dx == 1:
                    continue
                ssum += sp[dy:dy + h, dx:dx + w]
                csum += cp[dy:dy + h, dx:dx + w]
        newly = (~known) & (csum > 0)
        out[newly] = ssum[newly] / csum[newly][:, None]
        known = known | newly
    return np.clip(out + 0.5, 0, 255).astype(np.uint8), changed


def content_bounds(alpha, threshold=ALPHA_CONTENT_THRESHOLD):
    ys, xs = np.where(alpha >= threshold)
    if len(xs) == 0:
        return None
    # exclusive right/bottom
    return [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1]


def pad_patch(rgb, alpha, pad, alpha_atlas):
    rgb_p = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="edge")
    if alpha_atlas:
        a_p = np.pad(alpha, pad, mode="constant", constant_values=0)
    else:
        a_p = np.pad(alpha, pad, mode="edge")
    return np.dstack([rgb_p, a_p]).astype(np.uint8)


def seam_metrics(rgb):
    """Opposite-edge error versus median interior neighbour error (0-255 units)."""
    f = rgb.astype(np.float32)
    res = {}
    for axis, name in ((1, "x"), (0, "y")):
        g = np.moveaxis(f, axis, 0)  # index along axis is first
        seam = np.abs(g[0] - g[-1]).mean()
        adj = [np.abs(g[i] - g[i + 1]).mean() for i in range(g.shape[0] - 1)]
        interior = statistics.median(adj)
        res[name] = {
            "opposite_edge_mae": round(float(seam), 3),
            "interior_median_mae": round(float(interior), 3),
            "ratio": round(float(seam / max(interior, 1e-6)), 3),
        }
    return res


def _feather_weight(n):
    """Weight of the wrapped (rolled) image: 1 at the edges, 0 at the centre half."""
    idx = np.arange(n, dtype=np.float32)
    d = np.minimum(idx, n - 1 - idx) / (n / 2.0)  # 0 at edge, 1 at centre
    t = np.clip(d / 0.5, 0.0, 1.0)
    return (1.0 - t * t * (3 - 2 * t)).astype(np.float32)


def make_seamless(rgb):
    f = rgb.astype(np.float32)
    h, w, _ = f.shape
    # Pass 1: x. Rolled copy moves the x-seam to the centre, blended back at edges.
    rolled = np.roll(f, w // 2, axis=1)
    a = _feather_weight(w)[None, :, None]
    f = a * rolled + (1 - a) * f
    # Pass 2: y, same construction on the result (linear ops keep the x-seamless property).
    rolled = np.roll(f, h // 2, axis=0)
    b = _feather_weight(h)[:, None, None]
    f = b * rolled + (1 - b) * f
    return np.clip(f + 0.5, 0, 255).astype(np.uint8)


def load_rgb_rgba(path):
    im = Image.open(path)
    im.load()
    if im.mode == "RGBA":
        arr = np.array(im)
        return arr[..., :3].copy(), arr[..., 3].copy(), True
    return np.array(im.convert("RGB")), None, False


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    default_repo = os.path.abspath(os.path.join(here, "..", ".."))
    ap.add_argument("--repo", default=default_repo)
    ap.add_argument("--pad", type=int, default=8)
    args = ap.parse_args()
    if args.pad < 8:
        ap.error("padding must be at least 8 px")

    src_dir = os.path.join(args.repo, "textures", "shuto")
    out_dir = os.path.join(src_dir, "derived")
    os.makedirs(out_dir, exist_ok=True)
    manifest = json.load(open(os.path.join(src_dir, "manifest.json"), encoding="utf-8"))

    records = []
    surfaces_for_seamless = []
    for asset in manifest["assets"]:
        src_path = os.path.join(src_dir, asset["file"])
        if sha256_file(src_path) != asset["sha256"]:
            sys.exit("sha256 mismatch for " + asset["file"])
        rgb, alpha, has_alpha = load_rgb_rgba(src_path)
        base = {
            "id": asset["id"],
            "source_file": asset["file"],
            "source_sha256": asset["sha256"],
            "source_size": [int(rgb.shape[1]), int(rgb.shape[0])],
            "roughness": asset.get("roughness"),
            "metallic": asset.get("metallic"),
            "roughness_metallic_are_starting_values": True,
            "color_space": "sRGB",
        }

        if asset["kind"] in SURFACE_KINDS:
            entry = dict(base, kind="surface", padding=0,
                         suggested_tile_meters=asset.get("suggested_tile_meters"),
                         wrap="repeat",
                         mip_note="padding 0; repeat textures rely on REPEAT wrap")
            out_name = asset["id"] + ".png"
            Image.fromarray(rgb).save(os.path.join(out_dir, out_name), optimize=True)
            entry["file"] = out_name
            entry["metrics_source"] = seam_metrics(rgb)
            seam_worst = max(entry["metrics_source"]["x"]["ratio"], entry["metrics_source"]["y"]["ratio"])
            seam_abs = max(entry["metrics_source"]["x"]["opposite_edge_mae"],
                           entry["metrics_source"]["y"]["opposite_edge_mae"])
            notable = seam_worst > SEAM_RATIO_LIMIT and seam_abs > SEAM_MAE_LIMIT
            entry["seam_notable"] = notable
            if notable:
                seamless = make_seamless(rgb)
                s_name = asset["id"] + "_seamless.png"
                Image.fromarray(seamless).save(os.path.join(out_dir, s_name), optimize=True)
                entry["seamless_file"] = s_name
                entry["metrics_seamless"] = seam_metrics(seamless)
                surfaces_for_seamless.append(asset["id"])
            records.append(entry)
            continue

        if asset["kind"] not in ATLAS_KINDS:
            continue
        alpha_atlas = asset["kind"] == "alpha_atlas"
        if alpha_atlas:
            ctype = alpha
        else:
            ctype = None
        cells_out = []
        for cell in asset["cells"]:
            x0, y0, x1, y1 = cell["pixel_rect"]
            c_rgb = rgb[y0:y1, x0:x1]
            entry = {"cell": cell["id"], "atlas_source_rect": [x0, y0, x1, y1]}
            if alpha_atlas:
                c_a = alpha[y0:y1, x0:x1]
                bounds = content_bounds(c_a)
                # bounds are relative to the cell; convert to atlas coordinates
                if bounds is None:
                    raise SystemExit("empty cell " + cell["id"])
                bx0, by0, bx1, by1 = bounds
                c_rgb = c_rgb[by0:by1, bx0:bx1]
                c_a = c_a[by0:by1, bx0:bx1]
                rgb_fixed, changed = fill_low_alpha_rgb(c_rgb, c_a)
                manifest_bounds = cell.get("alpha_bounds_threshold_16")
                entry.update({
                    "content_rect_atlas": [x0 + bx0, y0 + by0, x0 + bx1, y0 + by1],
                    "manifest_content_rect_atlas": manifest_bounds,
                    "fringe_pixels_replaced": changed,
                    "fringe_fraction": round(changed / max(1, c_a.size), 4),
                })
                patch = pad_patch(rgb_fixed, c_a, args.pad, True)
            else:
                patch = pad_patch(c_rgb, np.full(c_rgb.shape[:2], 255, np.uint8), args.pad, False)[..., :3]
                entry["content_rect_atlas"] = [x0, y0, x1, y1]
                entry["fringe_fraction"] = 0.0
            name = asset["id"] + "_" + cell["id"] + ".png"
            if patch.shape[2] == 3:
                Image.fromarray(patch, "RGB").save(os.path.join(out_dir, name), optimize=True)
            else:
                Image.fromarray(patch, "RGBA").save(os.path.join(out_dir, name), optimize=True)
            h, w = patch.shape[:2]
            entry.update({
                "file": name,
                "size": [int(w), int(h)],
                "padding": args.pad,
                "alpha_atlas": alpha_atlas,
                "sample_uv_rect_in_patch": [args.pad / w, args.pad / h,
                                            1 - args.pad / w, 1 - args.pad / h],
                "max_safe_mip_note": "padding 8 px keeps bleed under one texel to about mip 2-3",
            })
            cells_out.append(entry)
        records.append(dict(base, kind=asset["kind"], wrap="clamp_to_edge_per_patch",
                            roughness=asset.get("roughness"), metallic=asset.get("metallic"),
                            cells=cells_out))

    with open(os.path.join(out_dir, "derived_manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({
            "generator": "tools/shuto/extract_patches.py",
            "padding_px": args.pad,
            "fringe_alpha_threshold": FRINGE_ALPHA,
            "content_alpha_threshold": ALPHA_CONTENT_THRESHOLD,
            "seam_notable_rule": {"ratio_gt": SEAM_RATIO_LIMIT, "mae_gt": SEAM_MAE_LIMIT},
            "suggested_tile_meters_note": "from source manifest; roughness/metallic are starting scalars",
            "assets": records,
        }, f, indent=2, ensure_ascii=True)
        f.write("\n")
    print("wrote", out_dir, "seamless:", surfaces_for_seamless)


if __name__ == "__main__":
    main()
