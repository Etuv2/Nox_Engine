// Status and feedback widgets for the Solace UI kit: badges, spinners, progress bars, dirty dot,
// tooltips and toast notifications. Layout and colours follow Solace src/ui/controls/widgets.cpp
// (badge, toast) and loader.cpp (MIT, Copyright (c) 2026 Pondot). Toast motion is a simple
// fade and slide instead of Solace's springs.

#include "editor/solace/SolaceInternal.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace solace
{
namespace
{

struct Toast
{
    std::string title;
    std::string body;
    Status status = Status::Neutral;
    float age = 0.f;
    float remaining = 4.f;
};

std::vector<Toast>& Toasts()
{
    static std::vector<Toast> toasts;
    return toasts;
}

constexpr size_t k_max_toasts = 5;
constexpr float k_toast_enter = 0.22f;
constexpr float k_toast_exit = 0.30f;

ImU32 StatusColour(Status status)
{
    const Palette& p = GetPalette();
    switch (status)
    {
    case Status::Info:
        return p.info;
    case Status::Success:
        return p.success;
    case Status::Warning:
        return p.warning;
    case Status::Error:
        return p.error;
    case Status::Neutral:
    default:
        return p.textMuted;
    }
}

Icon StatusIcon(Status status)
{
    switch (status)
    {
    case Status::Info:
        return Icon::Info;
    case Status::Success:
        return Icon::Check;
    case Status::Warning:
        return Icon::Alert;
    case Status::Error:
        return Icon::Cross;
    case Status::Neutral:
    default:
        return Icon::None;
    }
}

} // namespace

void Badge(const char* label, Status status)
{
    const Palette& p = GetPalette();
    ImFont* font = Font(Face::Caption);
    const char* end = detail::VisibleLabelEnd(label);
    const ImVec2 text = detail::MeasureText(font, label, end);
    const ImU32 tone = StatusColour(status);

    const float h = Px(20.f);
    const float w = text.x + Px(16.f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + w, min.y + h);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = status == Status::Neutral ? detail::WithAlpha(p.text, 0.06f)
                                                 : detail::WithAlpha(tone, 0.12f);
    const ImU32 edge = status == Status::Neutral ? p.border : detail::WithAlpha(tone, 0.35f);
    dl->AddRectFilled(min, max, fill, h * 0.5f);
    dl->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f), edge,
                h * 0.5f, ImDrawFlags_None, Px(1.f));
    detail::DrawTextLeft(dl, font, ImVec2(min.x + Px(8.f), min.y + (h - text.y) * 0.5f),
                         tone, label, end);

    ImGui::Dummy(ImVec2(w, h));
}

void Spinner(float diameter, ImU32 color)
{
    const Palette& p = GetPalette();
    const float d = Px(diameter);
    const float thickness = Px(2.f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 centre(min.x + d * 0.5f, min.y + d * 0.5f);
    const float radius = d * 0.5f - thickness * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = color ? color : p.text;
    dl->AddCircle(centre, radius, detail::WithAlpha(col, 0.15f), 32, thickness);
    DrawSpinnerArc(dl, centre, radius, thickness, (float)ImGui::GetTime() * 5.f, col);

    ImGui::Dummy(ImVec2(d, d));
}

void ProgressBar(float fraction, float width)
{
    const Palette& p = GetPalette();
    const float w = width > 0.f ? Px(width) : ImGui::GetContentRegionAvail().x;
    const float h = Px(6.f);
    const float f = std::clamp(fraction, 0.f, 1.f);

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + w, min.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, detail::WithAlpha(p.text, 0.10f), h * 0.5f);
    if (f > 0.f)
        dl->AddRectFilled(min, ImVec2(min.x + w * f, max.y), p.accent, h * 0.5f);

    ImGui::Dummy(ImVec2(w, h));
}

void IndeterminateProgressBar(float width)
{
    const Palette& p = GetPalette();
    const float w = width > 0.f ? Px(width) : ImGui::GetContentRegionAvail().x;
    const float h = Px(6.f);

    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + w, min.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, detail::WithAlpha(p.text, 0.10f), h * 0.5f);

    // A 30% segment sweeps from fully left of the track to fully right of it.
    const float t = std::fmod((float)ImGui::GetTime() * 0.8f, 1.f);
    const float seg = w * 0.30f;
    const float x0 = min.x - seg + (w + seg) * t;
    const float a = std::max(min.x, x0);
    const float b = std::min(max.x, x0 + seg);
    if (b > a)
        dl->AddRectFilled(ImVec2(a, min.y), ImVec2(b, max.y), p.accent, h * 0.5f);

    ImGui::Dummy(ImVec2(w, h));
}

