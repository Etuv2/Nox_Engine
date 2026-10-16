"""Energy per dumped frame against the reference, lit-mask definition of the harness.

usage: surfel_gi_convergence.py RUN_DIR OFF_DIR OFF_FIRST OFF_LAST REF_DIR REF_FIRST REF_LAST [STEADY_FIRST STEADY_LAST]
  lit mask = pixels whose time-averaged off display luminance (frames OFF_FIRST..OFF_LAST) is in (0.02, 0.97).
  energy(f) = mean over lit pixels of the RGB-mean linear surf_term frame f.
  ratio(f) = energy(f) / reference energy (REF window, time-averaged).
  steady = mean ratio over STEADY window (default 800..900 when dumped), used for the 90% time.
"""
import os
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def srgb_to_linear(v):
    v = v / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


run_dir, off_dir = sys.argv[1], sys.argv[2]
off_first, off_last = int(sys.argv[3]), int(sys.argv[4])
ref_dir = sys.argv[5]
ref_first, ref_last = int(sys.argv[6]), int(sys.argv[7])
steady_first = int(sys.argv[8]) if len(sys.argv) > 8 else 800
steady_last = int(sys.argv[9]) if len(sys.argv) > 9 else 900

lum_sum = None
count = 0
for index in range(off_first, off_last + 1):
    path = os.path.join(off_dir, "final_%05d.png" % index)
    if os.path.exists(path):
        img = load(path) / 255.0
        lum = img @ np.array([0.2126, 0.7152, 0.0722])
        lum_sum = lum if lum_sum is None else lum_sum + lum
        count += 1
mask = (lum_sum / count > 0.02) & (lum_sum / count < 0.97)

ref_sum = None
n_ref = 0
for index in range(ref_first, ref_last + 1):
    path = os.path.join(ref_dir, "final_%05d.png" % index)
    if os.path.exists(path):
        img = srgb_to_linear(load(path)).mean(axis=2)
        ref_sum = img if ref_sum is None else ref_sum + img
        n_ref += 1
ref_energy = float((ref_sum / n_ref)[mask].mean())

ref_all = float((ref_sum / n_ref).mean())
rows = []
for name in sorted(os.listdir(run_dir)):
    if not (name.startswith("final_") and name.endswith(".png")):
        continue
    index = int(name[6:11])
    surf = srgb_to_linear(load(os.path.join(run_dir, name))).mean(axis=2)
    energy = float(surf[mask].mean())
    rows.append((index, energy, energy / ref_energy, float(surf.mean()) / ref_all))

print("lit pixels %d, reference lit energy %.5f, all-pixel %.5f (frames %d-%d)"
      % (int(mask.sum()), ref_energy, ref_all, ref_first, ref_last))
for index, energy, ratio, all_ratio in rows:
    print("%4d energy %.5f ratio %.4f all-pixel ratio %.4f" % (index, energy, ratio, all_ratio))

# Exponential approach fit over frames 100-900: ratio = A - B exp(-f / tau), scanned over tau.
fit_rows = [(index, ratio) for index, _, ratio, _ in rows if 100 <= index <= 900]
if len(fit_rows) >= 8:
    f = np.array([i for i, _ in fit_rows], dtype=np.float64)
    y = np.array([r for _, r in fit_rows], dtype=np.float64)
    best = None
    for tau in np.geomspace(20.0, 20000.0, 400):
        basis = np.stack([np.ones_like(f), -np.exp(-f / tau)], axis=1)
        coef, *_ = np.linalg.lstsq(basis, y, rcond=None)
        resid = float(np.sum((basis @ coef - y) ** 2))
        if best is None or resid < best[0]:
            best = (resid, tau, coef[0], coef[1])
    print("exponential fit (100-900): asymptote A %.4f, tau %.0f frames, residual %.2e" % (best[2], best[1], best[0]))
    print("fit check: predicted at 900 %.4f, measured %.4f" % (best[2] - best[3] * np.exp(-900 / best[1]),
                                                                 fit_rows[-1][1]))

steady = [ratio for index, _, ratio, _ in rows if steady_first <= index <= steady_last]
if steady:
    s = float(np.mean(steady))
    print("steady ratio (frames %d-%d) %.4f" % (steady_first, steady_last, s))
    target = 0.9 * s
    reached = [index for index, _, ratio, _ in rows if ratio >= target]
    first = reached[0] if reached else None
    print("first dumped frame at or above 90%% of steady: %s (target %.4f)" % (first, target))
