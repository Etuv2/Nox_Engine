// Interactive controls for the Solace UI kit.
// Layout, colours, rounding and interaction states follow Solace src/ui/controls/widgets.cpp,
// form_controls.cpp and src/ui/screens/shell_layout.cpp (MIT, Copyright (c) 2026 Pondot), with
// ImGui 1.91.9 items (InvisibleButton + draw list) replacing Solace's Win32 hit testing and
// spring-based motion. Hover/press/focus use exponential approach through detail::Anim.

#include "editor/solace/SolaceInternal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <string>
#include <vector>

namespace solace
{
namespace
{
using detail::Anim;
using detail::AnimState;
using detail::Approach;
using detail::DrawFocusRing;
using detail::DrawTextLeft;
using detail::FrameDt;
using detail::KeyboardFocused;
using detail::Mix;
using detail::MeasureText;
using detail::VisibleLabelEnd;
using detail::WithAlpha;

constexpr float k_hover_rate = 14.f;
constexpr float k_press_rate = 30.f;
constexpr float k_focus_rate = 16.f;
constexpr float k_slide_rate = 18.f;

// Updates hover/press/focus of an item. Call right after the InvisibleButton for that item.
AnimState& UpdateItemState(bool hovered, bool held, bool focused, float dt)
{
    AnimState& a = Anim(ImGui::GetItemID());
    a.hover = Approach(a.hover, hovered ? 1.f : 0.f, k_hover_rate, dt);
    a.press = Approach(a.press, held ? 1.f : 0.f, k_press_rate, dt);
    a.focus = Approach(a.focus, focused ? 1.f : 0.f, k_focus_rate, dt);
    return a;
}

// Position of the glyph/label block inside a rect, vertically centred.
ImVec2 CentredY(ImFont* font, const ImVec2& min, float height, float x)
{
    const float lh = font ? font->FontSize : 0.f;
    return ImVec2(x, min.y + (height - lh) * 0.5f);
}

// Icon-button colours shared by IconButton and ToolbarButton.
void DrawGhostSurface(ImDrawList* dl, const ImVec2& min, const ImVec2& max, float rounding,
                      bool active, float hover)
{
    const Palette& p = GetPalette();
    ImU32 bg = WithAlpha(p.text, 0.0f);
    bg = Mix(bg, WithAlpha(p.text, 0.06f), hover);
    if (active)
        bg = WithAlpha(p.text, 0.12f);
    if (bg & IM_COL32_A_MASK)
        dl->AddRectFilled(min, max, bg, rounding);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------------

bool Button(const char* label, ButtonKind kind, ImVec2 size, bool enabled)
{
    const Palette& p = GetPalette();
    ImFont* font = Font(Face::Medium);
    const char* end = VisibleLabelEnd(label);
    const ImVec2 text = MeasureText(font, label, end);
    const float h = size.y > 0.f ? size.y : Px(36.f);
    const float w = size.x > 0.f ? size.x : text.x + Px(32.f);

    ImGui::PushID(label);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + w, min.y + h);

    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##solace_button", ImVec2(w, h));
    ImGui::EndDisabled();

    AnimState& a = UpdateItemState(enabled && ImGui::IsItemHovered(), enabled && ImGui::IsItemActive(),
                                   enabled && KeyboardFocused(), FrameDt());

    ImU32 base = 0, hover = 0, pressed = 0, text_col = 0, border = 0, border_hover = 0;
    switch (kind)
    {
    case ButtonKind::Primary:
        base = p.accent;
        hover = Mix(p.accent, p.accentText, 0.08f);
        pressed = Mix(p.accent, p.accentText, 0.16f);
        text_col = p.accentText;
        break;
    case ButtonKind::Ghost:
        base = WithAlpha(p.text, 0.f);
        hover = WithAlpha(p.text, 0.06f);
        pressed = WithAlpha(p.text, 0.10f);
        text_col = p.text;
        break;
    case ButtonKind::Destructive:
        base = WithAlpha(p.error, 0.08f);
        hover = WithAlpha(p.error, 0.14f);
        pressed = WithAlpha(p.error, 0.22f);
        border = WithAlpha(p.error, 0.40f);
        border_hover = border;
        text_col = p.error;
        break;
    case ButtonKind::Secondary:
    default:
        base = p.surface;
        hover = p.surfaceRaised;
        pressed = p.surfacePressed;
        border = p.border;
        border_hover = p.borderStrong;
        text_col = p.text;
        break;
    }

    const float alpha = enabled ? 1.f : 0.4f;
    const float rounding = Px(10.f);
    const ImU32 bg = Mix(Mix(base, hover, a.hover), pressed, a.press);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, WithAlpha(bg, alpha), rounding);
    if (border & IM_COL32_A_MASK)
    {
        const ImU32 bd = Mix(border, border_hover, a.hover);
        dl->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f),
                    WithAlpha(bd, alpha), rounding, ImDrawFlags_None, Px(1.f));
    }
    DrawFocusRing(dl, min, max, rounding, a.focus);

    const ImVec2 tp(min.x + (w - text.x) * 0.5f, min.y + (h - text.y) * 0.5f);
    DrawTextLeft(dl, font, tp, WithAlpha(text_col, alpha), label, end);

    ImGui::PopID();
    return clicked && enabled;
}

