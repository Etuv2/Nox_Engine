"""Static structural checks for the surfel GI pipeline (current design).

Checks that hold on the tree as it is now: every compute shader the pipeline loads exists, the GPU
struct layouts match between C++ and GLSL, the reference mode and shared-trace include are wired,
and the removed WP1 names are gone. Run: python scripts/validate_surfel_gi_static.py
"""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def cpp_struct_fields(source: str, name: str) -> list[tuple[str, str]]:
    """(type, name) of each data member of a C++ struct, in declaration order."""
    body = re.search(r"struct\s+(?:alignas\(\d+\)\s+)?" + name + r"\s*\{(.*?)\n\};", source, re.S)
    require(body is not None, f"C++ struct {name} not found")
    fields = []
    for line in body.group(1).splitlines():
        m = re.match(r"\s*(glm::u?vec4|uint32_t|float)\s+(\w+)", line)
        if m:
            fields.append((m.group(1), m.group(2)))
    return fields


def glsl_struct_fields(source: str, name: str) -> list[tuple[str, str]]:
    """(type, name) of each member of a GLSL struct, in declaration order."""
    body = re.search(r"struct\s+" + name + r"\s*\{(.*?)\n\};", source, re.S)
    require(body is not None, f"GLSL struct {name} not found")
    fields = []
    for line in body.group(1).splitlines():
        m = re.match(r"\s*(uvec4|vec4|uint|float)\s+(\w+)", line)
        if m:
            fields.append((m.group(1), m.group(2)))
    return fields


def check_struct_layout() -> None:
    resources_h = read("src/surfel_gi/SurfelGIResources.h")
    common = read("shaders/surfel_gi/common.glsl")

    cpp_counters = cpp_struct_fields(resources_h, "SurfelCounters")
    glsl_counters = glsl_struct_fields(common, "SurfelCounters")
    require([n for _, n in cpp_counters] == [n for _, n in glsl_counters],
            "SurfelCounters field names or order differ between SurfelGIResources.h and common.glsl")
    require(len(cpp_counters) * 4 == 288, "SurfelCounters must be 72 uint32 fields (288 bytes)")
    require("static_assert(sizeof(SurfelCounters) == 288" in resources_h,
            "SurfelCounters size assert missing or changed")

    cpp_surfel = cpp_struct_fields(resources_h, "Surfel")
    glsl_surfel = glsl_struct_fields(common, "Surfel")
    require([t.replace("glm::", "") for t, _ in cpp_surfel] == [t for t, _ in glsl_surfel],
            "Surfel member types or order differ between C++ and GLSL")
    require(len(cpp_surfel) == 13, "Surfel must keep its 13 members")

    require("static_assert(sizeof(Surfel) == 208" in resources_h, "Surfel size assert missing or changed")


def check_pipeline_shaders() -> None:
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    paths = re.findall(r'CreateCompute\([^,]+,\s*"(shaders/surfel_gi/[^"]+)"\)', pipeline)
    require(len(paths) >= 15, f"expected at least 15 compute shaders in the pipeline, found {len(paths)}")
    for relpath in paths:
        require((ROOT / relpath).exists(), f"pipeline loads a missing shader: {relpath}")
    require((ROOT / "shaders/surfel_gi/reference_gi.comp").exists(), "reference_gi.comp is missing")
    require('CreateCompute(m_referenceShader, "shaders/surfel_gi/reference_gi.comp")' in pipeline,
            "reference shader is not loaded in reference mode")


