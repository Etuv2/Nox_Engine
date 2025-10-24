#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <GL/glew.h>
#include "json.hpp"

/**
 * Base class for all light types in the scene.
 * Provides common interface and functionality for lighting calculations.
 */
class BaseLight
{
public:
    enum class LightType {
        DIRECTIONAL,
        POINT,
        SPOT,
        AREA,
        STUDIO_KEY,
        STUDIO_RIM,
        STUDIO_FILL
    };

    BaseLight(LightType type);
    virtual ~BaseLight() = default;

    // Core light properties
    virtual void SetColor(const glm::vec3& color) { m_color = color; }
    virtual void SetIntensity(float intensity) { m_intensity = intensity; }
    virtual void SetEnabled(bool enabled) { m_enabled = enabled; }

    virtual glm::vec3 GetColor() const { return m_color; }
    virtual float GetIntensity() const { return m_intensity; }
    virtual glm::vec3 GetEffectiveColor() const { return m_color * m_intensity; }
    virtual bool IsEnabled() const { return m_enabled; }
    virtual LightType GetLightType() const { return m_lightType; }

    // Transform operations (for scene graph integration)
    virtual void SetPosition(const glm::vec3& position) { m_position = position; }
    virtual void SetDirection(const glm::vec3& direction) { m_direction = glm::normalize(direction); }
    virtual glm::vec3 GetPosition() const { return m_position; }
    virtual glm::vec3 GetDirection() const { return m_direction; }

    // Shadow mapping support
    virtual bool CastsShadows() const { return m_castsShadows; }
    virtual void SetCastsShadows(bool casts) { m_castsShadows = casts; }
    virtual GLuint GetShadowMapID() const { return 0; } // Override in derived classes

    // Attenuation (for point/spot lights)
    virtual void SetAttenuation(float constant, float linear, float quadratic) {
        m_attenuationConstant = constant;
        m_attenuationLinear = linear;
        m_attenuationQuadratic = quadratic;
    }
    virtual glm::vec3 GetAttenuation() const { 
        return glm::vec3(m_attenuationConstant, m_attenuationLinear, m_attenuationQuadratic); 
    }

    // Light range calculation
    virtual float GetRange() const { return m_range; }
    virtual void SetRange(float range) { m_range = range; }

    // Update function for dynamic lights
    virtual void Update(float deltaTime) {}

    // Serialization support for scene loading/saving
    virtual void Serialize(nlohmann::json& json) const;
    virtual void Deserialize(const nlohmann::json& json);

    // Debug information
    virtual std::string GetDebugInfo() const;

protected:
    LightType m_lightType;
    glm::vec3 m_color = glm::vec3(1.0f);
    float m_intensity = 1.0f;
    bool m_enabled = true;
    bool m_castsShadows = false;

    // Spatial properties
    glm::vec3 m_position = glm::vec3(0.0f);
    glm::vec3 m_direction = glm::vec3(0.0f, -1.0f, 0.0f);

    // Attenuation for point/spot lights
    float m_attenuationConstant = 1.0f;
    float m_attenuationLinear = 0.09f;
    float m_attenuationQuadratic = 0.032f;
    float m_range = 10.0f;
};