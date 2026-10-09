#include "BaseWindow.h"
#include <iostream>

BaseWindow::BaseWindow(const std::string& name, const std::string& keyBinding)
    : m_name(name)
    , m_keyBinding(keyBinding)
    , m_visible(true)
    , m_position(0, 0)
    , m_size(300, 200)
    , m_flags(ImGuiWindowFlags_NoCollapse)
    , m_firstFrame(true)
{
}

void BaseWindow::BeginWindow() {
    if (!m_visible) return;
    // Stable window names are the dock IDs; size/position come from the docked layout
    // (config/editor_layout.ini). The first-use size only matters for floating windows.
    ImGui::SetNextWindowSize(m_size, ImGuiCond_FirstUseEver);
    ImGui::Begin(m_name.c_str(), &m_visible, m_flags);
}

void BaseWindow::EndWindow() {
    if (!m_visible) return;
    ImGui::End();
}