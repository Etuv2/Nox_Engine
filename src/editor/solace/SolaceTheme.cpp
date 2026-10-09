// Theme and typography for the Solace UI kit.
// Palette values and the spacing/rounding scale are taken from Solace src/ui/foundation/theme.h
// and theme.cpp (MIT, Copyright (c) 2026 Pondot). Font loading is new: Solace embeds Geist as
// byte arrays and rasterizes with FreeType; NOX loads the TTFs from fonts/solace with stb_truetype.

#include "editor/solace/SolaceInternal.h"

#include <string>

namespace solace
{
namespace
{

// Solace theme.cpp k_dark / k_light, plus the status colours from theme.h and widgets.cpp.
constexpr Palette k_dark{
    IM_COL32(0xF2, 0xF2, 0xF2, 0xFF), // accent (Solace primary)
    IM_COL32(0x15, 0x15, 0x15, 0xFF), // accentText
    IM_COL32(0x15, 0x15, 0x15, 0xFF), // background
    IM_COL32(0x1C, 0x1C, 0x1C, 0xFF), // surface (card)
    IM_COL32(0x24, 0x24, 0x24, 0xFF), // surfaceRaised
    IM_COL32(0x2C, 0x2C, 0x2C, 0xFF), // surfacePressed
    IM_COL32(0x11, 0x11, 0x11, 0xFF), // inputFill
    IM_COL32(0xFF, 0xFF, 0xFF, 0x0D), // border
    IM_COL32(0xFF, 0xFF, 0xFF, 0x1A), // borderStrong
    IM_COL32(0xF2, 0xF2, 0xF2, 0xFF), // text
    IM_COL32(0x86, 0x86, 0x86, 0xFF), // textMuted
    IM_COL32(0x5A, 0x5A, 0x5A, 0xFF), // textSubtle
    IM_COL32(0x34, 0xD3, 0x99, 0xFF), // success (emerald_400)
    IM_COL32(0xFC, 0xBB, 0x00, 0xFF), // warning (amber_400)
    IM_COL32(0xEE, 0x34, 0x3B, 0xFF), // error (destructive)
    IM_COL32(0xF2, 0xF2, 0xF2, 0xFF), // info
    IM_COL32(0xF2, 0xF2, 0xF2, 0x66), // focusRing
    IM_COL32(0xFF, 0xFF, 0xFF, 0x1F), // selection
    IM_COL32(0xFC, 0xBB, 0x00, 0xFF), // dirty
};

constexpr Palette k_light{
    IM_COL32(0x0B, 0x0B, 0x0B, 0xFF), // accent
    IM_COL32(0xFC, 0xFC, 0xFC, 0xFF), // accentText
    IM_COL32(0xFC, 0xFC, 0xFC, 0xFF), // background
    IM_COL32(0xF5, 0xF5, 0xF5, 0xFF), // surface
    IM_COL32(0xEB, 0xEB, 0xEB, 0xFF), // surfaceRaised
    IM_COL32(0xE2, 0xE2, 0xE2, 0xFF), // surfacePressed
    IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), // inputFill
    IM_COL32(0x0B, 0x0B, 0x0B, 0x0F), // border
    IM_COL32(0x0B, 0x0B, 0x0B, 0x1F), // borderStrong
    IM_COL32(0x0B, 0x0B, 0x0B, 0xFF), // text
    IM_COL32(0x63, 0x63, 0x63, 0xFF), // textMuted
    IM_COL32(0x9A, 0x9A, 0x9A, 0xFF), // textSubtle
    IM_COL32(0x05, 0x96, 0x69, 0xFF), // success (emerald_600)
    IM_COL32(0xF9, 0x9C, 0x00, 0xFF), // warning (amber_500)
    IM_COL32(0xEE, 0x34, 0x3B, 0xFF), // error
    IM_COL32(0x0B, 0x0B, 0x0B, 0xFF), // info
    IM_COL32(0x0B, 0x0B, 0x0B, 0x66), // focusRing
    IM_COL32(0x0B, 0x0B, 0x0B, 0x14), // selection
    IM_COL32(0xF9, 0x9C, 0x00, 0xFF), // dirty
};

Palette g_palette = k_dark;
bool g_dark = true;
float g_dpi = 1.f;

ImFont* g_fonts[5] = {};

struct FaceSpec
{
    const char* file;
    float scale; // multiplier on the body size
    Face face;
};

constexpr FaceSpec k_faces[] = {
    {"Geist-Regular.ttf", 1.00f, Face::Regular},
    {"Geist-Medium.ttf", 1.00f, Face::Medium},
    {"Geist-SemiBold.ttf", 1.00f, Face::Semibold},
    {"Geist-Regular.ttf", 0.85f, Face::Caption},
    {"Geist-SemiBold.ttf", 1.25f, Face::Title},
};

void ApplyColours(const Palette& p, ImGuiStyle& s)
{
    ImVec4* c = s.Colors;
    const auto v = [](ImU32 col) { return ImGui::ColorConvertU32ToFloat4(col); };
    const ImVec4 transparent(0.f, 0.f, 0.f, 0.f);

    c[ImGuiCol_Text] = v(p.text);
    c[ImGuiCol_TextDisabled] = v(p.textSubtle);
    c[ImGuiCol_TextLink] = v(p.info);
    c[ImGuiCol_TextSelectedBg] = v(p.selection);

    c[ImGuiCol_WindowBg] = v(p.background);
    c[ImGuiCol_ChildBg] = v(p.background);
    c[ImGuiCol_PopupBg] = v(p.surface);
    c[ImGuiCol_Border] = v(p.border);
    c[ImGuiCol_BorderShadow] = transparent;

    c[ImGuiCol_FrameBg] = v(p.inputFill);
    c[ImGuiCol_FrameBgHovered] = v(p.surfaceRaised);
    c[ImGuiCol_FrameBgActive] = v(p.surfacePressed);

    c[ImGuiCol_TitleBg] = v(p.background);
    c[ImGuiCol_TitleBgActive] = v(p.surface);
    c[ImGuiCol_TitleBgCollapsed] = v(p.background);
    c[ImGuiCol_MenuBarBg] = v(p.background);

    c[ImGuiCol_ScrollbarBg] = transparent;
    c[ImGuiCol_ScrollbarGrab] = v(detail::WithAlpha(p.text, 0.16f));
    c[ImGuiCol_ScrollbarGrabHovered] = v(detail::WithAlpha(p.text, 0.28f));
    c[ImGuiCol_ScrollbarGrabActive] = v(detail::WithAlpha(p.text, 0.40f));

    c[ImGuiCol_CheckMark] = v(p.accent);
    c[ImGuiCol_SliderGrab] = v(p.accent);
    c[ImGuiCol_SliderGrabActive] = v(p.text);

    c[ImGuiCol_Button] = v(p.surface);
    c[ImGuiCol_ButtonHovered] = v(p.surfaceRaised);
    c[ImGuiCol_ButtonActive] = v(p.surfacePressed);

    c[ImGuiCol_Header] = v(detail::WithAlpha(p.text, 0.05f));
    c[ImGuiCol_HeaderHovered] = v(detail::WithAlpha(p.text, 0.08f));
    c[ImGuiCol_HeaderActive] = v(p.selection);

    c[ImGuiCol_Separator] = v(p.border);
    c[ImGuiCol_SeparatorHovered] = v(p.borderStrong);
    c[ImGuiCol_SeparatorActive] = v(p.textMuted);

    c[ImGuiCol_ResizeGrip] = transparent;
    c[ImGuiCol_ResizeGripHovered] = v(detail::WithAlpha(p.text, 0.10f));
    c[ImGuiCol_ResizeGripActive] = v(detail::WithAlpha(p.text, 0.20f));

    c[ImGuiCol_Tab] = v(p.background);
    c[ImGuiCol_TabHovered] = v(p.surfaceRaised);
    c[ImGuiCol_TabSelected] = v(p.surface);
    c[ImGuiCol_TabSelectedOverline] = v(p.accent);
    c[ImGuiCol_TabDimmed] = v(p.background);
    c[ImGuiCol_TabDimmedSelected] = v(p.surface);
    c[ImGuiCol_TabDimmedSelectedOverline] = v(p.textSubtle);

    c[ImGuiCol_DockingPreview] = v(detail::WithAlpha(p.text, 0.25f));
    c[ImGuiCol_DockingEmptyBg] = v(p.background);

    c[ImGuiCol_PlotLines] = v(p.textMuted);
    c[ImGuiCol_PlotLinesHovered] = v(p.text);
    c[ImGuiCol_PlotHistogram] = v(p.textMuted);
    c[ImGuiCol_PlotHistogramHovered] = v(p.text);

    c[ImGuiCol_TableHeaderBg] = v(p.surface);
    c[ImGuiCol_TableBorderStrong] = v(p.borderStrong);
    c[ImGuiCol_TableBorderLight] = v(p.border);
    c[ImGuiCol_TableRowBg] = transparent;
    c[ImGuiCol_TableRowBgAlt] = v(detail::WithAlpha(p.text, 0.02f));

    c[ImGuiCol_DragDropTarget] = v(detail::WithAlpha(p.accent, 0.90f));
    c[ImGuiCol_NavCursor] = v(p.focusRing);
    c[ImGuiCol_NavWindowingHighlight] = v(detail::WithAlpha(p.text, 0.70f));
    c[ImGuiCol_NavWindowingDimBg] = v(detail::WithAlpha(p.background, 0.60f));
    c[ImGuiCol_ModalWindowDimBg] = v(IM_COL32(0, 0, 0, 153));
}

} // namespace

