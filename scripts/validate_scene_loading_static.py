from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def normalize_asset_path(raw: str) -> Path:
    return ROOT / raw.replace("\\", "/")


def validate_scene_assets(scene_path: str) -> None:
    scene_file = ROOT / scene_path
    scene = json.loads(scene_file.read_text(encoding="utf-8"))
    require(isinstance(scene.get("nodes"), list) and scene["nodes"], f"{scene_path} must contain nodes")
    camera = scene.get("camera")
    require(isinstance(camera, dict), f"{scene_path} must contain deterministic top-level camera metadata")
    require(isinstance(camera.get("position"), list) and len(camera["position"]) == 3,
            f"{scene_path} camera.position must be a vec3")
    require("yaw" in camera and "pitch" in camera and "fov" in camera,
            f"{scene_path} camera must define yaw, pitch, and fov")

    for node in scene["nodes"]:
        if node.get("type") == "model":
            model_path = node.get("path", "")
            require(model_path, f"{scene_path} contains a model node without a path")
            require(normalize_asset_path(model_path).exists(), f"{scene_path} model asset is missing: {model_path}")

    skybox_path = scene.get("skybox", None)
    if isinstance(skybox_path, str) and skybox_path.strip():
        require(normalize_asset_path(skybox_path).exists(), f"{scene_path} skybox asset is missing: {skybox_path}")


def main() -> None:
    scene_loader_cpp = read("src/SceneLoader.cpp")
    core_cpp = read("src/Core.cpp")

    validate_scene_assets("scenes/surfel_gi_cornell_validation.json")
    validate_scene_assets("scenes/shibuya.json")

    cornell = json.loads((ROOT / "scenes/surfel_gi_cornell_validation.json").read_text(encoding="utf-8"))
    require(cornell.get("skybox") == "", "Cornell validation scene should intentionally disable skybox with an empty path")

    require("Skybox disabled" in scene_loader_cpp, "SceneLoader must explicitly skip empty skybox paths")
    require("ResolveReadableAssetPath" in scene_loader_cpp, "SceneLoader must resolve scene assets deterministically")
    require("return nullptr" in scene_loader_cpp, "SceneLoader must fail fatal scene loads instead of returning an empty graph")
    require("Scene load summary" in scene_loader_cpp, "SceneLoader must log deterministic load summary diagnostics")
    require("AddLight(entityID" in scene_loader_cpp, "SceneLoader must propagate loaded light nodes into ECS light components")
    require("AddCamera(entityID" in scene_loader_cpp, "SceneLoader must propagate loaded camera nodes into ECS camera components")

    require("ResolveSceneFilePath" in core_cpp, "Core must normalize NOX_FIRST_SCENE/config scene paths before loading")
    require("ApplySceneCameraOverride" in core_cpp, "Core must apply deterministic scene camera metadata")

    print("Scene loading static validation passed.")


if __name__ == "__main__":
    main()
