from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(relpath: str) -> str:
    return (ROOT / relpath).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    header_path = ROOT / "src/surfel_gi/SurfelGIResources.h"
    manager_path = ROOT / "src/passes/SurfelGIManager.h"
    source_path = ROOT / "src/surfel_gi/SurfelGIResources.cpp"
    glsl_path = ROOT / "shaders/includes/surfel_gi_resources.glsl"

    require(header_path.exists(), "Surfel GI resource owner header must exist")
    require(manager_path.exists(), "Surfel GI manager header must exist")
    require(source_path.exists(), "Surfel GI resource owner implementation must exist")
    require(glsl_path.exists(), "Surfel GI shared GLSL resource declarations must exist")

    header = header_path.read_text(encoding="utf-8")
    manager = manager_path.read_text(encoding="utf-8")
    source = source_path.read_text(encoding="utf-8")
    glsl = glsl_path.read_text(encoding="utf-8")
    context = read("src/RenderContext.h")
    project = read("Nox_Engine.vcxproj")

    require(
        "bool enabled = false" in manager and "bool enableSurfelGI = false" in context,
        "Surfel GI resource settings and runtime toggle must remain disabled by default",
    )
    require(
        all(token in header for token in ("class SurfelPool", "class SurfelGrid", "class SurfelRayQueue")),
        "header must declare SurfelPool, SurfelGrid, and SurfelRayQueue",
    )
    for method in (
        "Create(",
        "Destroy(",
        "ResizeReset(",
        "ResetFreeList(",
        "Clear(",
        "RequestRays(",
        "AllocateRays(",
        "GenerateRays(",
        "BinAndSortRays(",
        "TraceRays(",
        "IntegrateHits(",
    ):
        require(method in header, f"resource scaffold must declare {method}")

    require("enum class SurfelGIBinding" in header, "C++ shared bindings must be declared")
    for struct_name in (
        "Surfel",
        "SurfelCounters",
        "SurfelCellHeader",
        "SurfelRayRequest",
        "SurfelRay",
        "SurfelRayHit",
    ):
        require(
            re.search(rf"struct(?:\s+alignas\([^)]+\))?\s+{struct_name}\b", header) is not None,
            f"C++ shared layout struct {struct_name} must be declared",
        )
    require(
        all(token in header for token in (
            "static_assert(sizeof(Surfel)",
            "static_assert(sizeof(SurfelRay)",
            "static_assert(sizeof(SurfelRayHit)",
        )),
        "C++ std430 layout structs must include size checks",
    )
    require(
        all(token in glsl for token in (
            "#define B_SURFELS",
            "#define B_SURFEL_FREELIST",
            "#define B_SURFEL_COUNTERS",
            "#define B_GRID_HEADERS",
            "#define B_GRID_ENTRIES",
            "#define B_RAY_REQUESTS",
            "struct Surfel",
            "struct SurfelCounters",
            "struct SurfelRayRequest",
            "struct SurfelRay",
            "struct SurfelRayHit",
        )),
        "GLSL shared resource declarations must mirror the C++ binding and layout names",
    )
    require(
        "static constexpr uint32_t kInvalidIndex = 0xffffffffu" in header and
        "#define SURFEL_GI_INVALID_INDEX 0xffffffffu" in glsl,
        "C++ and GLSL declarations must share the invalid index sentinel",
    )

    for cls in ("SurfelPool", "SurfelGrid", "SurfelRayQueue"):
        pattern = rf"{cls}::{cls}\([^)]*\).*?= default|{cls}::~{cls}\([^)]*\).*?Destroy\(\)"
        require(re.search(pattern, source, flags=re.S), f"{cls} must have RAII destroy behavior")

    require(
        "glDeleteBuffers" in source and "glGenBuffers" in source and "glBufferData" in source,
        "implementation must allocate and destroy OpenGL SSBOs using the existing buffer style",
    )
    require(
        source.count("Destroy();") >= 3 and "CheckedBufferSize" in source,
        "Create/resize paths must reset existing buffers and guard byte-size overflow",
    )
    require(
        "std::vector<uint32_t> freeIndices" in source and "SurfelCounters counters" in source,
        "SurfelPool reset must initialize the free-list stack and counters safely",
    )
    require(
        "glClearBufferData" in source and "0xffffffffu" in source,
        "clear/reset paths must initialize SSBOs and invalid entry sentinels on the GPU",
    )
    require(
        "src\\surfel_gi\\SurfelGIResources.cpp" in project and
        "src\\surfel_gi\\SurfelGIResources.h" in project,
        "Visual Studio project must include the new resource scaffold files",
    )


if __name__ == "__main__":
    main()
