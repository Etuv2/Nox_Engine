#include "DirectionalLight.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include "ShadowMapper.h"

DirectionalLight::DirectionalLight()
    : BaseLight(LightType::DIRECTIONAL)
    , m_shadowSize(2048), m_splitLambda(0.6f)  // Optimized lambda for better balance
    , m_shadowShaderID(0)
    , m_shadowNearPlane(0.0f), m_shadowFarPlane(0.0f)
    , m_pcssConfig()  // Initialize PCSS configuration
    , m_useEnhancedFiltering(true)
{
    m_cascadeLightSpace.fill(glm::mat4(1.0f));
    cascadeSplits.fill(0.0f);
    
    // Set default directional light properties
    SetPosition(glm::vec3(0.0f, 100.0f, 0.0f));
    SetDirection(glm::vec3(0.0f, -1.0f, 0.0f));
    SetColor(glm::vec3(1.0f, 1.0f, 0.8f));
    SetIntensity(1.0f);
    
    // Initialize PCSS configuration with sensible defaults
    m_pcssConfig.blockerSearchSamples = 16;
    m_pcssConfig.pcfSamples = 32;
    m_pcssConfig.lightSize = 0.025f;
    m_pcssConfig.enablePCSS = true;
}

DirectionalLight::~DirectionalLight() {
    // FrameBuffer cleans up automatically (RAII)
}

bool DirectionalLight::InitializeCascades(GLuint shadowSize, float splitLambda) {
    SetSplitLambda(splitLambda);
    
    // CRITICAL FIX: Disable legacy shadow system to prevent conflicts with LightManager
    std::cout << "[DirectionalLight] Legacy cascade system disabled - using unified LightManager shadows" << std::endl;
    
    // Store shadow size for reference but don't create FBO
    m_shadowSize = static_cast<int>(glm::clamp(shadowSize, 512u, 4096u));
    
    // Mark as shadow casting but don't create separate shadow maps
    SetCastsShadows(true);
    
    std::cout << "[DirectionalLight] DirectionalLight configured for unified shadow system:" << std::endl;
    std::cout << "  - Shadow Size: " << m_shadowSize << "x" << m_shadowSize << std::endl;
    std::cout << "  - PCSS enabled: " << (m_pcssConfig.enablePCSS ? "Yes" : "No") << std::endl;
    std::cout << "  - Using LightManager shadow array instead of separate FBO" << std::endl;
    
    return true;
}

void DirectionalLight::UpdateCascades(const glm::mat4& view,
    const glm::mat4& projection,
    float nearPlane, float farPlane,
    float windowAspect, float fov)
{
    // Use enhanced lambda for optimal distribution
    float enhancedLambda = m_splitLambda;
    
    // Compute enhanced split distances with better close-range bias
    std::vector<float> splits = ShadowMapper::ComputeCascadeSplits(
        nearPlane, farPlane, NUM_CASCADES, enhancedLambda
    );

    // CRITICAL DEBUG: Log cascade splits to verify they're reasonable
    static int debugFrameCounter = 0;
    if (debugFrameCounter++ % 300 == 0) { // Log every ~5 seconds at 60fps
        std::cout << "[DirectionalLight] Cascade splits (near=" << nearPlane << ", far=" << farPlane << "):" << std::endl;
        for (int i = 0; i < NUM_CASCADES; ++i) {
            float range = (i == 0) ? (splits[0] - nearPlane) : (splits[i] - splits[i-1]);
            std::cout << "  Cascade " << i << ": " << splits[i] 
                      << " (range: " << range << ", " 
                      << (range / (farPlane - nearPlane) * 100.0f) << "% of total)" << std::endl;
        }
    }

    // Store split distances for shader use
    for (int i = 0; i < NUM_CASCADES; ++i)
        cascadeSplits[i] = splits[i];

    float prevSplit = nearPlane;

    // Enhanced cascade generation with improved stability
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        float splitDist = splits[i];
        
        // Calculate adaptive overlap for smooth transitions
        float overlap = ShadowMapper::ComputeCascadeOverlap(i, NUM_CASCADES, 0.02f);
        float overlapDistance = (splitDist - prevSplit) * overlap;

        // Calculate cascade bounds with smooth overlap
        float cascadeStart = prevSplit;
        float cascadeEnd = splitDist + overlapDistance;

        // Clamp to valid range
        cascadeStart = glm::max(cascadeStart, nearPlane);
        cascadeEnd = glm::min(cascadeEnd, farPlane);

        // CRITICAL FIX: First cascade needs to be MUCH larger
        if (i == 0) {
            cascadeStart = nearPlane;
            // First cascade covers at least 10% of total range (was 3%)
            cascadeEnd = glm::max(splitDist * 1.5f, nearPlane + (farPlane - nearPlane) * 0.10f);
        }

        // Compute enhanced light-space matrix with stability improvements
        m_cascadeLightSpace[i] = ShadowMapper::ComputeCascadeLightSpace(
            cascadeStart, cascadeEnd,
            view, projection,
            GetPosition(), GetDirection(),
            windowAspect, fov,
            i,              // Cascade index for optimization
            m_shadowSize    // Shadow map size for texel snapping
        );

        prevSplit = splitDist;
    }
}