const Palette& GetPalette()
{
    return g_palette;
}

bool IsDarkTheme()
{
    return g_dark;
}

float DpiScale()
{
    return g_dpi;
}

float Px(float dp)
{
    return dp * g_dpi;
}

void ApplyTheme(bool dark, float dpiScale)
{
    g_dark = dark;
    g_dpi = dpiScale > 0.f ? dpiScale : 1.f;
    g_palette = dark ? k_dark : k_light;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();

    // Spacing: Solace sp_* scale (4, 6, 8, 12, 16, 24 dp).
    s.WindowPadding = ImVec2(space::lg, space::lg);
    s.FramePadding = ImVec2(10.f, 6.f);
    s.ItemSpacing = ImVec2(space::md, space::sm);
    s.ItemInnerSpacing = ImVec2(space::sm, space::xs);
    s.CellPadding = ImVec2(space::md, space::xs);
    s.IndentSpacing = 16.f;
    s.ScrollbarSize = 10.f;
    s.GrabMinSize = 10.f;
    s.TouchExtraPadding = ImVec2(0.f, 0.f);
    s.SeparatorTextPadding = ImVec2(space::xl, space::xs);

    // Borders: Solace is flat. Popups keep a hairline; tabs use the overline only.
    s.WindowBorderSize = 0.f;
    s.ChildBorderSize = 0.f;
    s.PopupBorderSize = 1.f;
    s.FrameBorderSize = 0.f;
    s.TabBorderSize = 0.f;
    s.TabBarBorderSize = 1.f;
    s.TabBarOverlineSize = 2.f;
    s.DockingSeparatorSize = 2.f;
    s.SeparatorTextBorderSize = 1.f;

    // Rounding: Solace uses 12 for fields/buttons, 16 for toasts, 24 for large cards.
    s.WindowRounding = 10.f;
    s.ChildRounding = 12.f;
    s.FrameRounding = 10.f;
    s.PopupRounding = 12.f;
    s.ScrollbarRounding = 10.f;
    s.GrabRounding = 6.f;
    s.TabRounding = 8.f;

    s.WindowMenuButtonPosition = ImGuiDir_Right;
    s.WindowTitleAlign = ImVec2(0.f, 0.5f);
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.f, 0.5f);
    s.SeparatorTextAlign = ImVec2(0.f, 0.5f);
    s.DisplaySafeAreaPadding = ImVec2(space::sm, space::sm);

    // Every size above is in dp; scale them together (colours and ratios are unaffected).
    s.ScaleAllSizes(g_dpi);

    ApplyColours(g_palette, s);
}

