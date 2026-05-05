from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    require(start >= 0, f"missing function: {signature}")
    brace = source.find("{", start)
    require(brace >= 0, f"missing function body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"unterminated function body: {signature}")


def main() -> None:
    rt_resources = read("src/RTSceneResources.cpp")
    bvh_builder = read("src/BVHBuilder.cpp")
    render_context = read("src/RenderContext.h")
    ui_h = read("src/ImGui/RenderingSettingsWindow.h")
    ui_cpp = read("src/ImGui/RenderingSettingsWindow.cpp")

    estimate_body = function_body(
        rt_resources,
        "std::size_t RTSceneResources::EstimateSceneTriangleCount")
    require("GetComponentManager()" in estimate_body and
            "GetRenderablePool()" in estimate_body and
            "ForEachRenderableMesh" in estimate_body and
            "node->GetModel()" not in estimate_body,
            "Surfel GI RT preflight must estimate ECS renderable meshes, not repeatedly count imported scene-node model pointers")

    build_body = function_body(
        bvh_builder,
        "RT::BVHData BVHBuilder::BuildFromScene")
    require("GetComponentManager()" in build_body and
            "GetTransformSystem()" in build_body and
            "GetRenderablePool()" in build_body and
            "ResolveRenderableMeshLocalTransform" in build_body,
            "shared RT BVH build must use renderer ECS renderables and their mesh-local transforms")
    require("node->GetModel()" not in build_body,
            "shared RT BVH build must not flatten every model pointer in the scene-node hierarchy")

    require("int surfelGIRTMaxTriangles = 2000000;" in render_context,
            "Shibuya-scale Surfel GI must not skip RT rays behind the legacy 250k triangle cap")
    require("m_surfelGIRTMaxTriangles = 2000000;" in ui_h and
            len(re.findall(r"m_surfelGIRTMaxTriangles = 2000000;", ui_cpp)) >= 2,
            "Rendering settings defaults/resets must expose the Shibuya-capable Surfel GI RT triangle budget")


if __name__ == "__main__":
    main()
