#pragma once

#include "BaseWindow.h"
#include <glm/glm.hpp>
#include <memory>

// Forward declarations
class BaseLight;
class LightManager;

/**
 * @brief Lighting controls window for managing lights in the scene
 */
class LightingWindow : public BaseWindow {
public:
    LightingWindow();
    
    void Render() override;
    
    // Lighting-specific data
    void SetLightPosition(const glm::vec3& pos) { m_lightPos = pos; }
    void SetLightDirection(const glm::vec3& dir) { m_lightDir = dir; }

private:
    glm::vec3 m_lightPos = glm::vec3(0.0f);
    glm::vec3 m_lightDir = glm::vec3(0.0f, -1.0f, 0.0f);
    
    // Helper methods for rendering different light types
    void RenderSingleLightControls();
    void RenderDirectionalLightControls(std::shared_ptr<BaseLight> light, int index);
    void RenderPointLightControls(std::shared_ptr<BaseLight> light, int index);
    void RenderSpotLightControls(std::shared_ptr<BaseLight> light, int index);
    void RenderShadowDiagnostics(const std::shared_ptr<LightManager>& lightManager);
};
