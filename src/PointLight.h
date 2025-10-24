#pragma once
#include "BaseLight.h"
#include "FrameBuffer.h"
#include "json.hpp"
#include <memory>

/**
 * Point Light implementation with optional shadow mapping.
 * Emits light uniformly in all directions from a point in space.
 */
class PointLight : public BaseLight
{
public:
    PointLight();
    virtual ~PointLight();

    // Shadow mapping for point lights (cubemap shadows)
    bool InitializeShadowMap(GLuint shadowResolution);
    void BindShadowFBO();
    void UnbindFBO();
    GLuint GetDepthMap() const { return depthMap; }
    GLuint GetShadowMapID() const override { return depthMap; }

    // Point light specific properties
    void SetRadius(float radius) { m_radius = radius; }
    float GetRadius() const { return m_radius; }

    // Legacy compatibility
    void SetLightPosition(const glm::vec3& pos) { SetPosition(pos); }
    glm::vec3 GetLightPosition() const { return GetPosition(); }

    // Calculate attenuation at a given distance
    float CalculateAttenuation(float distance) const;
    
    // Check if a point is within light influence
    bool IsPointInRange(const glm::vec3& point) const;

    // Update shadow matrices for cube mapping
    void UpdateShadowMatrices();

    // Serialization
    void Serialize(nlohmann::json& json) const override;
    void Deserialize(const nlohmann::json& json) override;

    std::string GetDebugInfo() const override;

    // Legacy members (kept for compatibility)
    glm::vec3 position;  // Use GetPosition() instead
    glm::vec3 direction; // Use GetDirection() instead
    float nearPlane = 0.1f;
    float farPlane = 25.0f;

private:
    float m_radius = 10.0f;  // Visual radius for light influence
    
    // Shadow mapping
    GLuint depthFBO = 0;
    GLuint depthMap = 0;
    int m_shadowResolution = 1024;
};
