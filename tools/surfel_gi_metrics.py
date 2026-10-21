#!/usr/bin/env python3
"""Surfel GI metrics harness for the NOX engine.

Runs named engine configurations on one scene, collects run logs and final-frame PNG dumps,
and writes a markdown summary plus JSON. --reuse re-analyses existing run dirs without launching.

  python tools/surfel_gi_metrics.py --exe PATH --scene PATH --out-dir DIR \
      --configs off,ssgi,surf,both,ref,indep,surf_term,surf_r4k,surf_spawn1 \
      [--camera-path SPEC] [--repeat N] [--table PATH.md] [--json PATH.json] [--reuse] [--timeout S]

Runs: one engine launch per config and repeat, never retried, with a hard timeout (default 1800 s):
  <exe> --game --scene <scene> --window windowed --width 1920 --height 1080
  cwd = repo root; stdout+stderr go to <run dir>/run.log. Run dirs: DIR/<cfg>/ for --repeat 1,
  else DIR/<cfg>_r<k>/. A scene with "exposure_mode": "auto" is run through a manual-exposure temp
  copy in the same scenes/ directory, removed afterwards (also on error).

Env contract: NOX_* variables inherited from the caller are dropped, then
  base:    NOX_FIXED_DT=0.0166666667 NOX_LOG_PASS_METRICS=1 NOX_SURFEL_GI_LOG_SUBPASS_METRICS=1
           NOX_EXPOSURE_MODE=manual NOX_FINAL_DUMP_DIR=<run dir> NOX_FINAL_DUMP_FRAMES=<window>
           NOX_EXIT_AFTER_FRAMES=<last frame + 2>
  config:  CONFIG_ENV (NOX_ENABLE_SURFEL_GI / NOX_DISABLE_SURFEL_GI, NOX_SSGI, NOX_GI_REFERENCE,
           NOX_SURFEL_GI_* knobs)
  camera:  --camera-path SPEC adds NOX_CAMERA_PATH=SPEC to every run (dolly:... or orbit:...).

Frames: live configs run 0..261 and analyse 200..260; ref and indep run 0..899 and analyse 780..840.
Dump names carry renderedFrames (final_NNNNN.png, 1-based).

Logs (UTF-8 or UTF-16): [SurfelGISubpassMetrics] frame=N header and "  <label> gpuMs=" lines,
[SurfelGIFrameCounters] frame=N key=value (numeric values only; a/b/c composites skipped),
[FramePassMetrics] begin frame=K and pass=<name> gpuMs=... lines.

Analysis, per repeat r (reference = indep run r, else the first repeat that has one):
  off baseline = time-averaged linear RGB of the off run over its window.
  lit mask     = pixels whose time-averaged off display luminance is in (0.02, 0.97).
  GI           = time-averaged linear(config) - off baseline, averaged over RGB.
  Energy       = mean GI over lit pixels (off energy is zero by construction).
  Ratio = energy / reference energy; RMSE absolute and divided by the reference lit mean; Pearson.
  Noise: surf_term vs off, per-pixel population std over the window, quadrature subtraction per
  pixel, averaged over lit pixels.

Outputs: markdown (--table, default DIR/surfel_gi_metrics.md), JSON (--json, default
DIR/surfel_gi_metrics.json), DIR/maps/<cfg>_gi.png (reference lit mean maps to 50% grey, linear).
Exit 1 on hard errors (missing exe or scene, a run with no dumps in its analysis window).
"""

