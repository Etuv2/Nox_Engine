"""Import every standalone kit/source LOD and proxy, with representative visual views."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "models/shuto/buildings"


def main():
    scene = json.loads((ROOT / "scenes/shuto_building_probe.json").read_text(encoding="utf-8"))
    scene["scene_name"] = "shuto_building_kit_check"
    scene["nodes"] = [n for n in scene["nodes"] if n.get("type") != "model"]
    kit = json.loads((OUT / "kit_index.json").read_text(encoding="utf-8"))
    sources = json.loads((OUT / "source_kit_index.json").read_text(encoding="utf-8"))
    poses = []
    # Every LOD and proxy is imported. Only LOD0 is shown in the presentation gallery.
    for i, b in enumerate(kit["buildings"] + sources["buildings"]):
        position = [(i % 6) * 320, 0, -(i // 6) * 320]
        for lod in b["lods"]:
            at = position if lod["level"] == 0 else [15000 + i * 250, 0, lod["level"] * 1000]
            scene["nodes"].append({"type": "model", "name": b["id"] + "_LOD%d" % lod["level"],
                                   "path": lod["path"], "position": at, "rotation": [0, 0, 0], "scale": [1, 1, 1]})
        scene["nodes"].append({"type": "model", "name": b["id"] + "_proxy",
                               "path": b["collision"], "position": [15000 + i * 250, 0, 3000],
                               "rotation": [0, 0, 0], "scale": [1, 1, 1]})
        if b["id"] in {"retail_corner", "residential_balcony", "warehouse_large", "landmark_fuji_tv", "source_japanese_01"}:
            h = b["dimensions_m"][1]
            w = b["dimensions_m"][0]
            distance = max(30, h * 1.7, w * 1.4)
            poses.append({"name": b["id"], "duration": 1, "capture_times": [0.8],
                          "camera": {"keys": [{"t": 0, "position": [position[0] + distance * 0.15, h * 0.65, position[2] + distance],
                                                 "look_at": [position[0], h * 0.5, position[2]], "fov": 55}]}})
    for i, c in enumerate(kit["components"]):
        scene["nodes"].append({"type": "model", "name": c["id"], "path": c["path"],
                               "position": [12000 + i * 10, 0, 5000], "rotation": [0, 0, 0], "scale": [1, 1, 1]})
    edited_probe = OUT / "probe/edited_compact.glb"
    if edited_probe.exists():
        scene["nodes"].append({"type": "model", "name": "edited_collection_export_check",
                               "path": edited_probe.relative_to(ROOT).as_posix(),
                               "position": [18000, 0, 5000], "rotation": [0, 0, 0], "scale": [1, 1, 1]})
    scene["benchmark"] = {"warmup_frames": 60, "fixed_hz": 60, "shots": poses}
    scene["camera"]["far"] = 12000
    target = ROOT / "data/shuto/buildings/asset_import_scene.json"
    target.write_text(json.dumps(scene, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print("kit import check", sum(n.get("type") == "model" for n in scene["nodes"]), "models")


if __name__ == "__main__":
    main()
