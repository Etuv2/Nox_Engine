from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    apply_shader = read("shaders/surfel_gi/apply_indirect.comp")
    manager_h = read("src/passes/SurfelGIManager.h")
    manager_cpp = read("src/passes/SurfelGIManager.cpp")
    pipeline_h = read("src/passes/SurfelGIPipeline.h")
    pipeline_cpp = read("src/passes/SurfelGIPipeline.cpp")
    modular_cpp = read("src/ModularRenderer.cpp")
    ui_cpp = read("src/ImGui/RenderingSettingsWindow.cpp")

    require("DEBUG_RAW_INDIRECT_IRRADIANCE" in apply_shader,
            "apply pass must expose raw final-gather irradiance before material response")
    require("DebugHDRIrradianceColor" in apply_shader,
            "raw irradiance debug view must tonemap HDR values for direct fullscreen inspection")
    require("RawIndirectIrradiance = 35" in manager_h,
            "SurfelGI debug enum must expose raw indirect irradiance")
    require("RawIndirectIrradiance" in manager_cpp and "return false;" in manager_cpp,
            "raw irradiance debug view must be treated as fullscreen, not overlay surfel disks")
    require("m_settings.debugView == SurfelGIDebugView::Off" in manager_cpp and
            "!IsSurfelOverlayDebugView(m_settings.debugView)" not in manager_cpp,
            "SurfelGIManager::RenderDebug must forward fullscreen diagnostics to the pipeline")
    require('"Raw Irradiance"' in ui_cpp,
            "rendering settings UI must expose the raw irradiance diagnostic")
    require("std::clamp(m_context.cleanSurfelGIDebugView, 0, 35)" in modular_cpp and
            'NOX_SURFEL_GI_DEBUG_VIEW", static_cast<int>(settings.debugView)), 0, 35)' in modular_cpp,
            "environment and UI debug view clamps must include raw irradiance")
    require("m_debugPresentProgram" in pipeline_h and
            "indirect_diffuse_debug_present_frag.glsl" in pipeline_cpp,
            "fullscreen surfel diagnostics must present the indirect texture directly to the backbuffer")
    require("IsSurfelFullscreenDebugView(m_settings.debugView)" in pipeline_cpp and
            "context.screenQuad->Render()" in pipeline_cpp,
            "surfel fullscreen debug views must bypass deferred material response during presentation")


if __name__ == "__main__":
    main()