bool IconButton(const char* id, Icon icon, const char* tooltip, bool active, bool enabled,
                float size)
{
    const Palette& p = GetPalette();
    const float s = Px(size);

    ImGui::PushID(id);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + s, min.y + s);

    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##solace_icon", ImVec2(s, s));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    AnimState& a = UpdateItemState(hovered, enabled && ImGui::IsItemActive(),
                                   enabled && KeyboardFocused(), FrameDt());
    TooltipOnHover(tooltip);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float alpha = enabled ? 1.f : 0.4f;
    DrawGhostSurface(dl, min, max, Px(8.f), active, a.hover);
    DrawFocusRing(dl, min, max, Px(8.f), a.focus);

    const ImU32 fg = active ? p.text : Mix(p.textMuted, p.text, a.hover);
    DrawIcon(dl, icon, ImVec2(min.x + s * 0.5f, min.y + s * 0.5f), s * 0.5f,
             WithAlpha(fg, alpha));

    ImGui::PopID();
    return clicked && enabled;
}

bool ToolbarButton(const char* id, Icon icon, const char* label, bool active, bool enabled,
                   const char* tooltip)
{
    const Palette& p = GetPalette();
    ImFont* font = Font(Face::Medium);
    const char* end = VisibleLabelEnd(label);
    const ImVec2 text = MeasureText(font, label, end);

    const float h = Px(32.f);
    const float pad = Px(10.f);
    const float icon_size = Px(16.f);
    const float gap = Px(6.f);
    const float w = pad + icon_size + gap + text.x + pad;

    ImGui::PushID(id);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + w, min.y + h);

    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##solace_toolbar", ImVec2(w, h));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    AnimState& a = UpdateItemState(hovered, enabled && ImGui::IsItemActive(),
                                   enabled && KeyboardFocused(), FrameDt());
    TooltipOnHover(tooltip);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float alpha = enabled ? 1.f : 0.4f;
    DrawGhostSurface(dl, min, max, Px(8.f), active, a.hover);
    DrawFocusRing(dl, min, max, Px(8.f), a.focus);

    const ImU32 fg = (active || hovered) ? p.text : p.textMuted;
    DrawIcon(dl, icon, ImVec2(min.x + pad + icon_size * 0.5f, min.y + h * 0.5f), icon_size,
             WithAlpha(fg, alpha));
    DrawTextLeft(dl, font, CentredY(font, min, h, min.x + pad + icon_size + gap), WithAlpha(fg, alpha),
                 label, end);

    ImGui::PopID();
    return clicked && enabled;
}

