"""Keep identical roads, lights, settings and cameras while excluding city geometry."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
scene = json.loads((ROOT / "scenes/shuto_buildings.json").read_text(encoding="utf-8"))
scene["scene_name"] = "shuto_buildings_road_only_baseline"
scene["nodes"] = [n for n in scene["nodes"] if not (
    n.get("type") == "model" and n.get("path", "").startswith("models/shuto/buildings/"))]
scene["game"]["collision"]["files"] = [p for p in scene["game"]["collision"]["files"]
                                        if not p.startswith("models/shuto/buildings/")]
(ROOT / "data/shuto/buildings/road_baseline_scene.json").write_text(
    json.dumps(scene, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
print("Road-only comparison generated;", len(scene["nodes"]), "nodes")
