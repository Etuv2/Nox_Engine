#include "LightingWindow.h"
#include "../SceneGraph.h"
#include "../LightManager.h"
#include "../BaseLight.h"
#include "../DirectionalLight.h"
#include "../PointLight.h"
#include "../SpotLight.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>

LightingWindow::LightingWindow()
    : BaseWindow("Lighting System", "F3")
{
    m_position = ImVec2(10, 350);
    m_size = ImVec2(300, 250);
}

void LightingWindow::Render() {
    BeginWindow();
    if (!IsVisible()) return;

    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Lighting System");
    ImGui::Separator();
    
    // Check for multi-light system first
    std::shared_ptr<LightManager> lightManager = nullptr;
    if (m_sceneGraph) {
        lightManager = m_sceneGraph->GetLightManager();
    }
    
    if (lightManager && lightManager->GetActiveLightCount() > 0) {
        // Multi-light system is available and has lights
        ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Multi-Light System Active");
        
        ImGui::Text("Active Lights: %d", lightManager->GetActiveLightCount());
        ImGui::Text("Directional: %zu", lightManager->GetDirectionalLightCount());
        ImGui::Text("Point: %zu", lightManager->GetPointLightCount());
        ImGui::Text("Spot: %zu", lightManager->GetSpotLightCount());
        ImGui::Text("Shadow Casting: %zu", lightManager->GetShadowCastingLightCount());
        
        ImGui::Separator();
        
        if (ImGui::Button("Update Light Proxies")) {
            lightManager->UpdateLightProxies();
        }
        ImGui::SameLine();
        if (ImGui::Button("Print Light Info")) {
            lightManager->PrintLightInfo();
        }

        RenderShadowDiagnostics(lightManager);
        
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "Individual Light Controls");
        
        // Get all lights for detailed editing
        auto allLights = lightManager->GetAllLights();
        
        for (size_t i = 0; i < allLights.size(); ++i) {
            auto light = allLights[i];
            if (!light || !light->IsEnabled()) continue;
            
            std::string lightName = "Light " + std::to_string(i);
            
            // Determine light type and render appropriate controls
            switch (light->GetLightType()) {
                case BaseLight::LightType::DIRECTIONAL:
                    lightName = "Directional Light " + std::to_string(i);
                    RenderDirectionalLightControls(light, static_cast<int>(i));
                    break;
                case BaseLight::LightType::POINT:
                    lightName = "Point Light " + std::to_string(i);
                    RenderPointLightControls(light, static_cast<int>(i));
                    break;
                case BaseLight::LightType::SPOT:
                    lightName = "Spot Light " + std::to_string(i);
                    RenderSpotLightControls(light, static_cast<int>(i));
                    break;
                default:
                    break;
            }
        }
        
    } else if (m_lighting) {
        // Fallback to single directional light
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Single Directional Light");
        ImGui::Separator();
        
        RenderSingleLightControls();
        
    } else {
        // No lighting system available
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No lighting system available");
        ImGui::Text("Expected lighting features:");
        ImGui::BulletText("Multi-light support");
        ImGui::BulletText("Real-time light manipulation");
        ImGui::BulletText("Shadow configuration");
        ImGui::BulletText("Light performance monitoring");
    }
    
    EndWindow();
}

void LightingWindow::RenderSingleLightControls() {
    if (!m_lighting) return;
    
    ImGui::Text("Directional Light Controls:");
    
    // Light color and intensity
    glm::vec3 color = m_lighting->GetColor();
    if (ImGui::ColorEdit3("Light Color", glm::value_ptr(color))) {
        m_lighting->SetColor(color);
    }
    
    float intensity = m_lighting->GetIntensity();
    if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 10.0f, "%.2f")) {
        m_lighting->SetIntensity(intensity);
    }
    
    // Shadow controls
    bool castsShadows = m_lighting->CastsShadows();
    if (ImGui::Checkbox("Cast Shadows", &castsShadows)) {
        m_lighting->SetCastsShadows(castsShadows);
    }
    
    // Enable/disable
    bool enabled = m_lighting->IsEnabled();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        m_lighting->SetEnabled(enabled);
    }
    
    ImGui::Separator();
    ImGui::Text("Direction: (%.2f, %.2f, %.2f)", m_lightDir.x, m_lightDir.y, m_lightDir.z);
    ImGui::Text("Position: (%.2f, %.2f, %.2f)", m_lightPos.x, m_lightPos.y, m_lightPos.z);
}

