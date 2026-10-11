"""Package the kit and placed section, excluding Standard-licensed standalone derivatives."""
import hashlib
import json
from pathlib import Path
import struct
from urllib.parse import unquote
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "models/shuto/buildings"


def main():
    files = set()
    for path in OUT.rglob("*"):
        if not path.is_file() or "textures" in path.relative_to(OUT).parts:
            continue
        if "restricted_local" in path.parts or path.name.startswith("source_japanese") or path.suffix == ".blend1":
            continue
        if path.suffix in {".blend", ".glb", ".json"}:
            files.add(path)
        if path.suffix == ".glb":
            raw = path.read_bytes()
            size = struct.unpack_from("<I", raw, 12)[0]
            doc = json.loads(raw[20:20 + size])
            for img in doc.get("images", []):
                if "uri" in img:
                    files.add((path.parent / unquote(img["uri"])).resolve())
    assert not any("source_japanese" in p.name or "restricted_local" in p.parts for p in files)
    files.update(p for p in (ROOT / "data/shuto/buildings").glob("*.json")
                 if p.name not in {"package_manifest.json", "asset_import_scene.json"})
    files.update((ROOT / "textures/window_rooms").glob("*.png"))
    files.update([ROOT / "textures/window_rooms/atlas_manifest.json", ROOT / "textures/window_rooms/README.md",
                  ROOT / "docs/shuto/buildings.md", ROOT / "docs/shuto/assets_and_licenses.md",
                  ROOT / "docs/shuto/README.md", ROOT / "src/SceneLoader.cpp",
                  ROOT / "scenes/shuto_buildings.json", ROOT / "scenes/shuto_building_probe.json",
                  ROOT / "fonts/noto/OFL-NotoSansCJK.txt", ROOT / "fonts/noto/NotoSansJP-Bold.otf",
                  ROOT / "fonts/BarlowCondensed-SemiBold.ttf", ROOT / "fonts/licenses/Barlow-OFL.txt"])
    files.update((ROOT / "tools/shuto/blender").glob("*building*.py"))
    files.update([ROOT / "tools/shuto/blender/build_architecture.py", ROOT / "tools/shuto/blender/build_scenery.py",
                  ROOT / "tools/shuto/blender/normalize_placed_origins.py", ROOT / "tools/shuto/benchmark_buildings.ps1"])
    files.add(ROOT / "tools/shuto/blender/fuji_tv.py")
    files.add(ROOT / "tools/shuto/blender/optimize_placed_buildings.py")
    files.add(ROOT / "tools/shuto/blender/finish_building_blends.py")
    files.add(ROOT / "tools/shuto/blender/fix_building_signs.py")
    files.update((ROOT / "tools/shuto").glob("*building*.py"))
    files.update((ROOT / "captures/shuto/buildings").rglob("*.png"))
    files.update((ROOT / "captures/shuto/buildings").rglob("summary.json"))
    files.update((ROOT / "captures/shuto/buildings").rglob("frames.csv"))
    for filename in ("buildings_engine_build.log", "building_clearances.log", "building_context_clearances.log",
                     "building_section_capture.log", "building_section_capture.err", "building_section_timing.log",
                     "building_section_timing.err", "building_kit_capture.log", "building_kit_capture.err"):
        files.add(ROOT / "logs" / filename)
    files.add(ROOT / "logs/building_baseline_timing.log")
    files.add(ROOT / "logs/building_baseline_timing.err")
    for path in files:
        if path.suffix == ".glb":
            raw = path.read_bytes()
            size = struct.unpack_from("<I", raw, 12)[0]
            for img in json.loads(raw[20:20 + size]).get("images", []):
                if "uri" in img:
                    assert (path.parent / unquote(img["uri"])).resolve() in files, "Unpackaged GLB image"
    manifest = {"format": "nox-shuto-buildings-delivery", "reference_date": "2002-12-31",
                "requires": "Existing NOX repository road assets, source originals and texture pack for regeneration",
                "excluded": ["source_japanese.blend", "restricted_local/", "source_japanese image resources"],
                "files": [{"path": p.relative_to(ROOT).as_posix(), "bytes": p.stat().st_size,
                           "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted(files)]}
    destination = ROOT / "output/shuto_building_kit.zip"
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=5) as archive:
        for path in sorted(files):
            archive.write(path, path.relative_to(ROOT).as_posix())
        archive.writestr("DELIVERY_MANIFEST.json", json.dumps(manifest, indent=2, ensure_ascii=True) + "\n")
    (ROOT / "data/shuto/buildings/package_manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    with zipfile.ZipFile(destination) as archive:
        assert archive.testzip() is None
        assert not any("restricted_local/" in p or "source_japanese.blend" in p or "source_japanese_Image" in p
                       for p in archive.namelist())
    print("packaged", len(files), "files", destination, destination.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
