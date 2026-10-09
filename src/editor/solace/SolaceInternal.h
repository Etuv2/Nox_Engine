// Internal helpers for the Solace UI kit. Not part of the public API (see SolaceUI.h).
#pragma once

#include "editor/solace/SolaceUI.h"

#include <cmath>

namespace solace::detail
{

// Colour helpers operating on ImU32 (IM_COL32 layout).
ImU32 Mix(ImU32 a, ImU32 b, float t);
ImU32 WithAlpha(ImU32 c, float alpha);

// Frame-rate independent exponential approach: rate is roughly 1/time-constant in 1/s.
inline float Approach(float current, float target, float rate, float dt)
{
    const float k = 1.f - std::exp(-rate * dt);
    return current + (target - current) * k;
}

// Per-ID animation values for one control. Values are 0..1 unless noted.
struct AnimState
{
    float hover = 0.f;
    float press = 0.f;
    float focus = 0.f;
    float aux = 0.f;  // control-specific: switch thumb, segment indicator x, search focus
    float aux2 = 0.f; // control-specific: segment indicator width
    bool seeded = false; // aux/aux2 hold a real value (first use snaps to target)
    int lastFrame = -1;
};

// Returns the animation state for `id`, creating it on first use. Entries unused for a
// while are removed automatically.
AnimState& Anim(ImGuiID id);

// True when the last item is focused by keyboard navigation (draws a focus ring).
bool KeyboardFocused();

// Draws the focus ring just outside [min,max].
void DrawFocusRing(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float alpha);

// Frame delta time, clamped so that a stalled frame does not snap animations.
float FrameDt();

// Draw helpers
void DrawTextLeft(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 col, const char* text,
                  const char* textEnd = nullptr);
ImVec2 MeasureText(ImFont* font, const char* text, const char* textEnd = nullptr);

// Returns the visible part of an ImGui label (text before "##").
const char* VisibleLabelEnd(const char* label);

} // namespace solace::detail

namespace solace::svg
{
struct sub_path
{
    ImVector<ImVec2> points;
    ImVector<float> cumulative;
    float length = 0.f;
    bool closed = false;
};

struct shape
{
    ImVector<sub_path> subs;
    float length = 0.f;
};

const shape& path(const char* d);
const shape& circle(float cx, float cy, float r);
const shape& rounded_rect(float x, float y, float w, float h, float rx);

void stroke(ImDrawList* draw_list, const shape& s, const ImVec2& origin, float scale, ImU32 col,
            float width, float fraction = 1.f);
} // namespace solace::svg