void DirectionalLight::SetCascadeSplits(float sliceNear, float sliceFar, float overlap) {
    m_shadowNearPlane = sliceNear;
    m_shadowFarPlane = sliceFar;
    
    // Use enhanced split calculation
    std::vector<float> splits = ShadowMapper::ComputeCascadeSplits(
        sliceNear, sliceFar, NUM_CASCADES, m_splitLambda
    );
    
    for (int i = 0; i < NUM_CASCADES; ++i) {
        cascadeSplits[i] = splits[i];
    }
    
    // Initialize identity light-space matrices (updated each frame)
    for (auto& mat : m_cascadeLightSpace) mat = glm::mat4(1.0f);
}

// Enhanced PCSS configuration methods
void DirectionalLight::SetPCSSConfig(const ShadowMapper::PCSS::PCSSConfig& config) {
    m_pcssConfig = config;
    std::cout << "[DirectionalLight] PCSS configuration updated:" << std::endl;
    std::cout << "  - Enabled: " << (config.enablePCSS ? "Yes" : "No") << std::endl;
    std::cout << "  - Light size: " << config.lightSize << std::endl;
    std::cout << "  - Blocker samples: " << config.blockerSearchSamples << std::endl;
    std::cout << "  - PCF samples: " << config.pcfSamples << std::endl;
}

const ShadowMapper::PCSS::PCSSConfig& DirectionalLight::GetPCSSConfig() const {
    return m_pcssConfig;
}