import argparse
import contextlib
import json
import math
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TIMEOUT = 1800.0
BASE_ENV = {
    "NOX_FIXED_DT": "0.0166666667",
    "NOX_LOG_PASS_METRICS": "1",
    "NOX_SURFEL_GI_LOG_SUBPASS_METRICS": "1",
    "NOX_LOG_PASS_METRICS_INTERVAL": "1",
    "NOX_SURFEL_GI_LOG_SUBPASS_METRICS_INTERVAL": "1",
    "NOX_EXPOSURE_MODE": "manual",
    # Scene fully loaded before frame 0 (async loading would count loading-overlay frames in the windows).
    "NOX_SYNC_LOAD": "1",
}
SURF_ENV = {"NOX_ENABLE_SURFEL_GI": "1", "NOX_SSGI": "0"}
REF_ENV = {
    **SURF_ENV, "NOX_SURFEL_GI_CAPTURE": "1", "NOX_SURFEL_GI_RAY_BUDGET": "65536",
    "NOX_SURFEL_GI_MAX_SURFELS": "262144", "NOX_SURFEL_GI_MAX_PROJECTED_SURFELS": "524288",
    "NOX_SURFEL_GI_MAX_SPAWNS": "65536", "NOX_SURFEL_GI_MAX_RECYCLES": "65536",
    "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "1.0", "NOX_SURFEL_GI_QUALITY_TIER": "3",
}
CONFIG_ENV = {
    "off": {"NOX_DISABLE_SURFEL_GI": "1", "NOX_SSGI": "0"},
    "ssgi": {"NOX_DISABLE_SURFEL_GI": "1", "NOX_SSGI": "1"},
    "surf": dict(SURF_ENV),
    "both": {"NOX_ENABLE_SURFEL_GI": "1", "NOX_SSGI": "1"},
    # Quality tiers (Low 0, High 2, Ultra 3; Medium is the default tier 1) and the gather candidate bound.
    "tier0": {**SURF_ENV, "NOX_SURFEL_GI_QUALITY_TIER": "0"},
    "tier2": {**SURF_ENV, "NOX_SURFEL_GI_QUALITY_TIER": "2"},
    "tier3": {**SURF_ENV, "NOX_SURFEL_GI_QUALITY_TIER": "3"},
    "surf_cand32": {**SURF_ENV, "NOX_SURFEL_GI_GATHER_CANDIDATES": "32"},
    "surf_cand128": {**SURF_ENV, "NOX_SURFEL_GI_GATHER_CANDIDATES": "128"},
    "surf_cand256": {**SURF_ENV, "NOX_SURFEL_GI_GATHER_CANDIDATES": "256"},
    # SSGI-on terms (composite debug 4 = far field, debug 3 = near field). Their sum is the additive GI of "both".
    "surf_term_both4": {**SURF_ENV, "NOX_SSGI": "1", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_both3": {**SURF_ENV, "NOX_SSGI": "1", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "3"},
    "surf_term": {**SURF_ENV, "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    # Plan section 22 round 2: the same term with the round-2 knobs off (disturbance, priority, prior init, stale grace 30).
    "surf_term_r2off": {**SURF_ENV, "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4", "NOX_SURFEL_GI_DISTURB": "0",
                        "NOX_SURFEL_GI_PRIORITY": "0", "NOX_SURFEL_GI_PRIOR_INIT": "0", "NOX_SURFEL_GI_STALE_GRACE": "30"},
    "surf_r4k": {**SURF_ENV, "NOX_SURFEL_GI_RAY_BUDGET": "4096"},
    "surf_spawn1": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "1.0"},
    "surf_k05": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "0.5"},
    "surf_k15": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "1.5"},
    "surf_norad": {**SURF_ENV, "NOX_SURFEL_GI_RADIAL_DEPTH": "0"},
    "surf_norecycle": {**SURF_ENV, "NOX_SURFEL_GI_MAX_RECYCLES": "1"},
    "surf_rc16": {**SURF_ENV, "NOX_SURFEL_GI_MAX_RECYCLES": "16"},
    # WP10 ablations at the same ray budget: guiding off, sharing off (each with its surf_term twin for noise).
    "surf_noguide": {**SURF_ENV, "NOX_SURFEL_GI_RAY_GUIDING": "0"},
    "surf_term_noguide": {**SURF_ENV, "NOX_SURFEL_GI_RAY_GUIDING": "0", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_nosharing": {**SURF_ENV, "NOX_SURFEL_GI_IRRADIANCE_SHARING": "0"},
    "surf_term_nosharing": {**SURF_ENV, "NOX_SURFEL_GI_IRRADIANCE_SHARING": "0", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    # Budget sweep for the noise target (noise twins of surf_r4k and an 8192-ray pair).
    "surf_term_r4k": {**SURF_ENV, "NOX_SURFEL_GI_RAY_BUDGET": "4096", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_r8k": {**SURF_ENV, "NOX_SURFEL_GI_RAY_BUDGET": "8192"},
    "surf_term_r8k": {**SURF_ENV, "NOX_SURFEL_GI_RAY_BUDGET": "8192", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    # Energy attribution (linear term metrics): one change from surf_term each. The names keep the surf_term prefix.
    "surf_term_norecycle": {**SURF_ENV, "NOX_SURFEL_GI_MAX_RECYCLES": "1", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_norad": {**SURF_ENV, "NOX_SURFEL_GI_RADIAL_DEPTH": "0", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_avgall": {**SURF_ENV, "NOX_SURFEL_GI_CELL_AVG_ALL": "1", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_reach0": {**SURF_ENV, "NOX_SURFEL_GI_GRID_REACH": "0", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_nodeficit": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_DEFICIT_LIMIT": "0", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_k05": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "0.5", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_k15": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "1.5", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_k125": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "1.25", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_k20": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "2.0", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_k025": {**SURF_ENV, "NOX_SURFEL_GI_SPAWN_BUDGET_SCALE": "0.25", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    # World surfels (plan section 17): off, and the path depth cap at its old value (32), each with a noise twin.
    "surf_noworld": {**SURF_ENV, "NOX_SURFEL_GI_WORLD": "0"},
    "surf_term_noworld": {**SURF_ENV, "NOX_SURFEL_GI_WORLD": "0", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_d32": {**SURF_ENV, "NOX_SURFEL_GI_PATH_DEPTH": "32", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_noworld_d32": {**SURF_ENV, "NOX_SURFEL_GI_WORLD": "0", "NOX_SURFEL_GI_PATH_DEPTH": "32",
                              "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_w8": {**SURF_ENV, "NOX_SURFEL_GI_WORLD_SPAWNS": "8", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "surf_term_w32": {**SURF_ENV, "NOX_SURFEL_GI_WORLD_SPAWNS": "32", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
    "ref": REF_ENV,
    "indep": {"NOX_GI_REFERENCE": "1", "NOX_SSGI": "0"},
    # Reference GI alone (composite debug 4), the same quantity as surf_term. Use it for ratios: "indep" energy
    # is composite minus off, which cancels the deferred IBL that off contains on sky-lit scenes.
    "indep_term": {"NOX_GI_REFERENCE": "1", "NOX_SSGI": "0", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"},
}
# Quality tiers (plan section 20). N = 0 Low, 1 Medium (default), 2 High, 3 Ultra. surf_t<N> is composite minus off (timing,
# energy); surf_term_t<N> is the GI term alone (energy, noise, convergence); both_t<N> turns SSGI on. The surf_term prefix
# makes the analysis use the term metrics.
for _tier in range(4):
    CONFIG_ENV[f"surf_t{_tier}"] = {**SURF_ENV, "NOX_SURFEL_GI_QUALITY_TIER": str(_tier)}
    CONFIG_ENV[f"surf_term_t{_tier}"] = {**SURF_ENV, "NOX_SURFEL_GI_QUALITY_TIER": str(_tier),
                                         "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4"}
    CONFIG_ENV[f"both_t{_tier}"] = {"NOX_ENABLE_SURFEL_GI": "1", "NOX_SSGI": "1",
                                    "NOX_SURFEL_GI_QUALITY_TIER": str(_tier)}
# Medium without the emitter distance cut (plan section 20): the twin of surf_term_t1 for the cut's energy and cost.
CONFIG_ENV["surf_term_t1_nocut"] = {**SURF_ENV, "NOX_SURFEL_GI_QUALITY_TIER": "1", "NOX_SURFEL_GI_COMPOSITE_DEBUG": "4",
                                    "NOX_SURFEL_GI_EMITTER_CULL": "0"}
LIVE_PLAN = (261, 200, 260)  # last run frame, analysis window first and last frame
DUMP_STEP = 1                # dump every Nth frame of the live window (--dump-step)
LONG_PLAN = (899, 780, 840)
# Set from --env: applied to every run on top of the config's own variables (for parameter sweeps).
EXTRA_ENV = {}
LONG_CONFIGS = ("ref", "indep", "indep_term")
REF_CONFIGS = ("indep", "ref")
# Configs whose energy is the composite debug-4 term itself (surfel or reference GI alone), not composite minus off.
TERM_CONFIG_PREFIXES = ("surf_term", "indep_term")
GI_CONFIGS = ("ssgi", "surf", "both", "ref", "indep", "surf_r4k", "surf_spawn1", "surf_k05", "surf_k15",
              "surf_noguide", "surf_nosharing", "surf_r8k")
SURFEL_LOG_CONFIGS = ("surf", "both", "tier0", "tier2", "tier3", "surf_cand32", "surf_cand128", "surf_cand256",
                      "surf_term", "surf_r4k", "surf_spawn1", "surf_k05", "surf_k15", "ref",
                      "surf_noguide", "surf_term_noguide", "surf_nosharing", "surf_term_nosharing",
                      "surf_term_r4k", "surf_r8k", "surf_term_r8k",
                      "surf_noworld", "surf_term_noworld", "surf_term_d32", "surf_term_noworld_d32",
                      "surf_term_w8", "surf_term_w32")
REQUIRED_COUNTERS = ("liveSurfels", "requestedRays", "allocatedRays", "shortfall")
OPTIONAL_COUNTERS = ("youngSurfels", "overflowGridEntries", "rayShortfall",
                     "worldRequests", "worldDropped", "worldSpawned", "worldSkipped", "liveWorld", "liveWorldCounted",
                     "cacheHits", "cacheMisses", "underCoveredTiles", "coverageVisibleTileCount",
                     "requestedWorld", "allocatedWorld", "worldAgeSum", "worldHistorySum")
SPREAD_KEYS = ["energy", "indep_ratio", "indep_rmse_rel", "indep_pearson", "noise_rel",
               "gi_both_minus_ssgi", "gi_surf_minus_off", "sum_labels_ms", "total_gpu_ms", "shortfall"]

DUMP_RE = re.compile(r"^final_(\d{5})\.png$")
NUM_RE = re.compile(r"^-?\d+(\.\d+)?([eE][-+]?\d+)?$")
KV_RE = re.compile(r"(\w+)=(\S+)")
SUB_HDR_RE = re.compile(r"^\[SurfelGISubpassMetrics\] frame=(\d+)(.*)$")
SUB_LBL_RE = re.compile(r"^\[SurfelGISubpassMetrics\]\s{2,}(.+?) gpuMs=(\S+)\s*$")
CNT_HDR_RE = re.compile(r"^\[SurfelGIFrameCounters\] frame=(\d+)(.*)$")
PASS_BEGIN_RE = re.compile(r"^\[FramePassMetrics\] begin frame=(\d+)")
PASS_LINE_RE = re.compile(r"^\[FramePassMetrics\] pass=(\S+)(.*)$")


class HarnessError(Exception):
    """Hard error: printed to stderr, exit code 1."""


# ---------------------------------------------------------------- run plan and launching

def plan_for(cfg):
    """(last run frame, analysis window frame list) for a config."""
    if cfg in LONG_CONFIGS:
        last, lo, hi = LONG_PLAN
        return last, list(range(lo, hi + 1))
    last, lo, hi = LIVE_PLAN
    return last, list(range(lo, hi + 1, DUMP_STEP))


def run_dir_for(out_dir, cfg, rep, repeat):
    return out_dir / (cfg if repeat == 1 else f"{cfg}_r{rep}")


def dumps_in(run_dir):
    """Frame number -> path for final_NNNNN.png files in run_dir."""
    if not run_dir.is_dir():
        return {}
    return {int(m.group(1)): p for p in run_dir.iterdir() if (m := DUMP_RE.match(p.name))}


def window_paths(cfg, run_dir):
    have = dumps_in(run_dir)
    return [have[f] for f in plan_for(cfg)[1] if f in have]


def is_complete(cfg, run_dir):
    have = dumps_in(run_dir)
    return (run_dir / "run.log").is_file() and all(f in have for f in plan_for(cfg)[1])


def scene_arg_for(path, root=REPO_ROOT):
    """Scene argument as the engine expects it: relative to the engine source root when inside it."""
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def _force_manual(node):
    """Rewrite every "exposure_mode": "auto" to "manual" in place. Returns True if anything changed."""
    if isinstance(node, dict):
        changed = False
        for key, value in node.items():
            if key == "exposure_mode" and value == "auto":
                node[key] = "manual"
                changed = True
            else:
                changed = _force_manual(value) or changed
        return changed
    if isinstance(node, list):
        return any([_force_manual(value) for value in node])
    return False


# The dumps are display-referred: the post-process applies the GT tone curve, bloom and grain by default, so a
# pixel is not proportional to radiance and the sRGB-LUT "linear" energy was not linear (plan 13, bug 0). Every
# measured run uses this profile: no tone curve, no bloom, no grain. Values above 1.0 then clip, so keep exposure in range.
MEASUREMENT_RENDER_SETTINGS = {
    "tonemap": "none", "bloom": False, "vignette": 0.0, "film_grain": 0.0,
    "chromatic_aberration": 0.0, "dither": False,
    # TAA and HBAO change the composite (history blend, diffuse AO) and are not part of the GI estimate.
    "taa": False, "ssao": False,
}


def _measurement_settings(data):
    """Set the linear-output render settings on a scene dict in place. Returns True if anything changed."""
    settings = data.get("render_settings")
    if not isinstance(settings, dict):
        settings = {}
    changed = not isinstance(data.get("render_settings"), dict)
    for key, value in MEASUREMENT_RENDER_SETTINGS.items():
        if settings.get(key) != value:
            settings[key] = value
            changed = True
    data["render_settings"] = settings
    return changed


@contextlib.contextmanager
def scene_for_runs(scene):
    """Yield the scene path to launch. Manual exposure and the linear-output profile go into a temp copy in the same dir."""
    try:
        data = json.loads(scene.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as err:
        raise HarnessError(f"cannot read scene {scene}: {err}") from err
    forced = _force_manual(data)
    measured = _measurement_settings(data)
    if not (forced or measured):
        yield scene
        return
    temp = scene.with_name(f"{scene.stem}.surfel_gi_metrics_{os.getpid()}{scene.suffix}")
    try:
        temp.write_text(json.dumps(data, indent=2), encoding="utf-8")
        yield temp
    finally:
        temp.unlink(missing_ok=True)


def launch(exe, scene_arg, run_dir, cfg, camera_path, timeout, cwd=REPO_ROOT):
    """One engine launch with a hard timeout. Returns the exit code, or 'timeout'.

    cwd must be a source root (it holds shaders/, config/ and CMakeLists.txt) for the engine to find its assets.
    """
    last, window = plan_for(cfg)
    env = {k: v for k, v in os.environ.items() if not k.startswith("NOX_")}
    env.update(BASE_ENV)
    env.update(CONFIG_ENV[cfg])
    env.update(EXTRA_ENV)
    env.update({"NOX_FINAL_DUMP_DIR": str(run_dir),
                "NOX_FINAL_DUMP_FRAMES": ",".join(str(f) for f in window),
                "NOX_EXIT_AFTER_FRAMES": str(last + 2)})
    if camera_path:
        env["NOX_CAMERA_PATH"] = camera_path
    cmd = [str(exe), "--game", "--scene", scene_arg,
           "--window", "windowed", "--width", "1920", "--height", "1080"]
    run_dir.mkdir(parents=True, exist_ok=True)
    with open(run_dir / "run.log", "wb") as log:
        try:
            return subprocess.run(cmd, cwd=cwd, env=env, stdout=log,
                                  stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            return "timeout"
        except OSError as err:
            raise HarnessError(f"cannot launch {exe}: {err}") from err


# ---------------------------------------------------------------- log parsing

def read_log(path):
    """Lines of a log. PowerShell redirection writes UTF-16, so detect BOM or NUL bytes."""
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        codec = "utf-16-le" if raw[:2] == b"\xff\xfe" else "utf-16-be"
        text = raw[2:].decode(codec, "replace")
    elif raw.startswith(b"\xef\xbb\xbf"):
        text = raw[3:].decode("utf-8", "replace")
    elif b"\x00" in raw[:256]:
        text = raw.decode("utf-16-le", "replace")
    else:
        text = raw.decode("utf-8", "replace")
    return text.splitlines()


def to_float(text):
    return float(text) if NUM_RE.match(text) else None


def parse_numbers(rest):
    """key=value pairs with plain numeric values; composite values such as a/b/c are skipped."""
    return {k: f for k, v in KV_RE.findall(rest) if (f := to_float(v)) is not None}


def parse_log(lines):
    """Return (subpass, counters, passes) keyed by frame; subpass[f] = {"header", "labels"}."""
    subpass, counters, passes = {}, {}, {}
    cur_sub = cur_block = None
    for line in lines:
        if m := SUB_HDR_RE.match(line):
            cur_sub = int(m.group(1))
            subpass[cur_sub] = {"header": parse_numbers(m.group(2)), "labels": {}}
        elif (m := SUB_LBL_RE.match(line)) and cur_sub is not None:
            if (ms := to_float(m.group(2))) is not None:
                subpass[cur_sub]["labels"][m.group(1)] = ms
        elif m := CNT_HDR_RE.match(line):
            counters[int(m.group(1))] = parse_numbers(m.group(2))
        elif m := PASS_BEGIN_RE.match(line):
            cur_block = int(m.group(1))
            passes.setdefault(cur_block, {})
        elif (m := PASS_LINE_RE.match(line)) and cur_block is not None:
            if (gpu := parse_numbers(m.group(2)).get("gpuMs")) is not None:
                passes[cur_block][m.group(1)] = gpu
    return subpass, counters, passes


def med(values):
    vals = [float(v) for v in values if v is not None and math.isfinite(v)]
    return float(np.median(vals)) if vals else None


def summarize(run_dir, window):
    """Per-run medians over the analysis window, from run.log (no log gives empty results)."""
    log = run_dir / "run.log"
    subpass, counters_by_frame, passes_by_frame = (
        parse_log(read_log(log)) if log.is_file() else ({}, {}, {}))
    lo, hi = window[0], window[-1]
    labels, totals, passes, counters = {}, [], {}, {}
    label_sums = []  # per frame: sum of the labelled sub-passes of that frame (not a sum of medians)
    sub_n = pass_n = cnt_n = 0
    for frame, rec in sorted(subpass.items()):
        if lo <= frame <= hi:
            sub_n += 1
            totals.append(rec["header"].get("totalGpuMs"))
            label_sums.append(sum(ms for ms in rec["labels"].values() if ms is not None))
            for label, ms in rec["labels"].items():
                labels.setdefault(label, []).append(ms)
    for frame, block in passes_by_frame.items():
        if lo <= frame <= hi and block:
            pass_n += 1
            for name, ms in block.items():
                passes.setdefault(name, []).append(ms)
    for frame, vals in sorted(counters_by_frame.items()):
        if lo <= frame <= hi:
            cnt_n += 1
            if "requestedRays" in vals and "allocatedRays" in vals:
                vals = {**vals, "shortfall": max(vals["requestedRays"] - vals["allocatedRays"], 0.0)}
            for key, value in vals.items():
                counters.setdefault(key, []).append(value)
    label_med = {k: med(v) for k, v in labels.items()}
    pass_med = {k: med(v) for k, v in passes.items()}
    # No sub-pass label is named "Execute" (plan 10.1): the old lookup never matched. The labelled total is the
    # median of per-frame sums, which includes the ApplyIndirect labels that the pass total also contains.
    return {
        "subpass_labels": label_med, "subpass_frames": sub_n, "total_gpu_ms": med(totals),
        "sum_labels_ms": med(label_sums) if label_sums else None, "execute_ms": None,
        "passes": pass_med, "pass_frames": pass_n,
        "counters": {k: med(v) for k, v in counters.items()}, "counter_frames": cnt_n,
    }


# ---------------------------------------------------------------- pixel analysis

def _srgb_lut():
    v = np.arange(256, dtype=np.float64) / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4).astype(np.float32)


LUT = _srgb_lut()


def accumulate(paths):
    """Time-average over dumps: linear RGB, display RGB, per-pixel mean/std of the linear RGB mean."""
    if not paths:
        raise HarnessError("no dumps to analyse")
    w, h = Image.open(paths[0]).size
    shape = (h, w, 3)
    lin_sum, disp_sum = np.zeros(shape), np.zeros(shape)
    s1, s2 = np.zeros((h, w)), np.zeros((h, w))
    for path in paths:
        img = np.asarray(Image.open(path).convert("RGB"))
        if img.shape != shape:
            raise HarnessError(f"{path}: size {img.shape} differs from {shape}")
        lin = LUT[img]
        s = lin.mean(axis=2, dtype=np.float64)
        lin_sum += lin
        disp_sum += img / 255.0
        s1 += s
        s2 += s * s
    n = len(paths)
    mean_s = s1 / n
    return {
        "lin": (lin_sum / n).astype(np.float32),
        "disp": (disp_sum / n).astype(np.float32),
        "s_mean": mean_s.astype(np.float32),
        "s_std": np.sqrt(np.maximum(s2 / n - mean_s * mean_s, 0.0)).astype(np.float32),
    }


def lit_mask(disp):
    lum = 0.2126 * disp[..., 0] + 0.7152 * disp[..., 1] + 0.0722 * disp[..., 2]
    return (lum > 0.02) & (lum < 0.97)


def safe_div(a, b):
    if a is None or b is None or not math.isfinite(b) or b == 0:
        return None
    return float(a) / float(b)


def pixel_pass(runs, configs, reps, warnings):
    """Per-run products keyed by (config, rep). Returns (pix, map_sum, map_n) for the GI maps."""
    pix, map_sum, map_n = {}, {}, {}
    for rep in reps:
        off = accumulate(window_paths("off", runs[("off", rep)])) if ("off", rep) in runs else None
        if off is None and any(c != "off" for c in configs):
            warnings.append(f"no off run for repeat {rep}: GI and noise metrics skipped")
            continue
        if off is None:
            continue
        mask = lit_mask(off["disp"])
        pix[("off", rep)] = {"mask": mask, "s_std": off["s_std"]}
        for cfg in configs:
            if cfg == "off" or (cfg, rep) not in runs:
                continue
            acc = accumulate(window_paths(cfg, runs[(cfg, rep)]))
            if cfg.startswith(TERM_CONFIG_PREFIXES):
                pix[(cfg, rep)] = {"s_mean": acc["s_mean"], "s_std": acc["s_std"]}
            else:
                gi_rgb = acc["lin"] - off["lin"]
                pix[(cfg, rep)] = {"gi": gi_rgb.mean(axis=2), "mask": mask}
                map_sum[cfg] = map_sum.get(cfg, 0) + gi_rgb
                map_n[cfg] = map_n.get(cfg, 0) + 1
    return pix, map_sum, map_n


def pick_ref(pix, ref, rep, reps):
    """Reference GI for repeat rep, falling back to the first repeat that has one."""
    for r in [rep] + [x for x in reps if x != rep]:
        entry = pix.get((ref, r))
        if entry is not None and "gi" in entry:
            return entry
    return None


def pair_metrics(a, b, mask):
    """Ratio, RMSE (absolute and / reference mean) and Pearson of GI map a against reference b."""
    if a.shape != b.shape:
        raise HarnessError(f"GI maps differ in size: {a.shape} vs {b.shape}")
    x, y = a[mask].astype(np.float64), b[mask].astype(np.float64)
    rmse = float(np.sqrt(np.mean((x - y) ** 2)))
    pearson = float(np.corrcoef(x, y)[0, 1]) if x.std() > 0 and y.std() > 0 else None
    return {"ratio": safe_div(x.mean(), y.mean()), "rmse": rmse,
            "rmse_rel": safe_div(rmse, y.mean()), "pearson": pearson}


def per_run_metrics(pix, runs, configs, reps):
    """Scalar metrics per (config, rep), plus the GI-on-minus-off differences."""
    per = {key: {} for key in runs}
    for rep in reps:
        off = pix.get(("off", rep))
        for cfg in configs:
            entry, m = pix.get((cfg, rep)), per.get((cfg, rep))
            if entry is None or m is None or cfg == "off":
                continue
            if cfg.startswith(TERM_CONFIG_PREFIXES):
                if off is None:
                    continue
                mask = off["mask"]
                surf_std = entry["s_std"][mask].astype(np.float64)
                off_std = off["s_std"][mask].astype(np.float64)
                corr = np.sqrt(np.maximum(surf_std ** 2 - off_std ** 2, 0.0))
                value = float(entry["s_mean"][mask].mean())
                m.update(noise_surf_std=float(surf_std.mean()), noise_off_std=float(off_std.mean()),
                         noise_corr_std=float(corr.mean()), surf_term_mean=value,
                         noise_rel=safe_div(float(corr.mean()), value))
                # Energy of the GI term alone (composite debug 4), and its ratio to the reference term of the same rep.
                ref_entry = pix.get(("indep_term", rep))
                if ref_entry is not None and "s_mean" in ref_entry:
                    ref_value = float(ref_entry["s_mean"][mask].mean())
                    m.update(term_energy=value, term_ratio_vs_indep=safe_div(value, ref_value))
                continue
            m["energy"] = float(entry["gi"][entry["mask"]].mean())
            for ref in REF_CONFIGS:
                ref_entry = pick_ref(pix, ref, rep, reps)
                if cfg != ref and ref_entry is not None:
                    pm = pair_metrics(entry["gi"], ref_entry["gi"], entry["mask"])
                    m.update({f"{ref}_{k}": v for k, v in pm.items()})
    for rep in reps:
        both, ssgi, surf = per.get(("both", rep), {}), per.get(("ssgi", rep), {}), per.get(("surf", rep), {})
        if "energy" in both and "energy" in ssgi:
            both["gi_both_minus_ssgi"] = both["energy"] - ssgi["energy"]
        if "energy" in surf:
            surf["gi_surf_minus_off"] = surf["energy"]  # off energy is zero by construction
    return per


def aggregate(per, configs, reps):
    """Mean over repeats per config and metric, and (max-min)/|mean| spread when repeats exist."""
    agg, spread = {}, {}
    for cfg in configs:
        agg[cfg], spread[cfg] = {}, {}
        keys = sorted({k for rep in reps for k in per.get((cfg, rep), {})})
        for key in keys:
            vals = [per[(cfg, r)][key] for r in reps if per.get((cfg, r), {}).get(key) is not None]
            if not vals:
                continue
            mean = float(np.mean(vals))
            agg[cfg][key] = mean
            if len(vals) > 1 and mean != 0:
                spread[cfg][key] = (max(vals) - min(vals)) / abs(mean)
    return agg, spread


def write_maps(map_sum, map_n, agg, out_dir, warnings):
    ref_energy = agg.get("indep", {}).get("energy")
    if not ref_energy or ref_energy <= 0:
        warnings.append("no indep energy: GI maps not written")
        return []
    maps_dir = out_dir / "maps"
    maps_dir.mkdir(parents=True, exist_ok=True)
    scale = 0.5 / ref_energy
    written = []
    for cfg, total in map_sum.items():
        img = np.clip(total / map_n[cfg] * scale, 0.0, 1.0)
        path = maps_dir / f"{cfg}_gi.png"
        Image.fromarray((img * 255.0 + 0.5).astype(np.uint8)).save(path)
        written.append(str(path))
    return written


# ---------------------------------------------------------------- markdown and JSON

def fmt(x, digits=4):
    return "n/a" if x is None else f"{x:.{digits}g}"


def pct(x):
    return "n/a" if x is None else f"{100.0 * x:.2f}%"


def md_table(headers, rows):
    return (["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
            + ["| " + " | ".join(row) + " |" for row in rows])


def mean_of(logs, cfg, reps, field, key=None):
    vals = []
    for rep in reps:
        summary = logs.get((cfg, rep))
        value = None if summary is None else (summary[field] if key is None else summary[field].get(key))
        if value is not None:
            vals.append(value)
    return float(np.mean(vals)) if vals else None


def ordered_union(dicts):
    seen = {}
    for d in dicts:
        for key in d:
            seen.setdefault(key, None)
    return list(seen)


def build_markdown(info, configs, reps, logs, agg, spread, warnings, maps):
    def mean(cfg, field, key=None):
        return mean_of(logs, cfg, reps, field, key)

    def per_config(rows_fn):
        return [fmt(rows_fn(c)) for c in configs]

    lines = list(info)
    header = ["Label"] + configs
    lines += ["## Per-pass GPU medians (ms)", "",
              "Median over analysis-window frames per run; mean over repeats.", ""]
    labels = ordered_union(logs[(c, r)]["subpass_labels"] for c in configs for r in reps if (c, r) in logs)
    rows = [[name] + per_config(lambda c, n=name: mean(c, "subpass_labels", n)) for name in labels]
    for title, field in (("sum of labelled passes", "sum_labels_ms"), ("totalGpuMs (header)", "total_gpu_ms"),
                         ("Execute median", "execute_ms")):
        rows.append([f"**{title}**"] + per_config(lambda c, f=field: mean(c, f)))
    lines += md_table(header, rows) + ["", "### FramePassMetrics (gpuMs, median in window)", ""]
    passes = ordered_union(logs[(c, r)]["passes"] for c in configs for r in reps if (c, r) in logs)
    pass_rows = [[name] + per_config(lambda c, n=name: mean(c, "passes", n)) for name in passes]
    lines += md_table(header, pass_rows or [["(none)"] + ["n/a"] * len(configs)]) + [""]

    lines += ["## Counters (median over analysis window, mean over repeats)", ""]
    counter_rows = []
    for name in REQUIRED_COUNTERS + OPTIONAL_COUNTERS:
        vals = [mean(c, "counters", name) for c in configs]
        if name in REQUIRED_COUNTERS or any(v is not None for v in vals):
            counter_rows.append([name] + [fmt(v, 6) for v in vals])
    lines += md_table(["Counter"] + configs, counter_rows) + [""]

    lines += ["## Energy, RMSE and correlation vs reference", "",
              "Energy = mean GI over lit pixels (linear). Ratio = config / reference energy. RMSE rel = "
              "RMSE / reference lit mean. Repeat-averaged; pairs are config run r vs reference run r.", ""]
    keys = ["energy", "indep_ratio", "indep_rmse", "indep_rmse_rel", "indep_pearson",
            "ref_ratio", "ref_rmse", "ref_rmse_rel", "ref_pearson"]
    energy_rows = [[c] + [fmt(agg.get(c, {}).get(k)) for k in keys] for c in configs if c in GI_CONFIGS]
    lines += md_table(["Config", "Energy", "Ratio vs indep", "RMSE vs indep", "RMSE rel vs indep",
                       "Pearson vs indep", "Ratio vs ref", "RMSE vs ref", "RMSE rel vs ref",
                       "Pearson vs ref"], energy_rows) + [""]

    st = agg.get("surf_term", {})
    lines += ["## Temporal noise (surf_term vs off, lit pixels)", "",
              "Per-pixel population std over the window of the RGB-mean linear value; corrected = "
              "sqrt(max(std_surf^2 - std_off^2, 0)) per pixel, averaged over lit pixels.", ""]
    lines += md_table(["Quantity", "Value"], [
        ["mean std, surf_term (linear)", fmt(st.get("noise_surf_std"))],
        ["mean std, off (linear)", fmt(st.get("noise_off_std"))],
        ["corrected std (quadrature)", fmt(st.get("noise_corr_std"))],
        ["mean surfel-term value (lit)", fmt(st.get("surf_term_mean"))],
        ["relative noise (corrected / mean)", pct(st.get("noise_rel"))],
        ["spread across repeats", pct(spread.get("surf_term", {}).get("noise_rel"))],
    ]) + [""]
    twins = [c for c in configs if c.startswith("surf_term") and c != "surf_term"]
    if twins:
        lines += md_table(["Term config", "Relative noise (corrected / mean)", "Mean surfel term (lit)"], [
            [c, pct(agg.get(c, {}).get("noise_rel")), fmt(agg.get(c, {}).get("surf_term_mean"))] for c in twins
        ]) + [""]

    indep_e, ref_e = agg.get("indep", {}).get("energy"), agg.get("ref", {}).get("energy")
    lines += ["## GI on minus off", "",
              "Energy differences in the energy table's units. Off energy is zero by construction.", ""]
    diff_rows = []
    for label, cfg, key in (("both - ssgi (surfel on top of SSGI)", "both", "gi_both_minus_ssgi"),
                            ("surf - off", "surf", "gi_surf_minus_off")):
        diff = agg.get(cfg, {}).get(key)
        diff_rows.append([label, fmt(diff), fmt(safe_div(diff, indep_e)), fmt(safe_div(diff, ref_e))])
    lines += md_table(["Difference", "Energy", "/ indep energy", "/ ref energy"], diff_rows) + [""]

    lines += ["## Repeat spread (max-min)/mean", ""]
    if len(reps) < 2:
        lines += ["Single repeat: spread not computed (use --repeat 2 or more).", ""]
    else:
        lines += ["Lead acceptance: spread within 5%.", ""]
        spread_rows = [[k] + [pct(spread.get(c, {}).get(k)) for c in configs]
                       for k in SPREAD_KEYS if any(k in spread.get(c, {}) for c in configs)]
        lines += md_table(["Metric"] + configs, spread_rows or [["(none)"] + ["n/a"] * len(configs)]) + [""]

    lines += ["## Warnings", ""] + ([f"- {w}" for w in warnings] or ["- none"]) + [""]
    lines += ["## GI maps", ""] + ([f"- `{m}`" for m in maps] or ["- none written"]) + [""]
    return lines


def jsonable(x):
    if isinstance(x, dict):
        return {str(k): jsonable(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [jsonable(v) for v in x]
    if isinstance(x, Path):
        return str(x)
    if isinstance(x, (bool, np.bool_)):
        return bool(x)
    if isinstance(x, (float, np.floating)):
        return float(x) if math.isfinite(float(x)) else None
    if isinstance(x, np.integer):
        return int(x)
    return x


# ---------------------------------------------------------------- main

def collect_runs(args, exe, scene, configs, out_dir, warnings):
    """Launch (or reuse) every config and repeat. Returns {(cfg, rep): run dir}."""
    runs = {}
    engine_root = Path(args.cwd).resolve() if args.cwd else REPO_ROOT
    # A scene under this checkout is launched from the same relative path in --cwd's tree, so the engine (and the manual
    # exposure temp copy, which is written next to the scene) stay inside that source root.
    scene_src = scene
    if engine_root != REPO_ROOT:
        try:
            scene_src = engine_root / scene.resolve().relative_to(REPO_ROOT)
        except ValueError:
            scene_src = scene
    with scene_for_runs(scene_src) as scene_path:
        scene_arg = scene_arg_for(scene_path, engine_root)
        for rep in range(1, args.repeat + 1):
            for cfg in configs:
                run_dir = run_dir_for(out_dir, cfg, rep, args.repeat)
                runs[(cfg, rep)] = run_dir
                if args.reuse and is_complete(cfg, run_dir):
                    print(f"[reuse] {run_dir.name}")
                    continue
                print(f"[run]   {run_dir.name}", flush=True)
                rc = launch(exe, scene_arg, run_dir, cfg, args.camera_path, args.timeout,
                            cwd=Path(args.cwd).resolve() if args.cwd else REPO_ROOT)
                window = plan_for(cfg)[1]
                missing = [f for f in window if f not in dumps_in(run_dir)]
                if len(missing) == len(window):
                    raise HarnessError(f"{run_dir.name}: no dumps in window {window[0]}..{window[-1]} "
                                       f"(exit {rc}); see {run_dir / 'run.log'}")
                if rc != 0:
                    warnings.append(f"{run_dir.name}: exit {rc} (dumps present)")
                if missing:
                    warnings.append(f"{run_dir.name}: {len(missing)} window dumps missing, e.g. {missing[:3]}")
    return runs


def run(args, configs):
    exe, scene = Path(args.exe).resolve(), Path(args.scene).resolve()
    if not exe.is_file():
        raise HarnessError(f"engine exe not found: {exe}")
    if not scene.is_file():
        raise HarnessError(f"scene not found: {scene}")
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    reps = list(range(1, args.repeat + 1))
    warnings = []
    runs = collect_runs(args, exe, scene, configs, out_dir, warnings)

    logs, sparse = {}, {}
    for (cfg, rep), run_dir in runs.items():
        window = plan_for(cfg)[1]
        logs[(cfg, rep)] = summary = summarize(run_dir, window)
        families = [("pass", summary["pass_frames"])]
        if cfg in SURFEL_LOG_CONFIGS:
            families += [("subpass", summary["subpass_frames"]), ("counter", summary["counter_frames"])]
        for name, count in families:
            if count == 0:
                warnings.append(f"{run_dir.name}: no {name} samples in window {window[0]}..{window[-1]}")
            elif count < len(window):
                sparse.setdefault(name, []).append(count)
    for name, counts in sorted(sparse.items()):
        env = "NOX_LOG_PASS_METRICS_INTERVAL" if name == "pass" else "NOX_SURFEL_GI_LOG_SUBPASS_METRICS_INTERVAL"
        warnings.append(f"{name} samples per run in the analysis window: {min(counts)}..{max(counts)} "
                        f"(log interval > 1; set {env}=1 for per-frame medians)")

    pix, map_sum, map_n = pixel_pass(runs, configs, reps, warnings)
    per = per_run_metrics(pix, runs, configs, reps)
    for key, summary in logs.items():
        per[key].update({"sum_labels_ms": summary["sum_labels_ms"], "total_gpu_ms": summary["total_gpu_ms"],
                         "execute_ms": summary["execute_ms"],
                         "shortfall": summary["counters"].get("shortfall"),
                         "live_surfels": summary["counters"].get("liveSurfels")})
    agg, spread = aggregate(per, configs, reps)
    maps = write_maps(map_sum, map_n, agg, out_dir, warnings)

    table_path = Path(args.table).resolve() if args.table else out_dir / "surfel_gi_metrics.md"
    json_path = Path(args.json).resolve() if args.json else out_dir / "surfel_gi_metrics.json"
    stat = exe.stat()
    camera = f"NOX_CAMERA_PATH={args.camera_path}" if args.camera_path else "static camera"
    info = ["## Run info", "",
            f"- Exe: `{exe}` (label `{exe.name} size={stat.st_size} "
            f"mtime={datetime.fromtimestamp(stat.st_mtime):%Y-%m-%d %H:%M:%S}`)",
            f"- Scene: `{scene}`; camera: {camera}",
            f"- Configs: {', '.join(configs)}; repeat={args.repeat}; reuse={'yes' if args.reuse else 'no'}",
            f"- Frame plans: live 0..{LIVE_PLAN[0]} analyse {LIVE_PLAN[1]}..{LIVE_PLAN[2]}; "
            f"ref/indep 0..{LONG_PLAN[0]} analyse {LONG_PLAN[1]}..{LONG_PLAN[2]}",
            f"- Out dir: `{out_dir}`; generated {datetime.now():%Y-%m-%d %H:%M:%S}", ""]
    md = build_markdown(info, configs, reps, logs, agg, spread, warnings, maps)
    results = {
        "run_info": {"exe": str(exe), "scene": str(scene), "out_dir": str(out_dir), "configs": configs,
                     "repeat": args.repeat, "camera_path": args.camera_path or None,
                     "generated": datetime.now().isoformat(timespec="seconds")},
        "runs": {f"{cfg}_r{rep}": per[(cfg, rep)] for (cfg, rep) in runs},
        "logs": {f"{cfg}_r{rep}": logs[(cfg, rep)] for (cfg, rep) in logs},
        "aggregate": agg, "spread_pct": spread, "warnings": warnings, "maps": maps,
    }
    table_path.parent.mkdir(parents=True, exist_ok=True)
    table_path.write_text("\n".join(md) + "\n", encoding="utf-8", newline="\n")
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(jsonable(results), indent=2), encoding="utf-8")

    print(f"surfel_gi_metrics: {len(runs)} runs, {len(warnings)} warnings")
    print(f"  table: {table_path}")
    print(f"  json:  {json_path}")
    for cfg in GI_CONFIGS:
        ratio = agg.get(cfg, {}).get("indep_ratio")
        if ratio is not None:
            print(f"  {cfg}: energy ratio vs indep = {ratio:.3f}")
    for warning in warnings:
        print(f"  warning: {warning}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run and summarize NOX surfel GI configurations.")
    parser.add_argument("--exe", required=True, help="engine executable")
    parser.add_argument("--scene", required=True, help="scene JSON")
    parser.add_argument("--out-dir", required=True, help="run dirs, maps and default outputs go here")
    parser.add_argument("--configs", required=True, help="comma list, e.g. off,surf,indep")
    parser.add_argument("--camera-path", default="", help="NOX_CAMERA_PATH spec for every run")
    parser.add_argument("--cwd", default="", help="engine source root (shaders/, config/, CMakeLists.txt); "
                        "default: this repository. Use it to run a baseline exe built from another commit.")
    parser.add_argument("--repeat", type=int, default=1, help="runs per config (default 1)")
    parser.add_argument("--table", help="markdown output (default OUT_DIR/surfel_gi_metrics.md)")
    parser.add_argument("--json", help="JSON output (default OUT_DIR/surfel_gi_metrics.json)")
    parser.add_argument("--reuse", action="store_true", help="skip configs whose run.log and dumps exist")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="hard timeout per run (s)")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="extra environment variable for every run, after the config's own (repeatable)")
    parser.add_argument("--frames", metavar="LO:HI", default="",
                        help="analysis window of the live configs (default 200:260); the run ends at HI+1. "
                             "The reference configs keep their own window.")
    parser.add_argument("--dump-step", type=int, default=1,
                        help="dump every Nth frame of the live window (default 1); e.g. --frames 10:900 --dump-step 10 "
                             "for a convergence run (plan section 20)")
    args = parser.parse_args(argv)
    for item in args.env:
        name, sep, value = item.partition("=")
        if not sep or not name:
            parser.error(f"--env expects NAME=VALUE: {item!r}")
        EXTRA_ENV[name] = value
    global LIVE_PLAN, DUMP_STEP
    if args.frames:
        try:
            lo, hi = (int(v) for v in args.frames.split(":"))
        except ValueError:
            parser.error(f"--frames expects LO:HI: {args.frames!r}")
        if not (0 <= lo <= hi) or args.dump_step < 1:
            parser.error("--frames needs 0 <= LO <= HI and --dump-step >= 1")
        LIVE_PLAN = (hi + 1, lo, hi)
    DUMP_STEP = max(args.dump_step, 1)

    configs = [c.strip() for c in args.configs.split(",") if c.strip()]
    unknown = [c for c in configs if c not in CONFIG_ENV]
    if unknown or not configs:
        parser.error(f"unknown or empty configs: {unknown or args.configs!r}")
    if args.repeat < 1:
        parser.error("--repeat must be >= 1")
    try:
        return run(args, configs)
    except HarnessError as err:
        print(f"ERROR: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