bool NavItem(const char* id, Icon icon, const char* label, bool selected, const char* trailing,
             float width, bool enabled)
{
    const Palette& p = GetPalette();
    ImFont* font = Font(Face::Regular);
    const char* end = VisibleLabelEnd(label);
    const float h = Px(34.f);
    const float w = width > 0.f ? Px(width) : ImGui::GetContentRegionAvail().x;
    const float pad = Px(12.f);
    const float icon_size = Px(16.f);
    const float gap = Px(10.f);

    ImGui::PushID(id);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + w, min.y + h);

    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##solace_nav", ImVec2(w, h));
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    AnimState& a = UpdateItemState(hovered, enabled && ImGui::IsItemActive(),
                                   enabled && KeyboardFocused(), FrameDt());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float alpha = enabled ? 1.f : 0.4f;
    const ImU32 bg = selected ? WithAlpha(p.text, 0.10f) : Mix(WithAlpha(p.text, 0.f), WithAlpha(p.text, 0.05f), a.hover);
    if (bg & IM_COL32_A_MASK)
        dl->AddRectFilled(min, max, WithAlpha(bg, alpha), Px(10.f));
    DrawFocusRing(dl, min, max, Px(10.f), a.focus);

    float text_x = min.x + pad;
    if (icon != Icon::None)
    {
        const ImU32 icon_col = selected ? p.text : Mix(p.textMuted, p.text, a.hover);
        DrawIcon(dl, icon, ImVec2(min.x + pad + icon_size * 0.5f, min.y + h * 0.5f), icon_size,
                 WithAlpha(icon_col, alpha));
        text_x += icon_size + gap;
    }

    const ImU32 label_col = selected ? p.text : Mix(p.textMuted, p.text, a.hover);
    DrawTextLeft(dl, font, CentredY(font, min, h, text_x), WithAlpha(label_col, alpha), label, end);

    if (trailing && *trailing)
    {
        const ImVec2 tsz = MeasureText(font, trailing);
        DrawTextLeft(dl, font, CentredY(font, min, h, max.x - pad - tsz.x),
                     WithAlpha(p.textMuted, alpha), trailing);
    }

    ImGui::PopID();
    return clicked && enabled;
}

// ---------------------------------------------------------------------------------------------
// Toggle and segmented control
// ---------------------------------------------------------------------------------------------

bool ToggleSwitch(const char* id, bool* value, bool enabled, const char* label)
{
    const Palette& p = GetPalette();
    ImFont* font = Font(Face::Regular);
    const char* end = VisibleLabelEnd(label);
    const float w = Px(40.f);
    const float h = Px(22.f);
    const float inset = Px(3.f);
    const float label_gap = Px(12.f);
    const ImVec2 label_size = label ? MeasureText(font, label, end) : ImVec2(0.f, 0.f);
    const float total_w = w + (label ? label_gap + label_size.x : 0.f);

    ImGui::PushID(id);
    const ImVec2 min = ImGui::GetCursorScreenPos();

    ImGui::BeginDisabled(!enabled);
    bool changed = ImGui::InvisibleButton("##solace_switch", ImVec2(total_w, h));
    ImGui::EndDisabled();
    if (changed && value)
        *value = !*value;
    else
        changed = false;

    const bool hovered = enabled && ImGui::IsItemHovered();
    AnimState& a = UpdateItemState(hovered, enabled && ImGui::IsItemActive(),
                                   enabled && KeyboardFocused(), FrameDt());
    const float target = (value && *value) ? 1.f : 0.f;
    if (!a.seeded)
    {
        a.aux = target;
        a.seeded = true;
    }
    a.aux = Approach(a.aux, target, k_slide_rate, FrameDt());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float alpha = enabled ? 1.f : 0.4f;
    const ImVec2 track_max(min.x + w, min.y + h);
    const ImU32 track = Mix(WithAlpha(p.textMuted, 0.55f), p.accent, a.aux);
    dl->AddRectFilled(min, track_max, WithAlpha(track, alpha), h * 0.5f);
    DrawFocusRing(dl, min, track_max, h * 0.5f, a.focus);

    const float radius = h * 0.5f - inset;
    const float travel = w - 2.f * inset - 2.f * radius;
    const ImVec2 thumb(min.x + inset + radius + travel * a.aux, min.y + h * 0.5f);
    dl->AddCircleFilled(thumb, radius, WithAlpha(p.background, alpha), 24);

    if (label)
        DrawTextLeft(dl, font, CentredY(font, min, h, min.x + w + label_gap),
                     WithAlpha(p.text, alpha), label, end);

    ImGui::PopID();
    return changed;
}

