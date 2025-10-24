#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include "BaseLight.h"

/**
 * StudioLighting provides a predefined 3-point lighting setup:
 * - Key Light: Primary directional light (front)
 * - Rim Light: Backlight for silhouette enhancement
 * - Fill Light: Soft ambient-like light to reduce harsh shadows
 */
class StudioLighting
{
public:
    StudioLighting();
    ~StudioLighting();

    // Initialize the studio lighting setup
    bool Initialize(int shadowMapSize = 2048);

    // Enable/disable studio lighting mode
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    // Update light positions based on camera or target
    void UpdateLighting(const glm::vec3& targetPosition, const glm::vec3& cameraPosition);

    // Get individual lights (now return BaseLight pointers)
    std::shared_ptr<BaseLight> GetKeyLight() const;
    std::shared_ptr<BaseLight> GetRimLight() const;
    
    // Light intensity controls
    void SetKeyLightIntensity(float intensity) { m_keyIntensity = intensity; }
    void SetRimLightIntensity(float intensity) { m_rimIntensity = intensity; }
    void SetFillLightIntensity(float intensity) { m_fillIntensity = intensity; }
    
    float GetKeyLightIntensity() const { return m_keyIntensity; }
    float GetRimLightIntensity() const { return m_rimIntensity; }
    float GetFillLightIntensity() const { return m_fillIntensity; }

    // Get effective light colors (intensity applied)
    glm::vec3 GetEffectiveKeyColor() const;
    glm::vec3 GetEffectiveRimColor() const;
    glm::vec3 GetEffectiveFillColor() const;

    // Light color controls
    void SetKeyLightColor(const glm::vec3& color) { m_keyColor = color; }
    void SetRimLightColor(const glm::vec3& color) { m_rimColor = color; }
    void SetFillLightColor(const glm::vec3& color) { m_fillColor = color; }

    glm::vec3 GetKeyLightColor() const { return m_keyColor; }
    glm::vec3 GetRimLightColor() const { return m_rimColor; }
    glm::vec3 GetFillLightColor() const { return m_fillColor; }

    // Preset configurations
    void LoadPreset(const std::string& presetName);
    void SavePreset(const std::string& presetName) const;

    // Studio lighting presets
    void SetWarmStudioPreset();
    void SetCoolStudioPreset();
    void SetDramaticPreset();
    void SetSoftPreset();

    // Integration with light manager
    void RegisterWithLightManager(class LightManager* lightManager);

private:
    bool m_enabled = false;
    
    // Light objects (now using BaseLight interface)
    std::shared_ptr<class DirectionalLight> m_keyLight;   // Primary light
    std::shared_ptr<class DirectionalLight> m_rimLight;   // Rim/back light
    
    // Light properties
    glm::vec3 m_keyColor = glm::vec3(1.0f, 0.95f, 0.8f);    // Warm white
    glm::vec3 m_rimColor = glm::vec3(0.8f, 0.9f, 1.0f);     // Cool white
    glm::vec3 m_fillColor = glm::vec3(0.7f, 0.8f, 0.9f);    // Soft blue
    
    float m_keyIntensity = 1.0f;
    float m_rimIntensity = 0.6f;
    float m_fillIntensity = 0.3f;
    
    // Light positioning parameters
    float m_keyLightAngle = 45.0f;      // Degrees from front
    float m_keyLightElevation = 30.0f;   // Degrees above horizon
    float m_rimLightElevation = 15.0f;   // Degrees above horizon
    float m_lightDistance = 10.0f;      // Distance from target
};