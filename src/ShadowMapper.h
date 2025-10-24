#pragma once

#include <vector>
#include <glm/glm.hpp>

namespace ShadowMapper {

    // Enhanced cascade split calculation with better close-up detail distribution
    std::vector<float> ComputeCascadeSplits(float nearPlane, float farPlane, int cascadeCount, float lambda = 0.6f);
    
    // Calculate adaptive overlap between cascades for smooth transitions
    float ComputeCascadeOverlap(int cascadeIndex, int totalCascades, float baseOverlap = 0.02f);

    // Enhanced cascade light space matrix computation with stability improvements
    glm::mat4 ComputeCascadeLightSpace(
        float cascadeNear,
        float cascadeFar,
        const glm::mat4& view,
        const glm::mat4& projection,
        const glm::vec3& lightPos,
        const glm::vec3& lightDir,
        float windowAspect,
        float fov,
        int cascadeIndex = 0,
        int shadowMapSize = 2048
    );

    // PCSS (Percentage Closer Soft Shadows) implementation
    namespace PCSS {
        
        // Generate high-quality Poisson disk samples for PCSS filtering
        std::vector<glm::vec2> GeneratePoissonDisk(int sampleCount = 16);
        
        // Calculate search radius for blocker search phase
        float CalculateSearchRadius(float lightSize, float receiverDepth, float nearPlane);
        
        // Calculate penumbra width for filtering phase
        float CalculatePenumbraWidth(float lightSize, float blockerDepth, float receiverDepth);
        
        // PCSS configuration parameters
        struct PCSSConfig {
            int blockerSearchSamples = 16;    // Samples for blocker search
            int pcfSamples = 32;              // Samples for PCF filtering
            float lightSize = 0.02f;          // Light source size for soft shadows
            float minPenumbraSize = 0.001f;   // Minimum penumbra size
            float maxPenumbraSize = 0.05f;    // Maximum penumbra size
            bool enablePCSS = true;           // Enable/disable PCSS
        };
    }

    // Multi-light shadow management
    namespace MultiLight {
        
        struct ShadowLightData {
            glm::vec3 position;
            glm::vec3 direction;
            glm::vec3 color;
            float intensity;
            float range;
            float innerCone; // For spot lights (degrees)
            float outerCone; // For spot lights (degrees)
            int shadowMapIndex;
            bool castsShadows;
            
            enum class Type {
                DIRECTIONAL,
                POINT,
                SPOT
            } type;
        };

        // Calculate optimal shadow map resolution based on light importance
        int CalculateOptimalResolution(const ShadowLightData& light, int baseResolution = 1024);
        
        // Sort lights by importance for shadow casting priority
        void SortLightsByImportance(std::vector<ShadowLightData>& lights);
        
        // Multi-light shadow configuration
        struct MultiLightConfig {
            int maxShadowCastingLights = 8;     // Maximum lights that can cast shadows
            int baseResolution = 1024;          // Base shadow map resolution
            int maxDirectionalLights = 2;       // Max directional lights with shadows
            int maxSpotLights = 4;              // Max spot lights with shadows
            int maxPointLights = 4;             // Max point lights with shadows
            bool dynamicResolution = true;      // Enable dynamic resolution scaling
            bool cascadedDirectional = true;    // Use cascades for directional lights
        };
    }

}
