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
    
    // Set window position and size on first frame
    if (m_firstFrame) {
        ImGui::SetNextWindowPos(m_position, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(m_size, ImGuiCond_FirstUseEver);
        m_firstFrame = false;
    }
    
    std::string windowTitle = m_name;
    if (!m_keyBinding.empty()) {
        windowTitle += " (" + m_keyBinding + ")";
    }
    
    ImGui::Begin(windowTitle.c_str(), &m_visible, m_flags);
}

void BaseWindow::EndWindow() {
    if (!m_visible) return;
    ImGui::End();
}