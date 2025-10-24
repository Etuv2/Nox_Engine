#pragma once
#include "IMGUI/imgui.h"

// Call this function after initializing ImGui to set a black-and-white dark style.
inline void SetStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Base colors (monochrome)
    ImVec4 black = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
    ImVec4 darkGray = ImVec4(0.1f, 0.1f, 0.1f, 1.00f);
    ImVec4 midGray = ImVec4(0.3f, 0.3f, 0.3f, 1.00f);
    ImVec4 lightGray = ImVec4(0.75f, 0.75f, 0.75f, 1.00f);
    ImVec4 white = ImVec4(1.0f, 1.0f, 1.0f, 1.00f);
    ImVec4 transparent = ImVec4(0, 0, 0, 0);

    // Backgrounds
    colors[ImGuiCol_WindowBg] = black;
    colors[ImGuiCol_ChildBg] = darkGray;
    colors[ImGuiCol_PopupBg] = darkGray;

    // Borders and separators
    colors[ImGuiCol_Border] = midGray;
    colors[ImGuiCol_BorderShadow] = black;
    colors[ImGuiCol_Separator] = midGray;
    colors[ImGuiCol_SeparatorHovered] = lightGray;
    colors[ImGuiCol_SeparatorActive] = white;

    // Text
    colors[ImGuiCol_Text] = white;
    colors[ImGuiCol_TextDisabled] = lightGray;

    // Frames (e.g., input boxes)
    colors[ImGuiCol_FrameBg] = darkGray;
    colors[ImGuiCol_FrameBgHovered] = midGray;
    colors[ImGuiCol_FrameBgActive] = lightGray;

    // Titles
    colors[ImGuiCol_TitleBg] = black;
    colors[ImGuiCol_TitleBgActive] = midGray;
    colors[ImGuiCol_TitleBgCollapsed] = darkGray;

    // Buttons
    colors[ImGuiCol_Button] = midGray;
    colors[ImGuiCol_ButtonHovered] = lightGray;
    colors[ImGuiCol_ButtonActive] = white;

    // Headers
    colors[ImGuiCol_Header] = midGray;
    colors[ImGuiCol_HeaderHovered] = lightGray;
    colors[ImGuiCol_HeaderActive] = white;

    // Tabs
    colors[ImGuiCol_Tab] = darkGray;
    colors[ImGuiCol_TabHovered] = lightGray;
    colors[ImGuiCol_TabActive] = midGray;
    colors[ImGuiCol_TabUnfocused] = darkGray;
    colors[ImGuiCol_TabUnfocusedActive] = midGray;

    // Scrollbars
    colors[ImGuiCol_ScrollbarBg] = black;
    colors[ImGuiCol_ScrollbarGrab] = midGray;
    colors[ImGuiCol_ScrollbarGrabHovered] = lightGray;
    colors[ImGuiCol_ScrollbarGrabActive] = white;

    // Check Mark
    colors[ImGuiCol_CheckMark] = white;

    // Sliders
    colors[ImGuiCol_SliderGrab] = lightGray;
    colors[ImGuiCol_SliderGrabActive] = white;

    // Resize grips
    colors[ImGuiCol_ResizeGrip] = midGray;
    colors[ImGuiCol_ResizeGripHovered] = lightGray;
    colors[ImGuiCol_ResizeGripActive] = white;

    // Docking
    // Ensure that docking features are enabled in ImGui
    #ifdef IMGUI_HAS_DOCK
    colors[ImGuiCol_DockingPreview] = lightGray;
    colors[ImGuiCol_DockingEmptyBg] = darkGray;
    #endif

    // DragDrop
    colors[ImGuiCol_DragDropTarget] = white;

    // Nav
    colors[ImGuiCol_NavHighlight] = white;
    colors[ImGuiCol_NavWindowingHighlight] = white;
    colors[ImGuiCol_NavWindowingDimBg] = darkGray;

    // Modal
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.85f);

    // Rounding & Borders
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.ChildRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.TabRounding = 2.0f;

    // Borders
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
}