void LightingWindow::RenderShadowDiagnostics(const std::shared_ptr<LightManager>& lightManager) {
    if (!lightManager) return;

    if (ImGui::CollapsingHeader("Shadow Diagnostics")) {
        auto& shadowConfig = lightManager->GetShadowConfig();
        ImGui::Text("Cascade Count: %d", shadowConfig.directionalCascadeCount);
        ImGui::Text("Split Lambda: %.2f", shadowConfig.directionalSplitLambda);
        ImGui::Text("Base Resolution: %d", shadowConfig.baseResolution);
        ImGui::Text("Stable Snapping: %s", shadowConfig.stableTexelSnapping ? "On" : "Off");
        ImGui::Text("Rotated PCF: %s", shadowConfig.useRotatedPoissonPCF ? "On" : "Off");
        ImGui::Text("PCSS: %s", shadowConfig.enablePCSS ? "On" : "Off");
        ImGui::Text("Dir Bias: %.5f / %.5f / %.5f",
            shadowConfig.directionalConstantBias,
            shadowConfig.directionalSlopeBias,
            shadowConfig.directionalNormalOffset);

        const auto slices = lightManager->GetShadowSliceDebug();
        ImGui::Text("Shadow Slices: %d", static_cast<int>(slices.size()));

        const int maxSlicesToShow = std::min<int>(8, static_cast<int>(slices.size()));
        for (int i = 0; i < maxSlicesToShow; ++i) {
            const auto& slice = slices[i];
            ImGui::BulletText(
                "Slice %d | Light %d | Sub %d | Age %u | Dirty 0x%X | %.2f ms",
                slice.arrayIndex,
                slice.lightIndex,
                slice.subIndex,
                slice.age,
                slice.dirtyReason,
                slice.lastUpdateMs
            );
        }
    }
}

void LightingWindow::RenderDirectionalLightControls(std::shared_ptr<BaseLight> light, int index) {
    std::string lightID = "##dir_" + std::to_string(index);
    std::string headerName = "Directional Light " + std::to_string(index);
    
    if (ImGui::CollapsingHeader(headerName.c_str())) {
        // Color control
        glm::vec3 color = light->GetColor();
        if (ImGui::ColorEdit3(("Color" + lightID).c_str(), glm::value_ptr(color))) {
            light->SetColor(color);
        }
        
        // Intensity control
        float intensity = light->GetIntensity();
        if (ImGui::SliderFloat(("Intensity" + lightID).c_str(), &intensity, 0.0f, 10.0f, "%.2f")) {
            light->SetIntensity(intensity);
        }
        
        // Direction control (if available)
        glm::vec3 direction = light->GetDirection();
        if (ImGui::DragFloat3(("Direction" + lightID).c_str(), glm::value_ptr(direction), 0.01f, -1.0f, 1.0f)) {
            light->SetDirection(glm::normalize(direction));
        }
        
        // Shadow controls
        bool castsShadows = light->CastsShadows();
        if (ImGui::Checkbox(("Cast Shadows" + lightID).c_str(), &castsShadows)) {
            light->SetCastsShadows(castsShadows);
        }
        
        // Enable/disable
        bool enabled = light->IsEnabled();
        if (ImGui::Checkbox(("Enabled" + lightID).c_str(), &enabled)) {
            light->SetEnabled(enabled);
        }
    }
}

