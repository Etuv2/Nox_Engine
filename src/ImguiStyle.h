#pragma once
#include "IMGUI/imgui.h"
#include "editor/solace/SolaceUI.h"

// Editor style: Solace dark theme (src/editor/solace, adapted from Free-Solace-ImGui-Interface).
// The racing HUD does not use ImGui styles, so it stays independent of the editor look.
inline void SetStyle()
{
    solace::ApplyTheme(true, 1.0f);
}