int SegmentedControl(const char* id, const char* const* labels, int count, int current,
                     float width)
{
    if (!labels || count <= 0)
        return current;

    const Palette& p = GetPalette();
    ImFont* font = Font(Face::Medium);
    const float h = Px(30.f);
    const float pad = Px(3.f);
    const float seg_pad = Px(14.f);

    std::vector<float> seg_w(count);
    float natural = 0.f;
    for (int i = 0; i < count; i++)
    {
        seg_w[i] = MeasureText(font, labels[i], VisibleLabelEnd(labels[i])).x + seg_pad * 2.f;
        natural += seg_w[i];
    }
    if (width > 0.f)
    {
        const float each = (Px(width) - pad * 2.f) / (float)count;
        for (float& sw : seg_w)
            sw = each;
    }

    float total = pad * 2.f;
    for (float sw : seg_w)
        total += sw;

    ImGui::PushID(id);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + total, min.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, p.surface, h * 0.5f);
    dl->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f), p.border,
                h * 0.5f, ImDrawFlags_None, Px(1.f));

    // Indicator slides between segments.
    std::vector<float> seg_x(count);
    float cursor_x = min.x + pad;
    for (int i = 0; i < count; i++)
    {
        seg_x[i] = cursor_x;
        cursor_x += seg_w[i];
    }
    const int selected = (current >= 0 && current < count) ? current : 0;

    AnimState& a = Anim(ImGui::GetID("##solace_segments"));
    if (!a.seeded)
    {
        a.aux = seg_x[selected];
        a.aux2 = seg_w[selected];
        a.seeded = true;
    }
    const float dt = FrameDt();
    a.aux = Approach(a.aux, seg_x[selected], k_slide_rate, dt);
    a.aux2 = Approach(a.aux2, seg_w[selected], k_slide_rate, dt);
    dl->AddRectFilled(ImVec2(a.aux, min.y + pad), ImVec2(a.aux + a.aux2, max.y - pad), p.accent,
                      (h - pad * 2.f) * 0.5f);

    int result = current;
    for (int i = 0; i < count; i++)
    {
        const ImVec2 seg_min(seg_x[i], min.y + pad);
        const ImVec2 seg_max(seg_x[i] + seg_w[i], max.y - pad);

        ImGui::SetCursorScreenPos(seg_min);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##solace_segment", ImVec2(seg_w[i], seg_max.y - seg_min.y));
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = KeyboardFocused();
        ImGui::PopID();

        if (focused)
            DrawFocusRing(dl, seg_min, seg_max, (h - pad * 2.f) * 0.5f, 1.f);

        const bool is_selected = (i == selected);
        const ImU32 col = is_selected ? p.accentText : (hovered ? p.text : p.textMuted);
        const ImVec2 text = MeasureText(font, labels[i], VisibleLabelEnd(labels[i]));
        DrawTextLeft(dl, font,
                     ImVec2(seg_min.x + (seg_w[i] - text.x) * 0.5f,
                            min.y + (h - text.y) * 0.5f),
                     col, labels[i], VisibleLabelEnd(labels[i]));

        if (clicked)
            result = i;
    }

    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(total, h));
    ImGui::PopID();
    return result;
}

// ---------------------------------------------------------------------------------------------
// Inputs and sections
// ---------------------------------------------------------------------------------------------