bool LoadFonts(float sizePx, float dpiScale, const char* fontDir)
{
    ImGuiIO& io = ImGui::GetIO();
    const float body = (sizePx > 0.f ? sizePx : 14.f) * (dpiScale > 0.f ? dpiScale : 1.f);
    const std::string dir = fontDir ? fontDir : "";

    io.Fonts->Clear();
    ImFontConfig config;
    config.OversampleH = 2;
    config.PixelSnapH = true;

    ImFont* loaded[5] = {};
    bool ok = true;
    for (const FaceSpec& spec : k_faces)
    {
        const std::string path = dir.empty() ? std::string(spec.file) : dir + "/" + spec.file;
        loaded[(int)spec.face] = io.Fonts->AddFontFromFileTTF(path.c_str(), body * spec.scale, &config);
        if (!loaded[(int)spec.face])
            ok = false;
    }

    if (!ok)
    {
        io.Fonts->Clear();
        ImFont* fallback = io.Fonts->AddFontDefault();
        for (ImFont*& f : g_fonts)
            f = fallback;
        io.FontDefault = fallback;
        return false;
    }

    for (int i = 0; i < 5; i++)
        g_fonts[i] = loaded[i];
    io.FontDefault = g_fonts[(int)Face::Regular];
    return true;
}

ImFont* Font(Face face)
{
    const int i = (int)face;
    if (i < 0 || i >= 5 || !g_fonts[i])
        return ImGui::GetIO().FontDefault;
    return g_fonts[i];
}

} // namespace solace
