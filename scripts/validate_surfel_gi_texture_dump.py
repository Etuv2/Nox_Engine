from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    pipeline_h = read("src/passes/SurfelGIPipeline.h")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")

    require("NOX_SURFEL_GI_DUMP_DIR" in pipeline_cpp,
            "Surfel GI pipeline must expose an env-controlled texture dump directory")
    require("NOX_SURFEL_GI_DUMP_FRAME" in pipeline_cpp,
            "Surfel GI pipeline must allow deterministic dump frame selection")
    require("NOX_SURFEL_GI_DUMP_FRAME_COUNT" in pipeline_cpp and
            "NOX_SURFEL_GI_DUMP_FRAME_INTERVAL" in pipeline_cpp and
            "frameSuffix" in pipeline_cpp,
            "texture dump must support same-run consecutive-frame stability captures")
    require("NOX_SURFEL_GI_DUMP_FRAMES" in pipeline_cpp and
            "EnvFrameListContains" in pipeline_cpp,
            "texture dump must support exact frame-list captures for runtime GI validation")
    require("SaveSurfelDebugTexturePNG" in pipeline_cpp,
            "Surfel GI texture dumps must write direct PNG evidence from GL textures")
    require("rawIndirectTexture" in pipeline_cpp and
            "filteredIndirectTexture" in pipeline_cpp and
            "indirectTexture" in pipeline_cpp,
            "texture dump must include raw, filtered, and temporal/final indirect textures")
    require("m_lastTextureDumpFrame" in pipeline_h,
            "pipeline must avoid dumping validation textures every frame")


if __name__ == "__main__":
    main()