void LightingWindow::RenderPointLightControls(std::shared_ptr<BaseLight> light, int index) {
    std::string lightID = "##point_" + std::to_string(index);
    std::string headerName = "Point Light " + std::to_string(index);
    
    if (ImGui::CollapsingHeader(headerName.c_str())) {
        // Color control
        glm::vec3 color = light->GetColor();
        if (ImGui::ColorEdit3(("Color" + lightID).c_str(), glm::value_ptr(color))) {
            light->SetColor(color);
        }
        
        // Intensity control
        float intensity = light->GetIntensity();
        if (ImGui::SliderFloat(("Intensity" + lightID).c_str(), &intensity, 0.0f, 100.0f, "%.2f")) {
            light->SetIntensity(intensity);
        }
        
        // Position control
        glm::vec3 position = light->GetPosition();
        if (ImGui::DragFloat3(("Position" + lightID).c_str(), glm::value_ptr(position), 0.1f)) {
            light->SetPosition(position);
        }
        
        // Range control
        float range = light->GetRange();
        if (ImGui::SliderFloat(("Range" + lightID).c_str(), &range, 0.1f, 100.0f, "%.2f")) {
            light->SetRange(range);
        }
        
        // Attenuation controls
        glm::vec3 attenuation = light->GetAttenuation();
        if (ImGui::DragFloat3(("Attenuation (C,L,Q)" + lightID).c_str(), glm::value_ptr(attenuation), 0.01f, 0.0f, 10.0f)) {
            light->SetAttenuation(attenuation.x, attenuation.y, attenuation.z);
        }
        
        // Shadow controls
        bool castsShadows = light->CastsShadows();
        if (ImGui::Checkbox(("Cast Shadows" + lightID).c_str(), &castsShadows)) {
            light->SetCastsShadows(castsShadows);
        }
        
        // Enable/disable
        bool enabled = light->IsEnabled();
        if (ImGui::Checkbox(("Enabled" + lightID).c_str(), &enabled)) {
            light->SetEnabled(enabled);
        }
    }
}

void LightingWindow::RenderSpotLightControls(std::shared_ptr<BaseLight> light, int index) {
    std::string lightID = "##spot_" + std::to_string(index);
    std::string headerName = "Spot Light " + std::to_string(index);
    
    if (ImGui::CollapsingHeader(headerName.c_str())) {
        // Color control
        glm::vec3 color = light->GetColor();
        if (ImGui::ColorEdit3(("Color" + lightID).c_str(), glm::value_ptr(color))) {
            light->SetColor(color);
        }
        
        // Intensity control
        float intensity = light->GetIntensity();
        if (ImGui::SliderFloat(("Intensity" + lightID).c_str(), &intensity, 0.0f, 100.0f, "%.2f")) {
            light->SetIntensity(intensity);
        }
        
        // Position control
        glm::vec3 position = light->GetPosition();
        if (ImGui::DragFloat3(("Position" + lightID).c_str(), glm::value_ptr(position), 0.1f)) {
            light->SetPosition(position);
        }
        
        // Direction control
        glm::vec3 direction = light->GetDirection();
        if (ImGui::DragFloat3(("Direction" + lightID).c_str(), glm::value_ptr(direction), 0.01f, -1.0f, 1.0f)) {
            light->SetDirection(glm::normalize(direction));
        }
        
        // Range control
        float range = light->GetRange();
        if (ImGui::SliderFloat(("Range" + lightID).c_str(), &range, 0.1f, 100.0f, "%.2f")) {
            light->SetRange(range);
        }
        
        // Spot light specific controls
        if (auto spotLight = std::dynamic_pointer_cast<SpotLight>(light)) {
            float cutOff = spotLight->GetCutOff();
            if (ImGui::SliderFloat(("Inner Angle" + lightID).c_str(), &cutOff, 0.0f, 89.0f, "%.1f°")) {
                spotLight->SetCutOff(cutOff);
            }
            
            float outerCutOff = spotLight->GetOuterCutOff();
            if (ImGui::SliderFloat(("Outer Angle" + lightID).c_str(), &outerCutOff, cutOff + 1.0f, 90.0f, "%.1f°")) {
                spotLight->SetOuterCutOff(outerCutOff);
            }
        }
        
        // Attenuation controls
        glm::vec3 attenuation = light->GetAttenuation();
        if (ImGui::DragFloat3(("Attenuation (C,L,Q)" + lightID).c_str(), glm::value_ptr(attenuation), 0.01f, 0.0f, 10.0f)) {
            light->SetAttenuation(attenuation.x, attenuation.y, attenuation.z);
        }
        
        // Shadow controls
        bool castsShadows = light->CastsShadows();
        if (ImGui::Checkbox(("Cast Shadows" + lightID).c_str(), &castsShadows)) {
            light->SetCastsShadows(castsShadows);
        }
        
        // Enable/disable
        bool enabled = light->IsEnabled();
        if (ImGui::Checkbox(("Enabled" + lightID).c_str(), &enabled)) {
            light->SetEnabled(enabled);
        }
    }
}
