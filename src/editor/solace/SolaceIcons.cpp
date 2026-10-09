// Icon set for the Solace UI kit.
// Stroke geometry is Lucide (ISC licence, Copyright (c) 2022 Lucide Contributors). Path data
// marked "Solace" was copied from Solace src/ui/foundation/icons.cpp (MIT, Copyright (c) 2026
// Pondot); path data marked "Lucide" was written from the Lucide 24x24 set for NOX and should
// be checked against lucide.dev if pixel-exact glyphs are needed. See THIRD_PARTY.md.

#include "editor/solace/SolaceInternal.h"

namespace solace
{
namespace
{
constexpr float k_view_box = 24.f;

void stroke_path(ImDrawList* dl, const char* d, ImVec2 tl, float s, ImU32 col, float w)
{
    svg::stroke(dl, svg::path(d), tl, s, col, w);
}

void stroke_circle(ImDrawList* dl, float cx, float cy, float r, ImVec2 tl, float s, ImU32 col,
                   float w)
{
    svg::stroke(dl, svg::circle(cx, cy, r), tl, s, col, w);
}

void stroke_rect(ImDrawList* dl, float x, float y, float rw, float rh, float rx, ImVec2 tl,
                 float s, ImU32 col, float w)
{
    svg::stroke(dl, svg::rounded_rect(x, y, rw, rh, rx), tl, s, col, w);
}
} // namespace

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col, float strokeWidth)
{
    if (!dl || icon == Icon::None || size <= 0.f)
        return;

    const float s = size / k_view_box;
    const ImVec2 tl(center.x - size * 0.5f, center.y - size * 0.5f);
    const float w = strokeWidth;

    // Local shorthands keep the switch readable.
    auto path = [&](const char* d) { stroke_path(dl, d, tl, s, col, w); };
    auto circle = [&](float cx, float cy, float r) { stroke_circle(dl, cx, cy, r, tl, s, col, w); };
    auto rect = [&](float x, float y, float rw, float rh, float rx)
    { stroke_rect(dl, x, y, rw, rh, rx, tl, s, col, w); };

    switch (icon)
    {
    case Icon::Play:
        path("M6 3l14 9-14 9z");
        break;
    case Icon::Pause:
        rect(14, 3, 5, 18, 1);
        rect(5, 3, 5, 18, 1);
        break;
    case Icon::Stop:
        rect(3, 3, 18, 18, 2);
        break;
    case Icon::Save:
        path("M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z");
        path("M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7");
        path("M7 3v4a1 1 0 0 0 1 1h7");
        break;
    case Icon::Build: // hammer (Lucide)
        path("m15 12-8.373 8.373a1 1 0 1 1-3-3L12 9");
        path("m18 15 4-4");
        path("m21.5 11.5-1.914-1.914A2 2 0 0 1 19 8.172V7l-2.26-2.26a6 6 0 0 0-4.202-1.756L9 2.96l.92.82A6.18 6.18 0 0 1 12 8.4V10l2 2h1.172a2 2 0 0 1 1.414.586L18.5 14.5");
        break;
    case Icon::Run: // external link (Lucide)
        path("M15 3h6v6");
        path("M10 14 21 3");
        path("M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6");
        break;
    case Icon::Package: // Lucide
        path("M11 21.73a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73z");
        path("M12 22V12");
        path("m3.3 7 8.7 5 8.7-5");
        path("M7.5 4.27l9 5.15");
        break;
    case Icon::Folder: // Lucide
        path("M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z");
        break;
    case Icon::Search:
        path("m21 21-4.34-4.34");
        circle(11, 11, 8);
        break;
    case Icon::Eye:
        path("M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0");
        circle(12, 12, 3);
        break;
    case Icon::EyeOff:
        path("M10.733 5.076a10.744 10.744 0 0 1 11.205 6.575 1 1 0 0 1 0 .696 10.747 10.747 0 0 1-1.444 2.49");
        path("M14.084 14.158a3 3 0 0 1-4.242-4.242");
        path("M17.479 17.499a10.75 10.75 0 0 1-15.417-5.151 1 1 0 0 1 0-.696 10.75 10.75 0 0 1 4.446-5.143");
        path("m2 2 20 20");
        break;
    case Icon::Settings:
        path("M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915");
        circle(12, 12, 3);
        break;
    case Icon::Layers: // Lucide
        path("M12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z");
        path("M2 12a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 12");
        path("M2 17a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 17");
        break;
    case Icon::Flag: // Lucide
        path("M4 15s1-1 4-1 5 2 8 2 4-1 4-1V3s-1 1-4 1-5-2-8-2-4 1-4 1z");
        path("M4 22v-7");
        break;
    case Icon::Map: // Lucide
        path("M14.106 5.553a2 2 0 0 0 1.788 0l3.659-1.83A1 1 0 0 1 21 4.619v12.764a1 1 0 0 1-.553.894l-4.553 2.277a2 2 0 0 1-1.788 0l-4.212-2.106a2 2 0 0 0-1.788 0l-3.659 1.83A1 1 0 0 1 3 19.381V6.618a1 1 0 0 1 .553-.894l4.553-2.277a2 2 0 0 1 1.788 0z");
        path("M15 5.764v15");
        path("M9 3.236v15");
        break;
    case Icon::Camera: // Lucide
        path("M13.997 4a2 2 0 0 1 1.76 1.05l.486.9A2 2 0 0 0 18.003 7H20a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V9a2 2 0 0 1 2-2h1.997a2 2 0 0 0 1.75-1.05l.486-.9A2 2 0 0 1 10.003 4z");
        circle(12, 13, 3);
        break;
    case Icon::Light: // lightbulb (Lucide)
        path("M15 14c.2-1 .7-1.7 1.5-2.5 1-.9 1.5-2.2 1.5-3.5A6 6 0 0 0 6 8c0 1 .2 2.2 1.5 3.5.7.7 1.3 1.5 1.5 2.5");
        path("M9 18h6");
        path("M10 22h4");
        break;
    case Icon::Sun:
        circle(12, 12, 4);
        path("M12 2v2");
        path("M12 20v2");
        path("m4.93 4.93 1.41 1.41");
        path("m17.66 17.66 1.41 1.41");
        path("M2 12h2");
        path("M20 12h2");
        path("m6.34 17.66-1.41 1.41");
        path("m19.07 4.93-1.41 1.41");
        break;
    case Icon::Moon:
        path("M20.985 12.486a9 9 0 1 1-9.473-9.472c.405-.022.617.46.402.803a6 6 0 0 0 8.268 8.268c.344-.215.825-.004.803.401");
        break;
    case Icon::Undo: // Lucide undo-2
        path("M9 14 4 9l5-5");
        path("M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5 5.5 5.5 0 0 1-5.5 5.5H11");
        break;
    case Icon::Redo: // Lucide redo-2
        path("m15 14 5-5-5-5");
        path("M20 9H9.5A5.5 5.5 0 0 0 4 14.5 5.5 5.5 0 0 0 9.5 20H13");
        break;
    case Icon::Plus:
        path("M5 12h14");
        path("M12 5v14");
        break;
    case Icon::Copy: // Lucide
        rect(8, 8, 14, 14, 2);
        path("M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2");
        break;
    case Icon::Trash: // Lucide trash-2
        path("M3 6h18");
        path("M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6");
        path("M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2");
        path("M10 11v6");
        path("M14 11v6");
        break;
    case Icon::Refresh: // Lucide refresh-cw
        path("M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8");
        path("M21 3v5h-5");
        path("M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16");
        path("M8 16H3v5");
        break;
    case Icon::ChevronLeft:
        path("m15 18-6-6 6-6");
        break;
    case Icon::ChevronRight:
        path("m9 18 6-6-6-6");
        break;
    case Icon::ChevronUp:
        path("m18 15-6-6-6 6");
        break;
    case Icon::ChevronDown:
        path("m6 9 6 6 6-6");
        break;
    case Icon::Check:
        path("M20 6 9 17l-5-5");
        break;
    case Icon::Cross:
        path("M18 6 6 18");
        path("m6 6 12 12");
        break;
    case Icon::Info:
        circle(12, 12, 10);
        path("M12 16v-4");
        path("M12 8h.01");
        break;
    case Icon::Alert:
        circle(12, 12, 10);
        path("M12 8v4");
        path("M12 16h.01");
        break;
    case Icon::PanelLeft:
        rect(3, 3, 18, 18, 2);
        path("M9 3v18");
        break;
    case Icon::User:
        path("M19 21v-2a4 4 0 0 0-4-4H9a4 4 0 0 0-4 4v2");
        circle(12, 7, 4);
        break;
    case Icon::Mail:
        rect(2, 4, 20, 16, 2);
        path("m22 7-8.991 5.727a2 2 0 0 1-2.009 0L2 7");
        break;
    case Icon::Lock:
        rect(3, 11, 18, 11, 2);
        path("M7 11V7a5 5 0 0 1 10 0v4");
        break;
    case Icon::Sparkles:
        path("M11.017 2.814a1 1 0 0 1 1.966 0l1.051 5.558a2 2 0 0 0 1.594 1.594l5.558 1.051a1 1 0 0 1 0 1.966l-5.558 1.051a2 2 0 0 0-1.594 1.594l-1.051 5.558a1 1 0 0 1-1.966 0l-1.051-5.558a2 2 0 0 0-1.594-1.594l-5.558-1.051a1 1 0 0 1 0-1.966l5.558-1.051a2 2 0 0 0 1.594-1.594z");
        path("M20 2v4");
        path("M22 4h-4");
        circle(4, 20, 2);
        break;
    case Icon::Bell:
        path("M10.268 21a2 2 0 0 0 3.464 0");
        path("M3.262 15.326A1 1 0 0 0 4 17h16a1 1 0 0 0 .74-1.673C19.41 13.956 18 12.499 18 8A6 6 0 0 0 6 8c0 4.499-1.411 5.956-2.738 7.326");
        break;
    case Icon::Command:
        path("M15 6v12a3 3 0 1 0 3-3H6a3 3 0 1 0 3 3V6a3 3 0 1 0-3 3h12a3 3 0 1 0-3-3");
        break;
    case Icon::None:
        break;
    }
}

void DrawSpinnerArc(ImDrawList* dl, ImVec2 center, float radius, float thickness, float angle,
                    ImU32 col)
{
    if (!dl || (col & IM_COL32_A_MASK) == 0)
        return;

    constexpr int k_segments = 48;
    constexpr float k_sweep = 288.f * (IM_PI / 180.f);
    ImVec2 points[k_segments + 1];
    for (int i = 0; i <= k_segments; i++)
    {
        const float a = angle + k_sweep * ((float)i / (float)k_segments);
        points[i] = ImVec2(center.x + ImCos(a) * radius, center.y + ImSin(a) * radius);
    }
    dl->AddPolyline(points, k_segments + 1, col, ImDrawFlags_None, thickness);
    dl->AddCircleFilled(points[0], thickness * 0.5f, col, 12);
    dl->AddCircleFilled(points[k_segments], thickness * 0.5f, col, 12);
}

} // namespace solace
