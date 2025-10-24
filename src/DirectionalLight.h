#pragma once
#include <array>
#include <glm/glm.hpp>
#include "FrameBuffer.h"
#include "BaseLight.h"
#include "ShadowMapper.h"
#include "json.hpp"

/**
 * Enhanced Directional light (like the sun) with cascaded shadow mapping and PCSS support.
 * Inherits from BaseLight for consistency in the lighting system.
 * Features: Optimized cascade splits, PCSS filtering, and enhanced temporal stability.
 */
class DirectionalLight : public BaseLight
{
public:
    static const int NUM_CASCADES = 4; // Number of cascades

    DirectionalLight();
    ~DirectionalLight();

    // Enhanced cascade initialization with PCSS support
    bool InitializeCascades(GLuint shadowSize, float splitLambda);

    // Enhanced cascade updates with improved stability and quality
    void UpdateCascades(const glm::mat4& view,
        const glm::mat4& projection,
        float nearPlane,
        float farPlane,
        float windowAspect,
        float fov);

    void SetCascadeSplits(float sliceNear, float sliceFar, float overlap);
    void SetSplitLambda(float lambda) { m_splitLambda = glm::clamp(lambda, 0.0f, 1.0f); }
    void SetShadowSize(int size) { m_shadowSize = size; }
    
    // PCSS (Percentage Closer Soft Shadows) configuration
    void SetPCSSConfig(const ShadowMapper::PCSS::PCSSConfig& config);
    const ShadowMapper::PCSS::PCSSConfig& GetPCSSConfig() const;
    
    // Enhanced filtering control
    void EnableEnhancedFiltering(bool enable);
    bool IsEnhancedFilteringEnabled() const { return m_useEnhancedFiltering; }
    
    // Quality analysis for cascade optimization
    float GetCascadeQuality(int cascadeIndex) const;
    
    // Access the final transform for each cascade
    std::array<glm::mat4, NUM_CASCADES> m_cascadeLightSpace;
    // Access each cascade's near plane
    std::array<float, NUM_CASCADES> cascadeSplits;

    // Enhanced FBO with array depth and optimized filtering
    std::unique_ptr<FrameBuffer> m_shadowFBO;

    // Light setters/getters (override BaseLight for legacy compatibility)
    void SetLightPosition(const glm::vec3& pos) { SetPosition(pos); }
    void SetLightDirection(const glm::vec3& dir) { SetDirection(dir); }
    void SetLightColor(const glm::vec3& color) { SetColor(color); }
    glm::vec3 GetLightPosition() const { return GetPosition(); }
    glm::vec3 GetLightDirection() const { return GetDirection(); }
    glm::vec3 GetLightColor() const { return GetColor(); }
    
    // Access the shadow map size
    int GetShadowSize() const { return m_shadowSize; }

    // Enhanced shadow map access
    GLuint GetDepthArrayID() const;
    GLuint GetShadowMapID() const override { return GetDepthArrayID(); }

    // Access the shadow shader ID
    GLuint GetShadowShaderID() const;
    void SetShadowShaderID(GLuint shaderID);

    // Enhanced serialization with PCSS support
    void Serialize(nlohmann::json& json) const override;
    void Deserialize(const nlohmann::json& json) override;

    // Enhanced debug information
    std::string GetDebugInfo() const override;

private:
    int m_shadowSize; // Size of each shadow map
    float m_splitLambda = 0.6f; // Optimized blend factor between uniform and logarithmic splits
    GLuint m_shadowShaderID = 0; // Shader ID for shadow pass
    float m_shadowNearPlane = 0.1f;
    float m_shadowFarPlane = 100.0f;
    
    // PCSS configuration
    ShadowMapper::PCSS::PCSSConfig m_pcssConfig;
    
    // Enhanced filtering flag
    bool m_useEnhancedFiltering = true;
};
