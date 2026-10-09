// Shared helpers for the Solace UI kit: colour math, per-ID animation store, focus ring, text.

#include "editor/solace/SolaceInternal.h"

#include <cfloat>
#include <cstring>
#include <unordered_map>

namespace solace::detail
{
namespace
{
constexpr int k_sweep_interval_frames = 600;
constexpr int k_unused_frames_limit = 1200;

std::unordered_map<ImGuiID, AnimState>& anim_store()
{
    static std::unordered_map<ImGuiID, AnimState> store;
    return store;
}

int g_last_sweep_frame = 0;
} // namespace

ImU32 Mix(ImU32 a, ImU32 b, float t)
{
    t = ImClamp(t, 0.f, 1.f);
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

ImU32 WithAlpha(ImU32 c, float alpha)
{
    const ImU32 a = (ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * ImClamp(alpha, 0.f, 1.f) + 0.5f);
    return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

AnimState& Anim(ImGuiID id)
{
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    const int frame = ctx ? ctx->FrameCount : 0;

    auto& store = anim_store();
    if (frame - g_last_sweep_frame > k_sweep_interval_frames)
    {
        g_last_sweep_frame = frame;
        for (auto it = store.begin(); it != store.end();)
        {
            if (frame - it->second.lastFrame > k_unused_frames_limit)
                it = store.erase(it);
            else
                ++it;
        }
    }

    AnimState& state = store[id];
    state.lastFrame = frame;
    return state;
}

bool KeyboardFocused()
{
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    return ctx && ctx->NavCursorVisible && ImGui::IsItemFocused();
}

void DrawFocusRing(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float alpha)
{
    if (alpha <= 0.f)
        return;
    const float grow = Px(2.f);
    dl->AddRect(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow),
                WithAlpha(GetPalette().focusRing, alpha), rounding + grow, ImDrawFlags_None,
                Px(1.5f));
}

float FrameDt()
{
    return ImClamp(ImGui::GetIO().DeltaTime, 0.f, 0.05f);
}

void DrawTextLeft(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 col, const char* text,
                  const char* textEnd)
{
    if (!font || !text || (col & IM_COL32_A_MASK) == 0)
        return;
    dl->AddText(font, font->FontSize, pos, col, text, textEnd);
}

ImVec2 MeasureText(ImFont* font, const char* text, const char* textEnd)
{
    if (!font || !text)
        return ImVec2(0.f, 0.f);
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, text, textEnd);
}

const char* VisibleLabelEnd(const char* label)
{
    if (!label)
        return nullptr;
    const char* hash = std::strstr(label, "##");
    return hash ? hash : label + std::strlen(label);
}

} // namespace solace::detail
