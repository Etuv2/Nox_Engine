#define GLM_ENABLE_EXPERIMENTAL
#include "StudioLighting.h"
#include "DirectionalLight.h"
#include "LightManager.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
#include <iostream>
#include <cmath>

StudioLighting::StudioLighting()
{
    m_keyLight = std::make_shared<DirectionalLight>();
    m_rimLight = std::make_shared<DirectionalLight>();
    
    // Set studio-specific light types
    // Note: These could be specialized studio light types if needed
}

StudioLighting::~StudioLighting()
{
}

bool StudioLighting::Initialize(int shadowMapSize)
{
    // Initialize key light with shadow mapping
    if (!m_keyLight->InitializeCascades(shadowMapSize, 0.85f)) {
        std::cerr << "[StudioLighting] Failed to initialize key light shadows\n";
        return false;
    }
    
    // Initialize rim light (typically no shadows to avoid double-shadowing)
    if (!m_rimLight->InitializeCascades(shadowMapSize / 2, 0.85f)) {
        std::cerr << "[StudioLighting] Failed to initialize rim light shadows\n";
        return false;
    }
    
    // Set up default studio lighting configuration
    SetWarmStudioPreset();
    
    std::cout << "[StudioLighting] Initialized successfully\n";
    return true;
}

void StudioLighting::UpdateLighting(const glm::vec3& targetPosition, const glm::vec3& cameraPosition)
{
    if (!m_enabled) return;
    
    // Calculate camera-relative directions for consistent lighting
    glm::vec3 cameraToTarget = glm::normalize(targetPosition - cameraPosition);
    glm::vec3 cameraRight = glm::normalize(glm::cross(cameraToTarget, glm::vec3(0, 1, 0)));
    glm::vec3 cameraUp = glm::cross(cameraRight, cameraToTarget);
    
    // Key Light: positioned at an angle from the camera direction
    float keyAngleRad = glm::radians(m_keyLightAngle);
    float keyElevationRad = glm::radians(m_keyLightElevation);
    
    // Use matrix-based rotations instead of vector rotations
    glm::mat4 keyRotation = glm::rotate(glm::mat4(1.0f), keyAngleRad, cameraUp);
    keyRotation = glm::rotate(keyRotation, -keyElevationRad, cameraRight);
    glm::vec3 keyDirection = glm::vec3(keyRotation * glm::vec4(cameraToTarget, 0.0f));
    
    glm::vec3 keyPosition = targetPosition - keyDirection * m_lightDistance;
    
    m_keyLight->SetPosition(keyPosition);
    m_keyLight->SetDirection(keyDirection);
    m_keyLight->SetColor(m_keyColor);
    m_keyLight->SetIntensity(m_keyIntensity);
    
    // Rim Light: positioned behind and above the target
    float rimElevationRad = glm::radians(m_rimLightElevation);
    glm::mat4 rimRotation = glm::rotate(glm::mat4(1.0f), rimElevationRad, cameraRight);
    glm::vec3 rimDirection = glm::vec3(rimRotation * glm::vec4(-cameraToTarget, 0.0f)); // Opposite to camera
    
    glm::vec3 rimPosition = targetPosition - rimDirection * m_lightDistance;
    
    m_rimLight->SetPosition(rimPosition);
    m_rimLight->SetDirection(rimDirection);
    m_rimLight->SetColor(m_rimColor);
    m_rimLight->SetIntensity(m_rimIntensity);
}

std::shared_ptr<BaseLight> StudioLighting::GetKeyLight() const
{
    return std::static_pointer_cast<BaseLight>(m_keyLight);
}

std::shared_ptr<BaseLight> StudioLighting::GetRimLight() const
{
    return std::static_pointer_cast<BaseLight>(m_rimLight);
}

glm::vec3 StudioLighting::GetEffectiveKeyColor() const
{
    return m_keyColor * m_keyIntensity;
}

glm::vec3 StudioLighting::GetEffectiveRimColor() const
{
    return m_rimColor * m_rimIntensity;
}

glm::vec3 StudioLighting::GetEffectiveFillColor() const
{
    return m_fillColor * m_fillIntensity;
}

