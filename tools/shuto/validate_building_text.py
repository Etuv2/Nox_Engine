"""Verify delivery text encoding and normalize this task's redirected logs."""
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parents[2]
files = {ROOT / "src/SceneLoader.cpp", ROOT / "docs/shuto/README.md",
         ROOT / "docs/shuto/assets_and_licenses.md", ROOT / "docs/shuto/buildings.md",
         ROOT / "textures/window_rooms/README.md", ROOT / "scenes/shuto_buildings.json",
         ROOT / "scenes/shuto_building_probe.json", ROOT / "tools/shuto/benchmark_buildings.ps1"}
files.update((ROOT / "data/shuto/buildings").glob("*.json"))
files.update((ROOT / "models/shuto/buildings").rglob("*.json"))
files.update((ROOT / "tools/shuto").glob("*building*.py"))
files.update((ROOT / "tools/shuto/blender").glob("*building*.py"))
files.update([ROOT / "tools/shuto/blender/build_architecture.py", ROOT / "tools/shuto/blender/fuji_tv.py",
              ROOT / "tools/shuto/blender/normalize_placed_origins.py", ROOT / "tools/shuto/blender/finish_building_blends.py"])
logs = set((ROOT / "logs").glob("*building*.log")) | set((ROOT / "logs").glob("*building*.err"))
logs.update((ROOT / "logs").glob("*fuji*.log"))
normalized = []
for path in sorted(logs):
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff", b"\xef\xbb\xbf")):
        text = raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig")
        path.write_text(text, encoding="utf-8")
        normalized.append(path.relative_to(ROOT).as_posix())
files.update(logs)
forbidden = set("\u2018\u2019\u201c\u201d\u2013\u2014\u00a0\u200b\u200c\u200d\ufeff")
for path in sorted(files):
    raw = path.read_bytes()
    assert not raw.startswith((b"\xff\xfe", b"\xfe\xff", b"\xef\xbb\xbf")), str(path)
    text = raw.decode("utf-8", errors="strict")
    assert not (set(text) & forbidden), "Non-ASCII punctuation or hidden character: " + str(path)
    assert not any(0x80 <= ord(c) <= 0x9f for c in text), "Legacy punctuation/control: " + str(path)
    if path.suffix == ".py":
        py_compile.compile(str(path), doraise=True)
report = {"status": "PASS", "checked_files": len(files), "encoding": "UTF-8 without BOM",
          "punctuation": "ASCII; no legacy controls, smart quotes, Unicode dashes or hidden separators",
          "python_compilation": "PASS", "normalized_logs": normalized}
(ROOT / "data/shuto/buildings/encoding_validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print("PASS UTF-8/BOM/punctuation checks for", len(files), "files and Python script compilation")
