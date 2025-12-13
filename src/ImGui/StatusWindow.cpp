#include "StatusWindow.h"
#include <IMGUI/imgui.h>
#include <algorithm>
#include <numeric>

StatusWindow::StatusWindow()
    : BaseWindow("Engine Status", "F1")
{
    m_position = ImVec2(10, 10);
    m_size = ImVec2(300, 180);
}

void StatusWindow::Render() {
    BeginWindow();
    if (!IsVisible()) return;

    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "NOX Engine Status");
    ImGui::Separator();
    
    // Performance info
    ImGui::Text("FPS: %.1f", m_fps);
    ImGui::Text("Frame Time: %.2f ms", 1000.0f / m_fps);
    ImGui::Text("Resolution: %dx%d", m_windowWidth, m_windowHeight);
    
    ImGui::Separator();
    
    // Scene information
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Scene Info");
    ImGui::Text("Current Scene: %s", m_sceneName.c_str());
    
    ImGui::Separator();
    
    // Scene selection dropdown
    if (!m_sceneList.empty()) {
        ImGui::Text("Load Scene:");
        
        // Create combo box with available scenes
        if (ImGui::BeginCombo("##SceneSelector", "Select Scene...")) {
            for (size_t i = 0; i < m_sceneList.size(); ++i) {
                const bool isSelected = false; // Could track current selection
                if (ImGui::Selectable(m_sceneList[i].c_str(), isSelected)) {
                    if (m_sceneSwapCallback) {
                        m_sceneSwapCallback(m_sceneList[i]);
                    }
                }
            }
            ImGui::EndCombo();
        }
    }
    
    ImGui::Separator();
    
    // Light information
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Lighting");
    ImGui::Text("Light Pos: (%.1f, %.1f, %.1f)", m_lightPosition.x, m_lightPosition.y, m_lightPosition.z);
    ImGui::Text("Light Dir: (%.2f, %.2f, %.2f)", m_lightDirection.x, m_lightDirection.y, m_lightDirection.z);
    
    ImGui::Separator();
    
    // Window control section
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "Window Controls");
    
    // Create buttons for each window
    if (ImGui::Button("Camera (F2)")) {
        if (m_windowToggleCallback) m_windowToggleCallback("Camera");
    }
    ImGui::SameLine();
    if (ImGui::Button("Lighting (F3)")) {
        if (m_windowToggleCallback) m_windowToggleCallback("Lighting");
    }
    
    if (ImGui::Button("Scene Tree (F4)")) {
        if (m_windowToggleCallback) m_windowToggleCallback("Hierarchy");
    }
    ImGui::SameLine();
    if (ImGui::Button("Gizmo (F5)")) {
        if (m_windowToggleCallback) m_windowToggleCallback("Gizmo");
    }
    
    if (ImGui::Button("Rendering (F6)")) {
        if (m_windowToggleCallback) m_windowToggleCallback("Rendering");
    }
    ImGui::SameLine();
    if (ImGui::Button("Performance (F7)")) {
        if (m_windowToggleCallback) m_windowToggleCallback("Performance");
    }
    
    if (ImGui::Button("Help (F12)")) {
        if (m_windowToggleCallback) m_windowToggleCallback("Help");
    }
    
    EndWindow();
}