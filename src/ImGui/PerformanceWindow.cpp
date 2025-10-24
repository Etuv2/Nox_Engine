#include "PerformanceWindow.h"
#include <IMGUI/imgui.h>
#include "../SceneGraph.h"
#include "../LightManager.h"
#include <algorithm>
#include <numeric>

PerformanceWindow::PerformanceWindow()
    : BaseWindow("Performance Monitor", "F7")
{
    m_position = ImVec2(680, 370);
    m_size = ImVec2(320, 200);
}

void PerformanceWindow::Render() {
    BeginWindow();
    if (!IsVisible()) return;

    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "System Performance");
    ImGui::Separator();
    
    // Frame timing
    ImGui::Text("Frame Rate: %.1f FPS", m_fps);
    
    if (!m_frameTimeData.empty()) {
        float currentFrameTime = m_frameTimeData.back();
        ImGui::Text("Frame Time: %.2f ms", currentFrameTime);
        
        // Calculate statistics
        std::vector<float> sorted = m_frameTimeData;
        std::sort(sorted.begin(), sorted.end());
        
        float min = sorted.front();
        float max = sorted.back();
        float avg = std::accumulate(sorted.begin(), sorted.end(), 0.0f) / sorted.size();
        float p1 = sorted[static_cast<size_t>(sorted.size() * 0.01f)];
        float p99 = sorted[static_cast<size_t>(sorted.size() * 0.99f)];
        
        ImGui::Separator();
        ImGui::Text("Frame Time Statistics:");
        ImGui::Text("Min: %.2f ms | Max: %.2f ms", min, max);
        ImGui::Text("Avg: %.2f ms | P1: %.2f ms", avg, p1);
        ImGui::Text("P99: %.2f ms", p99);
        
        // Performance classification
        if (avg < 16.67f) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Performance: Excellent (>60 FPS)");
        } else if (avg < 33.33f) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Performance: Good (30-60 FPS)");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Performance: Poor (<30 FPS)");
        }
        
        // Mini frame time graph
        ImGui::PlotLines("##FrameTime", m_frameTimeData.data(), 
                        static_cast<int>(m_frameTimeData.size()), 
                        0, nullptr, min * 0.9f, max * 1.1f, ImVec2(-1, 60));
    }
    
    // Rendering statistics
    ImGui::Separator();
    ImGui::Text("Rendering Pipeline:");
    
    if (m_sceneGraph) {
        auto lightManager = m_sceneGraph->GetLightManager();
        if (lightManager) {
            ImGui::Text("Active Lights: %d", lightManager->GetActiveLightCount());
            ImGui::Text("Shadow Casting: %zu", lightManager->GetShadowCastingLightCount());
            
            // Light breakdown
            ImGui::Text("Directional: %zu", lightManager->GetDirectionalLightCount());
            ImGui::Text("Point: %zu", lightManager->GetPointLightCount());
            ImGui::Text("Spot: %zu", lightManager->GetSpotLightCount());
        } else {
            ImGui::Text("Light Manager: Not available");
        }
    }
    
    // Memory usage (placeholder)
    ImGui::Separator();
    ImGui::Text("Memory Usage:");
    ImGui::Text("GPU Memory: Unknown");
    ImGui::Text("System Memory: Unknown");
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(Memory profiling not implemented)");
    
    EndWindow();
}