def check_shared_trace_include() -> None:
    trace = read("shaders/surfel_gi/trace_rays.comp")
    reference = read("shaders/surfel_gi/reference_gi.comp")
    shared = read("shaders/surfel_gi/surfel_scene_trace.glsl")
    require('#include "surfel_scene_trace.glsl"' in trace, "trace_rays.comp must include the shared BVH/light include")
    require('#include "surfel_scene_trace.glsl"' in reference, "reference_gi.comp must include the shared BVH/light include")
    require("vec3 TraceSoftwareBVH" not in trace and "SoftwareBVHHit TraceSoftwareBVH" not in trace,
            "software BVH traversal must live only in the shared include")
    require("SoftwareBVHHit TraceSoftwareBVH(" in shared, "shared include lost TraceSoftwareBVH")
    require("EvaluateHitDirectIrradiance(" in shared, "shared include lost the direct-light helper")
    require("uniform mat4 uView;" not in trace, "uView must be declared once (in the shared include)")


def check_reference_wiring() -> None:
    modular = read("src/ModularRenderer.cpp")
    pipeline = read("src/passes/SurfelGIPipeline.cpp")
    reference = read("shaders/surfel_gi/reference_gi.comp")
    require('IsEnvVarEnabled("NOX_GI_REFERENCE")' in modular, "NOX_GI_REFERENCE is not read by ModularRenderer")
    require("if (m_settings.referenceMode) {" in pipeline and "ExecuteIndependentReference(" in pipeline,
            "reference mode does not own the frame in Execute")
    require("ApplyIndependentReference(context);" in pipeline, "reference mode does not resample in ApplyIndirect")
    require("B_SURFELS" not in reference and "B_GRID" not in reference,
            "the independent reference must not read surfel or grid buffers")


def check_camera_and_exposure_hooks() -> None:
    require((ROOT / "src/CameraPath.cpp").exists() and (ROOT / "src/CameraPath.h").exists(),
            "CameraPath source files are missing")
    core = read("src/Core.cpp")
    require('GetEnvVarString("NOX_CAMERA_PATH")' in core and "cameraPath.Apply(*m_camera);" in core,
            "NOX_CAMERA_PATH is not applied in the Core update loop")
    scene_settings = read("src/SceneRenderSettings.cpp")
    require('std::getenv("NOX_EXPOSURE_MODE")' in scene_settings, "NOX_EXPOSURE_MODE override is missing")


def check_removed_names_absent() -> None:
    removed = [
        "BinAndSortRays", "GridAverages", "SortedRays", "rayBinBuffer", "sortedRayBuffer",
        "gridAverageBuffer", "useRayBinning", "radialDepthUpdateInterval", "fastFillSpawnPasses",
        "rayHitSourceDirect", "sourceDirectSamples", "B_RAY_BINS", "B_GRID_AVERAGES",
        "SURFEL_REQUEST_CACHE_MAINTENANCE_PERIOD", "SURFEL_GENERATE_MAX_RAYS_PER_SURFEL",
    ]
    sources = [
        "src/passes/SurfelGIPipeline.cpp", "src/passes/SurfelGIPipeline.h", "src/passes/SurfelGIManager.h",
        "src/surfel_gi/SurfelGIResources.h", "src/surfel_gi/SurfelGIResources.cpp", "src/ModularRenderer.cpp",
        "src/ImGui/RenderingSettingsWindow.cpp", "src/RenderContext.h",
        "shaders/surfel_gi/common.glsl", "shaders/surfel_gi/integrate.comp", "shaders/surfel_gi/generate_rays.comp",
        "shaders/surfel_gi/trace_rays.comp", "shaders/surfel_gi/request_rays.comp",
    ]
    for relpath in sources:
        text = read(relpath)
        for name in removed:
            require(re.search(r"\b" + name + r"\b", text) is None, f"removed name '{name}' still in {relpath}")


def check_msme_writes() -> None:
    msme = read("shaders/surfel_gi/msme.comp")
    require(re.search(r"surfel\.msme\.(z|w)\b", msme) is None, "msme.zw is written again; no reader uses it")


def main() -> None:
    check_struct_layout()
    check_pipeline_shaders()
    check_shared_trace_include()
    check_reference_wiring()
    check_camera_and_exposure_hooks()
    check_removed_names_absent()
    check_msme_writes()
    print("validate_surfel_gi_static: OK")


if __name__ == "__main__":
    main()
