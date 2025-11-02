#include "PointLight.h"
#include <GL/glew.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

PointLight::PointLight()
    : BaseLight(LightType::POINT)
    , depthFBO(0), depthMap(0)
    , position(0.0f), direction(0.0f, -1.0f, 0.0f)
    , nearPlane(1.0f), farPlane(100.0f)
    , m_shadowResolution(2048) // Increased default for better quality
{
    // Set default point light attenuation
    SetAttenuation(1.0f, 0.09f, 0.032f);
    SetRange(25.0f);
}

PointLight::~PointLight() {
    if (depthFBO) glDeleteFramebuffers(1, &depthFBO);
    if (depthMap) glDeleteTextures(1, &depthMap);
}

bool PointLight::InitializeShadowMap(GLuint shadowResolution) {
    // Disable legacy shadow system to prevent conflicts with LightManager
    std::cout << "[PointLight] Legacy shadow system disabled - using unified LightManager shadows" << std::endl;
    
    // Store shadow resolution for reference but don't create FBO
    m_shadowResolution = static_cast<int>(shadowResolution);
    
    // Mark as shadow casting but don't create separate shadow maps
    SetCastsShadows(true);
    
    std::cout << "[PointLight] PointLight configured for unified shadow system:" << std::endl;
    std::cout << "  - Shadow Size: " << m_shadowResolution << "x" << m_shadowResolution << std::endl;
    std::cout << "  - Range: " << GetRange() << " units" << std::endl;
    std::cout << "  - Using LightManager shadow array instead of separate cubemap" << std::endl;
    
    return true;
}

void PointLight::BindShadowFBO() {
    glBindFramebuffer(GL_FRAMEBUFFER, depthFBO);
    glViewport(0, 0, m_shadowResolution, m_shadowResolution);
}

void PointLight::UnbindFBO() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

float PointLight::CalculateAttenuation(float distance) const {
    glm::vec3 attenuation = GetAttenuation();
    return 1.0f / (attenuation.x + attenuation.y * distance + attenuation.z * distance * distance);
}

bool PointLight::IsPointInRange(const glm::vec3& point) const {
    float distance = glm::length(point - GetPosition());
    return distance <= GetRange();
}

void PointLight::UpdateShadowMatrices() {
    // Implementation for shadow cube mapping if needed
    // This would generate 6 view matrices for cube faces
}

void PointLight::Serialize(nlohmann::json& json) const {
    BaseLight::Serialize(json);
    json["radius"] = m_radius;
    json["nearPlane"] = nearPlane;
    json["farPlane"] = farPlane;
    json["shadowResolution"] = m_shadowResolution;
}

void PointLight::Deserialize(const nlohmann::json& json) {
    BaseLight::Deserialize(json);
    
    if (json.contains("radius")) {
        m_radius = json["radius"];
    }
    if (json.contains("nearPlane")) {
        nearPlane = json["nearPlane"];
    }
    if (json.contains("farPlane")) {
        farPlane = json["farPlane"];
    }
    if (json.contains("shadowResolution")) {
        m_shadowResolution = json["shadowResolution"];
    }
    
    // Sync legacy position/direction with base class
    position = GetPosition();
    direction = GetDirection();
}

std::string PointLight::GetDebugInfo() const {
    std::string base = BaseLight::GetDebugInfo();
    base += "Point Light Radius: " + std::to_string(m_radius) + "\n";
    base += "Shadow Resolution: " + std::to_string(m_shadowResolution) + "\n";
    base += "Near/Far Plane: " + std::to_string(nearPlane) + "/" + std::to_string(farPlane) + "\n";
    return base;
}
