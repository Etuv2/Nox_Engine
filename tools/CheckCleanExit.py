#!/usr/bin/env python3
"""Clean-exit check: runs Nox_Engine.exe for a few frames in each launch mode and fails on any non-zero exit code
or any "[CrashHandler]" line in the log (a crash during static destruction shows up only there).

    python tools/CheckCleanExit.py                 (one pass over every mode)
    python tools/CheckCleanExit.py --repeat 10     (repeat the modes; the surfel exit crash was intermittent)

Needs build/vs2022-x64/Release/Nox_Engine.exe. Logs go to captures/clean_exit/ (kept only for failing runs).
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / "vs2022-x64" / "Release" / "Nox_Engine.exe"
LOG_DIR = ROOT / "captures" / "clean_exit"
FRAMES = "60"
TIMEOUT_S = 240

# Each mode: launch arguments and extra environment. The surfel GI mode is the one that reproduced the exit crash.
MODES = {
    "game": (["--game"], {}),
    "editor": (["--preset", "editor"], {}),
    "editor-surfel-gi": (["--preset", "editor"], {"NOX_ENABLE_SURFEL_GI": "1", "NOX_SSGI": "0"}),
}


def run_mode(name, args, extra_env, index):
    env = dict(os.environ)
    for key in ("NOX_EXIT_AFTER_FRAMES", "NOX_FIXED_DT", "NOX_EDITOR_SMOKE"):
        env.pop(key, None)
    env.update({"NOX_EXIT_AFTER_FRAMES": FRAMES, "NOX_FIXED_DT": "0.0166666667"})
    env.update(extra_env)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"{name}_{index}.log"
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        try:
            proc = subprocess.run([str(EXE), *args], cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                  stdout=log, stderr=subprocess.STDOUT, timeout=TIMEOUT_S)
            code = proc.returncode
        except subprocess.TimeoutExpired:
            code = None
    text = log_path.read_text(encoding="utf-8", errors="replace")
    crashed = "[CrashHandler]" in text
    ok = code == 0 and not crashed
    if ok:
        log_path.unlink()
    detail = "timed out" if code is None else f"exit code {code}"
    if crashed:
        detail += ", [CrashHandler] in log"
    print(f"[{'PASS' if ok else 'FAIL'}] {name} #{index}: {detail}" + ("" if ok else f" (log: {log_path})"), flush=True)
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repeat", type=int, default=1, help="runs of each mode")
    options = parser.parse_args()
    if not EXE.exists():
        print(f"engine not built: {EXE}")
        return 1
    failures = 0
    for index in range(options.repeat):
        for name, (args, extra_env) in MODES.items():
            if not run_mode(name, args, extra_env, index):
                failures += 1
    total = options.repeat * len(MODES)
    print(f"{total - failures} passed, {failures} failed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