void DirtyDot(bool dirty, float diameter)
{
    const Palette& p = GetPalette();
    const float d = Px(diameter);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    if (dirty)
    {
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(min.x + d * 0.5f, min.y + d * 0.5f),
                                                    d * 0.5f, p.dirty, 16);
    }
    ImGui::Dummy(ImVec2(d, d));
}

void TooltipOnHover(const char* text)
{
    if (!text || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_ForTooltip))
        return;

    const Palette& p = GetPalette();
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(p.border));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.text));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(10.f), Px(6.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, Px(1.f));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(text);
    ImGui::EndTooltip();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
}

void PushToast(const char* title, const char* body, Status status, float seconds)
{
    auto& toasts = Toasts();
    Toast t;
    t.title = title ? title : "";
    t.body = body ? body : "";
    t.status = status;
    t.remaining = seconds > 0.f ? seconds : 4.f;
    toasts.push_back(std::move(t));
    if (toasts.size() > k_max_toasts)
        toasts.erase(toasts.begin());
}

void DrawToasts(const ImRect& area, ToastCorner corner)
{
    auto& toasts = Toasts();
    if (toasts.empty())
        return;

    const Palette& p = GetPalette();
    const float dt = detail::FrameDt();
    const float margin = Px(space::xxl);
    const float gap = Px(space::md);
    const float width = std::min(Px(360.f), area.GetWidth() - margin * 2.f);
    const float pad = Px(space::lg);
    const float title_h = Px(20.f);
    const float body_h = Px(16.f);
    const float bottom_gap = Px(2.f);

    ImFont* title_font = Font(Face::Medium);
    ImFont* body_font = Font(Face::Caption);
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Age and expire first, then measure, so stacking can run from either edge.
    for (Toast& t : toasts)
    {
        t.age += dt;
        t.remaining -= dt;
    }
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
                                [](const Toast& t) { return t.remaining <= 0.f; }),
                 toasts.end());
    if (toasts.empty())
        return;

    std::vector<float> heights;
    heights.reserve(toasts.size());
    for (const Toast& t : toasts)
    {
        const float body_lines = t.body.empty() ? 0.f : body_h + bottom_gap;
        heights.push_back(pad * 2.f + title_h + body_lines);
    }

    const bool top = corner == ToastCorner::TopLeft || corner == ToastCorner::TopRight;
    const bool right = corner == ToastCorner::TopRight || corner == ToastCorner::BottomRight;
    const float x = right ? area.Max.x - margin - width : area.Min.x + margin;

    // Top corners stack downward from the newest-first order; bottom corners stack upward so
    // the newest toast sits nearest the edge.
    std::vector<float> tops(toasts.size());
    if (top)
    {
        float y = area.Min.y + margin;
        for (size_t i = 0; i < toasts.size(); i++)
        {
            tops[i] = y;
            y += heights[i] + gap;
        }
    }
    else
    {
        float y = area.Max.y - margin;
        for (size_t i = toasts.size(); i-- > 0;)
        {
            y -= heights[i];
            tops[i] = y;
            y -= gap;
        }
    }

    for (size_t i = 0; i < toasts.size(); i++)
    {
        const Toast& t = toasts[i];
        const float enter = std::clamp(t.age / k_toast_enter, 0.f, 1.f);
        const float leave = std::clamp(t.remaining / k_toast_exit, 0.f, 1.f);
        const float alpha = enter * leave;
        const float slide = (1.f - enter) * Px(16.f) * (right ? 1.f : -1.f);

        const ImVec2 min(x + slide, tops[i]);
        const ImVec2 max(min.x + width, min.y + heights[i]);
        const float rounding = Px(14.f);

        dl->AddRectFilled(min, max, detail::WithAlpha(p.surface, 0.97f * alpha), rounding);
        dl->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f),
                    detail::WithAlpha(p.border, alpha), rounding, ImDrawFlags_None, Px(1.f));

        float text_x = min.x + pad;
        const Icon icon = StatusIcon(t.status);
        if (icon != Icon::None)
        {
            const float icon_size = Px(16.f);
            DrawIcon(dl, icon,
                     ImVec2(min.x + pad + icon_size * 0.5f, min.y + pad + title_h * 0.5f),
                     icon_size, detail::WithAlpha(StatusColour(t.status), alpha));
            text_x += icon_size + Px(space::sm);
        }

        detail::DrawTextLeft(dl, title_font, ImVec2(text_x, min.y + pad), detail::WithAlpha(p.text, alpha),
                             t.title.c_str());
        if (!t.body.empty())
            detail::DrawTextLeft(dl, body_font, ImVec2(text_x, min.y + pad + title_h + bottom_gap),
                                 detail::WithAlpha(p.textMuted, alpha), t.body.c_str());
    }
}

} // namespace solace
