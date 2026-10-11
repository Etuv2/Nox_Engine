"""Consolidate measured delivery geometry and actual NOX import evidence."""
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "data/shuto/buildings"
OUT = ROOT / "models/shuto/buildings"


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def save(name, value):
    (DATA / name).write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def log_text(path):
    raw = path.read_bytes()
    return raw.decode("utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")


def glb_doc(path):
    raw = path.read_bytes()
    return json.loads(raw[20:20 + struct.unpack_from("<I", raw, 12)[0]])


def import_check(prefix, scene, capture_folder):
    log = log_text(ROOT / "logs" / (prefix + "_capture.log"))
    errors = log_text(ROOT / "logs" / (prefix + "_capture.err"))
    def count_models(nodes):
        return sum((n.get("type") == "model") + count_models(n.get("children", [])) for n in nodes)
    expected = count_models(scene["nodes"])
    loaded = log.count("[Scene] Import summary for ")
    assert expected == loaded, (prefix, expected, loaded)
    assert "Imported building room atlas: 6 rooms, 30 faces" in log
    assert "[Core] Shutdown complete" in log
    assert not re.search(r"Failed to load|Unknown image format|missing critical|\[ERROR\]", log + errors, re.I)
    images = []
    for path in sorted((ROOT / capture_folder).rglob("*.png")):
        with Image.open(path) as im:
            images.append({"path": path.relative_to(ROOT).as_posix(), "dimensions": list(im.size)})
    collision = re.search(r"\[GameSession\] Collision: (\d+) meshes, (\d+) triangles", log)
    return {"status": "PASS", "model_nodes_loaded": loaded, "model_nodes_expected": expected,
            "room_atlas": {"rooms": 6, "faces": 30}, "captures": images,
            "collision": {"meshes": int(collision[1]), "triangles": int(collision[2])} if collision else None,
            "stderr": errors.strip(), "log": "logs/" + prefix + "_capture.log"}


def main():
    validation = read(DATA / "export_validation.json")
    assert validation["status"] == "PASS"
    checks = validation["checks"]
    section = read(OUT / "section_index.json")
    kit = read(OUT / "kit_index.json")
    sources = read(OUT / "source_kit_index.json")
    plan = read(DATA / "placements.json")
    by_folder = {}
    for entry in checks:
        folder = Path(entry["path"]).parent.name
        record = by_folder.setdefault(folder, {"files": 0, "triangles": 0, "parallax_panes": 0, "bytes": 0})
        for key in record:
            record[key] += 1 if key == "files" else entry[key]
    render_paths = [ROOT / c["render"] for c in section["chunks"]]
    material_groups = sum(len(m["primitives"]) for p in render_paths for m in glb_doc(p)["meshes"])
    stats = {"units": "metres", "reference_date": "2002-12-31", "project_building_prototypes": len(kit["buildings"]),
             "components": len(kit["components"]), "normalized_source_buildings": len(sources["buildings"]),
             "redistributable_source_buildings": sum(b["standalone_redistribution"] for b in sources["buildings"]),
             "placed_building_parts": len(plan["buildings"]), "detail_tiers": dict(Counter(b["detail"] for b in plan["buildings"])),
             "building_render_triangles": section["render_triangles"], "building_collision_triangles": section["collision_triangles"],
             "chunk_material_groups": material_groups, "source_linked_instances": section["source_instances"],
             "exported_files_by_folder": by_folder,
             "total_city_scenery_render_triangles": by_folder["render"]["triangles"] + by_folder["context"]["triangles"],
             "geometry_budget_triangles": 1300000, "lods": 3,
             "lod_policy": "Placed proximity tiers baked into chunks; standalone files have no automatic runtime switch"}
    assert stats["total_city_scenery_render_triangles"] <= stats["geometry_budget_triangles"]
    save("geometry_statistics.json", stats)
    save("material_optimization.json", {"initial_chunk_material_groups": 1048, "final_chunk_material_groups": material_groups,
         "final_building_triangles": section["render_triangles"],
         "policy": "12 shared mid/far facade styles; full detail within 50 m and lightweight parallax geometry to 110 m"})
    main_import = import_check("building_section", read(ROOT / "scenes/shuto_buildings.json"), "captures/shuto/buildings/section_capture")
    kit_import = import_check("building_kit", read(DATA / "asset_import_scene.json"), "captures/shuto/buildings/kit_import")
    assert len(main_import["captures"]) == 4 and len(kit_import["captures"]) == 5
    timings = read(ROOT / "captures/shuto/buildings/section_timing/summary.json")
    assert timings["mode"] == "timing" and timings["repeat"] == 3
    target = 1000 / 165
    preservation = []
    for source in read(DATA / "source_audit.json")["sources"]:
        path = ROOT / "models/shuto/src/originals" / source["file"]
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        assert actual == source["sha256"]
        preservation.append({"file": source["file"], "sha256": actual, "unchanged": True})
    for name in ("building_clearances.log", "building_context_clearances.log"):
        assert "PASS: zero scenery collision triangles" in log_text(ROOT / "logs" / name)
    report = {"asset_import_status": "PASS", "section": main_import, "standalone_kit_sources_and_edited_export": kit_import,
              "static_glb_files_checked": len(checks), "originals": preservation,
              "performance": {"hardware": "RTX 3080 / Ryzen 7 5800X", "quality": "High", "resolution": [3440, 1440],
                              "target_hz": 165, "target_ms": target, "target_met": timings["overall"]["frame_ms"]["p95"] <= target,
                              "overall": timings["overall"], "repeat": timings["repeat"],
                              "samples": sum(s["frames"] for s in timings["shots"]),
                              "shots": [{"name": s["name"], "frame_ms": s["frame_ms"], "gpu_ms": s["gpu_ms"]} for s in timings["shots"]],
                              "interpretation": "Frame wall time includes CPU and GPU scheduling; GPU pass sums are not end-to-end FPS"},
              "not_validated": ["Full-course driving, collision response and traversal regression",
                                "Every placement's visual appearance; four district views and five standalone views inspected",
                                "Standalone lower LOD appearance at every transition; files imported, no runtime LOD switch implemented",
                                "Historical accuracy of undated current OSM footprints and inferred heights",
                                "Remaining road-graph-only network has no buildings placed"],
              "licensing_limit": "Japanese source is locally editable and imported but excluded from public standalone archive: embedded Sketchfab Standard"}
    baseline_path = ROOT / "captures/shuto/buildings/road_baseline/summary.json"
    if baseline_path.exists():
        baseline = read(baseline_path)
        assert baseline["mode"] == "timing" and baseline["repeat"] == 3
        report["performance"]["road_only_comparison"] = {
            "overall": baseline["overall"], "target_met": baseline["overall"]["frame_ms"]["p95"] <= target,
            "method": "Same scene settings, roads, lights, player and camera shots; all building/context render and collision chunks removed",
            "samples": sum(s["frames"] for s in baseline["shots"])}
    save("import_results.json", report)
    print("Import PASS;", len(checks), "static GLBs; scenery triangles", stats["total_city_scenery_render_triangles"])
    print("Frame ms", timings["overall"]["frame_ms"], "165 Hz target met:", report["performance"]["target_met"])


if __name__ == "__main__":
    main()