void StudioLighting::SetWarmStudioPreset()
{
    m_keyColor = glm::vec3(1.0f, 0.95f, 0.8f);    // Warm white
    m_rimColor = glm::vec3(0.8f, 0.9f, 1.0f);     // Cool white
    m_fillColor = glm::vec3(0.9f, 0.85f, 0.7f);   // Warm fill
    
    m_keyIntensity = 2.0f;      // Increased from 1.2f for brighter key light
    m_rimIntensity = 1.0f;      // Increased from 0.7f for more visible rim
    m_fillIntensity = 0.6f;     // Increased from 0.4f for better fill lighting
    
    m_keyLightAngle = 45.0f;
    m_keyLightElevation = 35.0f;
    m_rimLightElevation = 20.0f;
}

void StudioLighting::SetCoolStudioPreset()
{
    m_keyColor = glm::vec3(0.9f, 0.95f, 1.0f);    // Cool white
    m_rimColor = glm::vec3(1.0f, 0.9f, 0.7f);     // Warm rim
    m_fillColor = glm::vec3(0.7f, 0.8f, 0.95f);   // Cool fill
    
    m_keyIntensity = 1.8f;      // Increased from 1.0f
    m_rimIntensity = 1.0f;      // Increased from 0.8f
    m_fillIntensity = 0.5f;     // Increased from 0.3f
    
    m_keyLightAngle = 40.0f;
    m_keyLightElevation = 30.0f;
    m_rimLightElevation = 15.0f;
}

void StudioLighting::SetDramaticPreset()
{
    m_keyColor = glm::vec3(1.0f, 0.9f, 0.7f);     // Warm dramatic
    m_rimColor = glm::vec3(0.6f, 0.7f, 1.0f);     // Strong rim
    m_fillColor = glm::vec3(0.4f, 0.5f, 0.7f);    // Dark fill
    
    m_keyIntensity = 2.5f;      // Increased from 1.5f for more dramatic lighting
    m_rimIntensity = 1.5f;      // Increased from 1.0f
    m_fillIntensity = 0.25f;    // Increased from 0.15f
    
    m_keyLightAngle = 60.0f;
    m_keyLightElevation = 45.0f;
    m_rimLightElevation = 25.0f;
}

void StudioLighting::SetSoftPreset()
{
    m_keyColor = glm::vec3(0.95f, 0.95f, 0.9f);   // Neutral soft
    m_rimColor = glm::vec3(0.9f, 0.9f, 0.95f);    // Subtle rim
    m_fillColor = glm::vec3(0.8f, 0.85f, 0.9f);   // Bright fill
    
    m_keyIntensity = 1.5f;      // Increased from 0.8f for brighter soft lighting
    m_rimIntensity = 0.7f;      // Increased from 0.4f
    m_fillIntensity = 0.8f;     // Increased from 0.6f for more fill light
    
    m_keyLightAngle = 30.0f;
    m_keyLightElevation = 20.0f;
    m_rimLightElevation = 10.0f;
}

void StudioLighting::LoadPreset(const std::string& presetName)
{
    if (presetName == "warm") {
        SetWarmStudioPreset();
    }
    else if (presetName == "cool") {
        SetCoolStudioPreset();
    }
    else if (presetName == "dramatic") {
        SetDramaticPreset();
    }
    else if (presetName == "soft") {
        SetSoftPreset();
    }
    else {
        std::cerr << "[StudioLighting] Unknown preset: " << presetName << "\n";
        SetWarmStudioPreset(); // Default fallback
    }
}

void StudioLighting::SavePreset(const std::string& presetName) const
{
    // This could save to a JSON file or configuration system
    // For now, just log the current settings
    std::cout << "[StudioLighting] Saving preset '" << presetName << "':\n";
    std::cout << "  Key: (" << m_keyColor.x << ", " << m_keyColor.y << ", " << m_keyColor.z << ") * " << m_keyIntensity << "\n";
    std::cout << "  Rim: (" << m_rimColor.x << ", " << m_rimColor.y << ", " << m_rimColor.z << ") * " << m_rimIntensity << "\n";
    std::cout << "  Fill: (" << m_fillColor.x << ", " << m_fillColor.y << ", " << m_fillColor.z << ") * " << m_fillIntensity << "\n";
}

void StudioLighting::RegisterWithLightManager(LightManager* lightManager)
{
    if (!lightManager) return;
    
    if (m_keyLight) {
        lightManager->RegisterLight(GetKeyLight(), "StudioKeyLight");
    }
    if (m_rimLight) {
        lightManager->RegisterLight(GetRimLight(), "StudioRimLight");
    }
}