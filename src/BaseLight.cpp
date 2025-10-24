#include "BaseLight.h"
#include "json.hpp"
#include <sstream>

BaseLight::BaseLight(LightType type) : m_lightType(type)
{
}

void BaseLight::Serialize(nlohmann::json& json) const
{
    json["type"] = static_cast<int>(m_lightType);
    json["color"] = { m_color.r, m_color.g, m_color.b };
    json["intensity"] = m_intensity;
    json["enabled"] = m_enabled;
    json["castsShadows"] = m_castsShadows;
    json["position"] = { m_position.x, m_position.y, m_position.z };
    json["direction"] = { m_direction.x, m_direction.y, m_direction.z };
    json["attenuation"] = { m_attenuationConstant, m_attenuationLinear, m_attenuationQuadratic };
    json["range"] = m_range;
}

void BaseLight::Deserialize(const nlohmann::json& json)
{
    if (json.contains("color") && json["color"].is_array() && json["color"].size() == 3) {
        m_color = glm::vec3(json["color"][0], json["color"][1], json["color"][2]);
    }
    
    if (json.contains("intensity")) {
        m_intensity = json["intensity"];
    }
    
    if (json.contains("enabled")) {
        m_enabled = json["enabled"];
    }
    
    if (json.contains("castsShadows")) {
        m_castsShadows = json["castsShadows"];
    }
    
    if (json.contains("position") && json["position"].is_array() && json["position"].size() == 3) {
        m_position = glm::vec3(json["position"][0], json["position"][1], json["position"][2]);
    }
    
    if (json.contains("direction") && json["direction"].is_array() && json["direction"].size() == 3) {
        m_direction = glm::normalize(glm::vec3(json["direction"][0], json["direction"][1], json["direction"][2]));
    }
    
    if (json.contains("attenuation") && json["attenuation"].is_array() && json["attenuation"].size() == 3) {
        m_attenuationConstant = json["attenuation"][0];
        m_attenuationLinear = json["attenuation"][1];
        m_attenuationQuadratic = json["attenuation"][2];
    }
    
    if (json.contains("range")) {
        m_range = json["range"];
    }
}

std::string BaseLight::GetDebugInfo() const
{
    std::stringstream ss;
    ss << "Light Type: " << static_cast<int>(m_lightType) << "\n";
    ss << "Color: (" << m_color.r << ", " << m_color.g << ", " << m_color.b << ")\n";
    ss << "Intensity: " << m_intensity << "\n";
    ss << "Enabled: " << (m_enabled ? "Yes" : "No") << "\n";
    ss << "Casts Shadows: " << (m_castsShadows ? "Yes" : "No") << "\n";
    ss << "Position: (" << m_position.x << ", " << m_position.y << ", " << m_position.z << ")\n";
    ss << "Direction: (" << m_direction.x << ", " << m_direction.y << ", " << m_direction.z << ")\n";
    ss << "Range: " << m_range << "\n";
    return ss.str();
}