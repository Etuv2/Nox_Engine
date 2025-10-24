#pragma once
#include "BaseLight.h"
#include "json.hpp"
#include <GL/glew.h>
#include <glm/glm.hpp>

/**
 * Spot Light implementation with cone-shaped light emission and shadow mapping.
 * Inherits from BaseLight for consistency in the lighting system.
 */
class SpotLight : public BaseLight
{
public:
    SpotLight();
    ~SpotLight();

    // Initialize the depth map for spotlight shadow mapping.
    bool InitializeShadowMap(GLuint shadowResolution);
    GLuint GetDepthMap() const;
    GLuint GetShadowMapID() const override { return GetDepthMap(); }

    // Legacy compatibility
    void SetLightPosition(const glm::vec3& pos) { SetPosition(pos); }
    void SetLightDirection(const glm::vec3& dir) { SetDirection(dir); }
    glm::vec3 GetLightPosition() const { return GetPosition(); }
    glm::vec3 GetLightDirection() const { return GetDirection(); }

    // Spot light specific properties
    void SetCutOff(float cutOff) { m_cutOff = cutOff; }
    void SetOuterCutOff(float outerCutOff) { m_outerCutOff = outerCutOff; }
    float GetCutOff() const { return m_cutOff; }
    float GetOuterCutOff() const { return m_outerCutOff; }
    
    // Get cone angles as cosine values for shader use
    float GetInnerCutOff() const { return glm::cos(glm::radians(m_cutOff)); }
    float GetOuterCutOffCos() const { return glm::cos(glm::radians(m_outerCutOff)); }

    // Calculate spot light intensity at a given direction
    float CalculateSpotIntensity(const glm::vec3& lightToFragment) const;
    
    // Check if a point is within the spotlight cone
    bool IsPointInCone(const glm::vec3& point) const;

    // Get light space matrix for shadow mapping
    glm::mat4 GetLightSpaceMatrix() const;

    // Serialization
    void Serialize(nlohmann::json& json) const override;
    void Deserialize(const nlohmann::json& json) override;

    std::string GetDebugInfo() const override;

private:
    GLuint depthFBO = 0;
    GLuint depthMap = 0;
    int m_shadowResolution = 1024;
    
    // Spot light cone parameters
    float m_cutOff = 12.5f;      // Inner cone angle (degrees)
    float m_outerCutOff = 17.5f; // Outer cone angle (degrees)
    
    // Shadow mapping parameters
    float m_nearPlane = 1.0f;
    float m_farPlane = 25.0f;
};
