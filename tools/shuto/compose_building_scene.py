"""Compose the architectural replacement without modifying the established road scene."""
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "models/shuto/buildings"
# Top-level scene values owned by other agents; copied from the existing scenes/shuto_buildings.json.
PRESERVED_KEYS = ("skybox", "render_settings", "exposure", "gamma", "quality_tiers")


def write(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def main():
    scene = json.loads((ROOT / "scenes/shuto_rep_section.json").read_text(encoding="utf-8"))
    # Other owners (lead, render, roads) edit the published scene's look; keep their current values.
    target = ROOT / "scenes/shuto_buildings.json"
    current = json.loads(target.read_text(encoding="utf-8")) if target.is_file() else {}
    for key in PRESERVED_KEYS:
        if key in current:
            scene[key] = current[key]
    section = json.loads((OUT / "section_index.json").read_text(encoding="utf-8"))
    plan = json.loads((ROOT / "data/shuto/buildings/placements.json").read_text(encoding="utf-8"))
    net = json.loads((ROOT / "data/shuto/network.json").read_text(encoding="utf-8"))
    scene["scene_name"] = "shuto_buildings"
    scene["window_room_root"] = "textures/window_rooms"
    scene["nodes"] = [n for n in scene["nodes"] if "/scenery/" not in n.get("path", "").replace("\\", "/")]
    for folder in ("render", "context"):
        for path in sorted((OUT / folder).glob("*.glb")):
            scene["nodes"].append({"type": "model", "name": "Architecture_" + folder + "_" + path.stem,
                                   "path": path.relative_to(ROOT).as_posix(),
                                   "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]})
    files = scene["game"]["collision"]["files"]
    files[:] = [p for p in files if "/scenery_collision/" not in p.replace("\\", "/")]
    files.extend(c["collision"] for c in section["chunks"])
    files.extend(p.relative_to(ROOT).as_posix() for p in sorted((OUT / "context_collision").glob("*.glb")))
    road_points = [p for cw in net["carriageways"] for p in cw["points"]
                   if 0 <= p[1] <= 30 and -2600 <= p[0] <= 0 and 1500 <= p[2] <= 6000]
    def road_pose(b, target_y):
        cx, cz = b["obb"]["c"]
        road = min(road_points, key=lambda p: math.hypot(p[0] - cx, p[2] - cz))
        return [road[0], road[1] + 3, road[2]], [cx, target_y, cz]
    poses = []
    for name in ("dentsu", "fuji"):
        b = next(b for b in plan["buildings"] if b.get("landmark") == name)
        eye, target = road_pose(b, b["h"] * 0.46)
        # Fuji stands beyond the built road box. This shot checks its skyline silhouette.
        if name == "fuji":
            ux, uz = b["obb"]["u"]
            eye = [b["obb"]["c"][0] + uz * 420, 72, b["obb"]["c"][1] - ux * 420]
            target = [b["obb"]["c"][0], 65, b["obb"]["c"][1]]
        poses.append((name, eye, target))
    for name, family, zrange in (("ginza_frontage", "commercial", (1500, 2100)),
                                 ("shibaura_residential", "apartment", (3300, 5000))):
        candidates = [b for b in plan["buildings"] if b["detail"] == "near" and b["family"] == family
                      and zrange[0] < b["obb"]["c"][1] < zrange[1] and 8 < b["h"] < 40
                      and 8 < b["obb"]["w"] < 30 and 8 < b["obb"]["d"] < 25]
        visible = []
        for b in candidates:
            eye, target = road_pose(b, 9)
            distance = math.hypot(eye[0] - target[0], eye[2] - target[2])
            if 18 < distance < 75 and eye[1] < b["h"] - 3:
                visible.append((abs(distance - 35), b, eye, target))
        _, b, eye, target = min(visible, key=lambda p: p[0])
        target[1] = min(b["h"] - 2, eye[1] + 1.0)
        poses.append((name, eye, target))
    scene["benchmark"] = {"warmup_frames": 120, "fixed_hz": 60,
                           "shots": [{"name": name, "duration": 3, "capture_times": [2.5],
                                      "camera": {"keys": [{"t": 0, "position": eye, "look_at": target, "fov": 48 if name == "fuji" else 65}]}}
                                     for name, eye, target in poses]}
    scene["camera"].update(position=poses[0][1], look_at=poses[0][2], far=12000)
    scene["camera"].pop("target", None)
    for n in scene["nodes"]:
        if n.get("type") == "model":
            assert (ROOT / n["path"]).is_file(), n["path"]
    for p in files:
        assert (ROOT / p).is_file(), p
    write(ROOT / "scenes/shuto_buildings.json", scene)
    write(ROOT / "data/shuto/buildings/scene_composition.json",
          {"scene": "scenes/shuto_buildings.json", "model_nodes": sum(n.get("type") == "model" for n in scene["nodes"]),
           "collision_files": len(files), "shots": scene["benchmark"]["shots"],
           "base_scene": "scenes/shuto_rep_section.json"})
    print("scene composed", len(scene["nodes"]), "nodes", len(files), "collision files")


if __name__ == "__main__":
    main()
