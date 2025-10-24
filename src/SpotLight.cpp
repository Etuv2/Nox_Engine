#include "SpotLight.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

SpotLight::SpotLight()
    : BaseLight(LightType::SPOT)
    , depthFBO(0), depthMap(0)
    , m_shadowResolution(2048) // Increased default for better quality
    , m_cutOff(12.5f), m_outerCutOff(17.5f)
    , m_nearPlane(1.0f), m_farPlane(25.0f)
{
    // Set default spot light properties
    SetAttenuation(1.0f, 0.09f, 0.032f);
    SetRange(25.0f);
    SetDirection(glm::vec3(0.0f, -1.0f, 0.0f));
}

SpotLight::~SpotLight() {
    if (depthFBO) glDeleteFramebuffers(1, &depthFBO);
    if (depthMap) glDeleteTextures(1, &depthMap);
}

bool SpotLight::InitializeShadowMap(GLuint shadowResolution) {
    // CRITICAL FIX: Disable legacy shadow system to prevent conflicts with LightManager
    std::cout << "[SpotLight] Legacy shadow system disabled - using unified LightManager shadows" << std::endl;
    
    // Store shadow resolution for reference but don't create FBO
    m_shadowResolution = static_cast<int>(shadowResolution);
    
    // Mark as shadow casting but don't create separate shadow maps
    SetCastsShadows(true);
    
    std::cout << "[SpotLight] SpotLight configured for unified shadow system:" << std::endl;
    std::cout << "  - Shadow Size: " << m_shadowResolution << "x" << m_shadowResolution << std::endl;
    std::cout << "  - Cut-off angles: " << m_cutOff << "° / " << m_outerCutOff << "°" << std::endl;
    std::cout << "  - Using LightManager shadow array instead of separate FBO" << std::endl;
    
    return true;
}

GLuint SpotLight::GetDepthMap() const {
    return depthMap;
}

float SpotLight::CalculateSpotIntensity(const glm::vec3& lightToFragment) const {
    glm::vec3 lightDir = glm::normalize(GetDirection());
    glm::vec3 lightToFragDir = glm::normalize(lightToFragment);
    
    float theta = glm::dot(lightToFragDir, -lightDir);
    float epsilon = glm::cos(glm::radians(m_cutOff)) - glm::cos(glm::radians(m_outerCutOff));
    float intensity = glm::clamp((theta - glm::cos(glm::radians(m_outerCutOff))) / epsilon, 0.0f, 1.0f);
    
    return intensity;
}

bool SpotLight::IsPointInCone(const glm::vec3& point) const {
    glm::vec3 lightToPoint = point - GetPosition();
    float distance = glm::length(lightToPoint);
    
    if (distance > GetRange()) return false;
    
    glm::vec3 lightDir = glm::normalize(GetDirection());
    glm::vec3 lightToPointDir = glm::normalize(lightToPoint);
    
    float theta = glm::dot(lightToPointDir, lightDir);
    return theta > glm::cos(glm::radians(m_outerCutOff));
}

glm::mat4 SpotLight::GetLightSpaceMatrix() const {
    glm::mat4 lightProjection = glm::perspective(
        glm::radians(m_outerCutOff * 2.0f), // FOV based on outer cutoff
        1.0f,                               // Aspect ratio
        m_nearPlane, 
        m_farPlane
    );
    
    glm::mat4 lightView = glm::lookAt(
        GetPosition(),
        GetPosition() + GetDirection(),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    
    return lightProjection * lightView;
}

void SpotLight::Serialize(nlohmann::json& json) const {
    BaseLight::Serialize(json);
    json["cutOff"] = m_cutOff;
    json["outerCutOff"] = m_outerCutOff;
    json["nearPlane"] = m_nearPlane;
    json["farPlane"] = m_farPlane;
    json["shadowResolution"] = m_shadowResolution;
}

void SpotLight::Deserialize(const nlohmann::json& json) {
    BaseLight::Deserialize(json);
    
    if (json.contains("cutOff")) {
        m_cutOff = json["cutOff"];
    }
    if (json.contains("outerCutOff")) {
        m_outerCutOff = json["outerCutOff"];
    }
    if (json.contains("nearPlane")) {
        m_nearPlane = json["nearPlane"];
    }
    if (json.contains("farPlane")) {
        m_farPlane = json["farPlane"];
    }
    if (json.contains("shadowResolution")) {
        m_shadowResolution = json["shadowResolution"];
    }
}

std::string SpotLight::GetDebugInfo() const {
    std::string base = BaseLight::GetDebugInfo();
    base += "Spot Light Cut-off: " + std::to_string(m_cutOff) + "°\n";
    base += "Outer Cut-off: " + std::to_string(m_outerCutOff) + "°\n";
    base += "Shadow Resolution: " + std::to_string(m_shadowResolution) + "\n";
    base += "Near/Far Plane: " + std::to_string(m_nearPlane) + "/" + std::to_string(m_farPlane) + "\n";
    return base;
}
