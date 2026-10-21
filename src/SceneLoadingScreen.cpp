#include "SceneLoadingScreen.h"
#include <IMGUI/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

SceneLoadingScreenHook g_hook;
constexpr float kPi = 3.14159265358979f;

}  // namespace

void SceneLoadingScreen::SetHook(SceneLoadingScreenHook hook)
{
    g_hook = std::move(hook);
}

SceneLoadingScreenAction SceneLoadingScreen::Draw(const SceneLoadingScreenState& s)
{
    if (g_hook) {
        return g_hook(s);
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Full-screen backdrop: opaque in game mode, dimmed over the editor.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), display,
        s.overlay ? IM_COL32(0, 0, 0, 160) : IM_COL32(12, 14, 18, 255));

    const float width = std::min(560.0f, display.x * 0.9f);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;

    SceneLoadingScreenAction action = SceneLoadingScreenAction::None;
    ImGui::Begin("##SceneLoading", nullptr, flags);

    const bool failed = !s.error.empty();
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted(failed ? "Could not load scene" : "Loading scene");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("%s", s.sceneName.c_str());
    ImGui::Spacing();

    if (failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 110, 255));
        ImGui::TextWrapped("%s", s.error.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextDisabled("%s", s.startupFailure ? "No scene is loaded." : "The current scene is kept.");
        ImGui::Spacing();
        if (ImGui::Button(s.startupFailure ? "Quit" : "Continue", ImVec2(120.0f, 0.0f))) {
            action = SceneLoadingScreenAction::Dismiss;
        }
    }
    else {
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(std::clamp(s.progress, 0.0f, 1.0f) * 100.0f + 0.5f));
        ImGui::ProgressBar(std::clamp(s.progress, 0.0f, 1.0f), ImVec2(-1.0f, 18.0f), percent);

        // Spinner: eight dots on a circle, the lead dot brightest.
        const ImVec2 spinnerMin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(28.0f, 28.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted(s.stage.c_str());
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 center(spinnerMin.x + 14.0f, spinnerMin.y + 14.0f);
        constexpr int dots = 8;
        for (int i = 0; i < dots; ++i) {
            const float angle = static_cast<float>(s.timeSeconds) * 6.0f + static_cast<float>(i) * (2.0f * kPi / dots);
            const float alpha = static_cast<float>(i + 1) / dots;
            draw->AddCircleFilled(ImVec2(center.x + 10.0f * std::cos(angle), center.y + 10.0f * std::sin(angle)), 2.8f,
                IM_COL32(230, 230, 235, static_cast<int>(255.0f * alpha)));
        }
    }

    ImGui::End();
    return action;
}