void DirectionalLight::EnableEnhancedFiltering(bool enable) {
    m_useEnhancedFiltering = enable;
    
    if (m_shadowFBO) {
        GLuint depthArrayId = m_shadowFBO->GetDepthArray();
        glBindTexture(GL_TEXTURE_2D_ARRAY, depthArrayId);
        
        if (enable) {
            // Enhanced filtering for PCSS
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        } else {
            // Basic filtering
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
}

// Enhanced cascade quality analysis
float DirectionalLight::GetCascadeQuality(int cascadeIndex) const {
    if (cascadeIndex < 0 || cascadeIndex >= NUM_CASCADES) return 0.0f;
    
    // Calculate quality based on texel density and cascade range
    float cascadeRange = (cascadeIndex == 0) ? 
        cascadeSplits[0] : 
        (cascadeSplits[cascadeIndex] - cascadeSplits[cascadeIndex - 1]);
    
    // Higher resolution and smaller range = better quality
    float texelDensity = static_cast<float>(m_shadowSize) / cascadeRange;
    
    // Normalize to 0-1 range (assuming optimal density around 100 texels per unit)
    return glm::clamp(texelDensity / 100.0f, 0.0f, 1.0f);
}

GLuint DirectionalLight::GetDepthArrayID() const {
    return m_shadowFBO->GetDepthArray();
}

GLuint DirectionalLight::GetShadowShaderID() const {
    return m_shadowShaderID;
}

void DirectionalLight::SetShadowShaderID(GLuint shaderID) {
    m_shadowShaderID = shaderID;
}

void DirectionalLight::Serialize(nlohmann::json& json) const {
    BaseLight::Serialize(json);
    json["shadowSize"] = m_shadowSize;
    json["splitLambda"] = m_splitLambda;
    json["shadowNearPlane"] = m_shadowNearPlane;
    json["shadowFarPlane"] = m_shadowFarPlane;
    json["useEnhancedFiltering"] = m_useEnhancedFiltering;
    
    // Serialize PCSS configuration
    json["pcss"] = {
        {"enablePCSS", m_pcssConfig.enablePCSS},
        {"lightSize", m_pcssConfig.lightSize},
        {"blockerSearchSamples", m_pcssConfig.blockerSearchSamples},
        {"pcfSamples", m_pcssConfig.pcfSamples},
        {"minPenumbraSize", m_pcssConfig.minPenumbraSize},
        {"maxPenumbraSize", m_pcssConfig.maxPenumbraSize}
    };
}

void DirectionalLight::Deserialize(const nlohmann::json& json) {
    BaseLight::Deserialize(json);
    
    if (json.contains("shadowSize")) {
        m_shadowSize = json["shadowSize"];
    }
    if (json.contains("splitLambda")) {
        m_splitLambda = json["splitLambda"];
    }
    if (json.contains("shadowNearPlane")) {
        m_shadowNearPlane = json["shadowNearPlane"];
    }
    if (json.contains("shadowFarPlane")) {
        m_shadowFarPlane = json["shadowFarPlane"];
    }
    if (json.contains("useEnhancedFiltering")) {
        m_useEnhancedFiltering = json["useEnhancedFiltering"];
    }
    
    // Deserialize PCSS configuration
    if (json.contains("pcss")) {
        const auto& pcssJson = json["pcss"];
        if (pcssJson.contains("enablePCSS")) m_pcssConfig.enablePCSS = pcssJson["enablePCSS"];
        if (pcssJson.contains("lightSize")) m_pcssConfig.lightSize = pcssJson["lightSize"];
        if (pcssJson.contains("blockerSearchSamples")) m_pcssConfig.blockerSearchSamples = pcssJson["blockerSearchSamples"];
        if (pcssJson.contains("pcfSamples")) m_pcssConfig.pcfSamples = pcssJson["pcfSamples"];
        if (pcssJson.contains("minPenumbraSize")) m_pcssConfig.minPenumbraSize = pcssJson["minPenumbraSize"];
        if (pcssJson.contains("maxPenumbraSize")) m_pcssConfig.maxPenumbraSize = pcssJson["maxPenumbraSize"];
    }
}

std::string DirectionalLight::GetDebugInfo() const {
    std::string base = BaseLight::GetDebugInfo();
    base += "Enhanced Directional Light Shadow System:\n";
    base += "  - Shadow Size: " + std::to_string(m_shadowSize) + "x" + std::to_string(m_shadowSize) + "\n";
    base += "  - Split Lambda: " + std::to_string(m_splitLambda) + "\n";
    base += "  - Shadow Near/Far: " + std::to_string(m_shadowNearPlane) + "/" + std::to_string(m_shadowFarPlane) + "\n";
    base += "  - Enhanced Filtering: " + std::string(m_useEnhancedFiltering ? "Enabled" : "Disabled") + "\n";
    
    base += "  - PCSS Configuration:\n";
    base += "    - Enabled: " + std::string(m_pcssConfig.enablePCSS ? "Yes" : "No") + "\n";
    base += "    - Light Size: " + std::to_string(m_pcssConfig.lightSize) + "\n";
    base += "    - Blocker Samples: " + std::to_string(m_pcssConfig.blockerSearchSamples) + "\n";
    base += "    - PCF Samples: " + std::to_string(m_pcssConfig.pcfSamples) + "\n";
    
    base += "  - Cascade Splits: ";
    for (int i = 0; i < NUM_CASCADES; ++i) {
        base += std::to_string(cascadeSplits[i]);
        base += " (Quality: " + std::to_string(GetCascadeQuality(i) * 100.0f) + "%)";
        if (i < NUM_CASCADES - 1) base += ", ";
    }
    base += "\n";
    
    return base;
}
