#include "StateExportWindow.h"
#include <IMGUI/imgui.h>
#include <fstream>
#include "../json.hpp"

using json = nlohmann::json;

StateExportWindow::StateExportWindow()
    : BaseWindow("State & Export", "F9")
{
    m_position = ImVec2(1020, 370);
    m_size = ImVec2(360, 320);
}

void StateExportWindow::Render() {
    BeginWindow();
    if (!IsVisible()) return;

    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Scene State Management");
    ImGui::Separator();

    // Quick Save/Load section
    if (ImGui::Button("Quick Save", ImVec2(170, 25))) {
        if (m_quickSaveCallback) {
            if (m_quickSaveCallback()) {
                ImGui::OpenPopup("QuickSaveSuccess");
            } else {
                ImGui::OpenPopup("QuickSaveFailed");
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Quick Load", ImVec2(170, 25))) {
        if (m_loadSceneStateCallback) {
            if (m_loadSceneStateCallback("snapshots/quicksave.json")) {
                ImGui::OpenPopup("QuickLoadSuccess");
            } else {
                ImGui::OpenPopup("QuickLoadFailed");
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Custom Save section
    ImGui::Text("Save Scene State:");
    ImGui::InputText("##SavePath", m_saveStateFilepath, sizeof(m_saveStateFilepath));
    if (ImGui::Button("Save State", ImVec2(-1, 25))) {
        if (m_saveSceneStateCallback) {
            std::string filepath(m_saveStateFilepath);
            if (m_saveSceneStateCallback(filepath)) {
                ImGui::OpenPopup("SaveSuccess");
            } else {
                ImGui::OpenPopup("SaveFailed");
            }
        }
    }

    ImGui::Spacing();

    // Custom Load section
    ImGui::Text("Load Scene State:");
    ImGui::InputText("##LoadPath", m_loadStateFilepath, sizeof(m_loadStateFilepath));
    
    // Show which scene this state belongs to
    std::string stateInfoText = GetStateFileInfo(m_loadStateFilepath);
    if (!stateInfoText.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", stateInfoText.c_str());
    }
    
    if (ImGui::Button("Load State", ImVec2(-1, 25))) {
        if (m_loadSceneStateCallback) {
            std::string filepath(m_loadStateFilepath);
            if (m_loadSceneStateCallback(filepath)) {
                ImGui::OpenPopup("LoadSuccess");
            } else {
                ImGui::OpenPopup("LoadFailed");
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Performance Export section
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Performance Data Export");
    ImGui::Separator();

    // Recording controls
    if (!m_isRecording) {
        if (ImGui::Button("Start Recording", ImVec2(-1, 30))) {
            if (m_startRecordingCallback) {
                m_startRecordingCallback();
                m_isRecording = true;
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Stop Recording", ImVec2(-1, 30))) {
            if (m_stopRecordingCallback) {
                m_stopRecordingCallback();
                m_isRecording = false;
            }
        }
        ImGui::PopStyleColor();
        
        // Show recording indicator
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "? RECORDING");
    }

    ImGui::Spacing();

    // CSV Export
    ImGui::Text("Export CSV:");
    ImGui::InputText("##CSVPath", m_exportCSVFilepath, sizeof(m_exportCSVFilepath));
    if (ImGui::Button("Export CSV", ImVec2(-1, 25))) {
        if (m_exportCSVCallback) {
            std::string filepath(m_exportCSVFilepath);
            if (m_exportCSVCallback(filepath)) {
                ImGui::OpenPopup("CSVExportSuccess");
            } else {
                ImGui::OpenPopup("CSVExportFailed");
            }
        }
    }

    ImGui::Spacing();

    // JSON Export
    ImGui::Text("Export JSON:");
    ImGui::InputText("##JSONPath", m_exportJSONFilepath, sizeof(m_exportJSONFilepath));
    if (ImGui::Button("Export JSON", ImVec2(-1, 25))) {
        if (m_exportJSONCallback) {
            std::string filepath(m_exportJSONFilepath);
            if (m_exportJSONCallback(filepath)) {
                ImGui::OpenPopup("JSONExportSuccess");
            } else {
                ImGui::OpenPopup("JSONExportFailed");
            }
        }
    }

    // Success/Failure popups
    if (ImGui::BeginPopupModal("SaveSuccess", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Scene state saved successfully!");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("SaveFailed", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Failed to save scene state.");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("LoadSuccess", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Scene state loaded successfully!");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("LoadFailed", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Failed to load scene state.");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("QuickSaveSuccess", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Quick save successful!");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("QuickSaveFailed", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Quick save failed.");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("QuickLoadSuccess", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Quick load successful!");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("QuickLoadFailed", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Quick load failed.");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("CSVExportSuccess", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Performance data exported to CSV!");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("CSVExportFailed", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Failed to export CSV.");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("JSONExportSuccess", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Performance data exported to JSON!");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("JSONExportFailed", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Failed to export JSON.");
        if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    EndWindow();
}

std::string StateExportWindow::GetStateFileInfo(const std::string& filepath) {
    if (filepath.empty()) {
        return "";
    }
    
    try {
        std::ifstream inFile(filepath);
        if (!inFile.is_open()) {
            return "";
        }
        
        nlohmann::json stateJson;
        inFile >> stateJson;
        inFile.close();
        
        // Extract info
        std::string baseScene = stateJson.value("base_scene_file", "Unknown");
        std::string sceneName = stateJson.value("scene_name", "Unknown");
        int nodeCount = stateJson.value("scene_node_count", -1);
        
        std::string info = "Scene: " + baseScene;
        if (nodeCount >= 0) {
            info += " (" + std::to_string(nodeCount) + " nodes)";
        }
        
        return info;
        
    } catch (...) {
        return "";
    }
}