bool SearchField(const char* id, char* buffer, size_t bufferSize, const char* hint, float width)
{
    if (!buffer || bufferSize == 0)
        return false;

    const Palette& p = GetPalette();
    const float h = Px(36.f);
    const float pad = Px(12.f);
    const float icon_size = Px(16.f);
    const float clear_size = Px(20.f);
    const float w = width > 0.f ? Px(width) : ImGui::GetContentRegionAvail().x;

    ImGui::PushID(id);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + w, min.y + h);
    const ImGuiID field_id = ImGui::GetID("##solace_search");
    AnimState& a = Anim(field_id);
    const float dt = FrameDt();
    const bool hovered = ImGui::IsMouseHoveringRect(min, max);
    a.hover = Approach(a.hover, hovered ? 1.f : 0.f, k_hover_rate, dt);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = Px(10.f);
    dl->AddRectFilled(min, max, p.inputFill, rounding);
    const ImU32 border = Mix(p.border, p.borderStrong, std::max(a.hover, a.aux));
    dl->AddRect(ImVec2(min.x + 0.5f, min.y + 0.5f), ImVec2(max.x - 0.5f, max.y - 0.5f), border,
                rounding, ImDrawFlags_None, Px(1.f));

    DrawIcon(dl, Icon::Search, ImVec2(min.x + pad + icon_size * 0.5f, min.y + h * 0.5f), icon_size,
             p.textMuted);

    const bool had_text = buffer[0] != '\0';
    const float input_x = min.x + pad + icon_size + Px(8.f);
    const float clear_space = had_text ? clear_size + Px(6.f) : 0.f;
    const float input_w = std::max(Px(20.f), max.x - input_x - pad - clear_space);

    ImFont* font = Font(Face::Regular);
    ImGui::PushFont(font);
    ImGui::SetCursorScreenPos(ImVec2(input_x, min.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(p.textMuted));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(0.f, std::max(0.f, (h - ImGui::GetFontSize()) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::SetNextItemWidth(input_w);
    const bool changed = ImGui::InputTextWithHint("##solace_search_input", hint, buffer, bufferSize);
    a.aux = ImGui::IsItemActive() ? 1.f : 0.f;
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();

    bool cleared = false;
    if (buffer[0] != '\0')
    {
        ImGui::SetCursorScreenPos(ImVec2(max.x - pad - clear_size, min.y + (h - clear_size) * 0.5f));
        ImGui::PushID("clear");
        ImGui::InvisibleButton("##solace_clear", ImVec2(clear_size, clear_size));
        const bool clear_hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
        {
            buffer[0] = '\0';
            cleared = true;
        }
        ImGui::PopID();

        const ImVec2 cc(max.x - pad - clear_size * 0.5f, min.y + h * 0.5f);
        DrawIcon(dl, Icon::Cross, cc, Px(12.f), clear_hovered ? p.text : p.textMuted, 2.f);
    }

    a.focus = Approach(a.focus, a.aux, k_focus_rate, dt);
    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return changed || cleared;
}

bool SectionHeader(const char* id, const char* title, bool* open)
{
    const Palette& p = GetPalette();
    ImFont* font = Font(Face::Caption);
    const float h = Px(28.f);

    std::string upper;
    if (title)
    {
        const char* end = VisibleLabelEnd(title);
        for (const char* c = title; c < end; c++)
            upper.push_back((char)std::toupper((unsigned char)*c));
    }

    ImGui::PushID(id);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + w, min.y + h);

    const bool clicked = ImGui::InvisibleButton("##solace_section", ImVec2(w, h));
    if (clicked && open)
        *open = !*open;
    AnimState& a = UpdateItemState(ImGui::IsItemHovered(), ImGui::IsItemActive(), KeyboardFocused(),
                                   FrameDt());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawFocusRing(dl, min, max, Px(6.f), a.focus);
    const ImU32 label_col = Mix(p.textMuted, p.text, a.hover);
    DrawTextLeft(dl, font, CentredY(font, min, h, min.x + Px(2.f)), label_col, upper.c_str());

    const bool is_open = open && *open;
    DrawIcon(dl, is_open ? Icon::ChevronDown : Icon::ChevronRight,
             ImVec2(max.x - Px(10.f), min.y + h * 0.5f), Px(14.f), p.textMuted, 2.f);

    ImGui::PopID();
    return is_open;
}

bool BeginPropertyGrid(const char* id, float labelWidth)
{
    const bool ok = ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX);
    if (ok)
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(labelWidth));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    }
    return ok;
}

void PropertyLabel(const char* label, const char* tooltip)
{
    const Palette& p = GetPalette();
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.textMuted));
    ImGui::TextUnformatted(label ? label : "");
    ImGui::PopStyleColor();
    TooltipOnHover(tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void EndPropertyGrid()
{
    ImGui::EndTable();
}

bool BeginCard(const char* id, ImVec2 size, bool padded)
{
    const Palette& p = GetPalette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(p.surface));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(p.border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(14.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, Px(1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        padded ? ImVec2(Px(16.f), Px(16.f)) : ImVec2(0.f, 0.f));

    ImGuiChildFlags flags = ImGuiChildFlags_Borders;
    if (padded)
        flags |= ImGuiChildFlags_AlwaysUseWindowPadding;
    return ImGui::BeginChild(id, size, flags);
}

void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace solace
