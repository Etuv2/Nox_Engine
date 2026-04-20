#include "ShadowMapper.h"
#include "ShadowMapper.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>
#include <algorithm>

namespace ShadowMapper {

    // Enhanced cascade split calculation with proper distribution across view frustum
    std::vector<float> ComputeCascadeSplits(float nearPlane, float farPlane, int cascadeCount, float lambda) {
        std::vector<float> splits(cascadeCount);
        float range = farPlane - nearPlane;
        float logBase = farPlane / nearPlane;

        for (int i = 0; i < cascadeCount; ++i) {
            float p = (i + 1) / float(cascadeCount);
            
            // Use standard logarithmic/linear blend (PSSM formula)
            // Logarithmic distribution gives more resolution to near objects
            // Linear distribution spreads evenly across depth range
            float logSplit = nearPlane * std::pow(logBase, p);
            float linSplit = nearPlane + range * p;
            
            splits[i] = lambda * logSplit + (1.0f - lambda) * linSplit;
        }
        
        // NO OVERRIDES - use pure logarithmic/linear blend to ensure proper coverage
        // The lambda parameter controls the distribution:
        //   lambda = 1.0: Pure logarithmic (more near detail, less far coverage)
        //   lambda = 0.0: Pure linear (even distribution)
        //   lambda = 0.5-0.7: Good balance for most scenes
        
        // Only ensure monotonic progression with minimal gap
        for (int i = 1; i < cascadeCount; ++i) {
            if (splits[i] <= splits[i-1]) {
                splits[i] = splits[i-1] + range * 0.01f;
            }
        }
        
        // Ensure last cascade reaches near the far plane
        splits[cascadeCount - 1] = glm::max(splits[cascadeCount - 1], farPlane * 0.95f);
        
        return splits;
    }

    // Enhanced cascade overlap calculation for smoother transitions
    float ComputeCascadeOverlap(int cascadeIndex, int totalCascades, float baseOverlap) {
        // Adaptive overlap: less for close cascades, more for distant ones
        float normalizedIndex = float(cascadeIndex) / float(totalCascades - 1);
        
        // Exponential increase in overlap for distant cascades
        float adaptiveMultiplier = 1.0f + normalizedIndex * normalizedIndex * 2.0f;
        
        return baseOverlap * adaptiveMultiplier;
    }

    // Helper: Get the 8 corners of the cascade frustum in world space.
    static std::vector<glm::vec3> GetFrustumCorners(const glm::mat4& invViewProj) {
        std::vector<glm::vec3> corners;
        corners.reserve(8);
        // NDC corners: each coordinate is either -1 or 1.
        for (int x = -1; x <= 1; x += 2) {
            for (int y = -1; y <= 1; y += 2) {
                for (int z = -1; z <= 1; z += 2) {
                    glm::vec4 corner = invViewProj * glm::vec4(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 1.0f);
                    corner /= corner.w;
                    corners.push_back(glm::vec3(corner));
                }
            }
        }
        return corners;
    }

    // Enhanced cascade light space computation with stability improvements
    glm::mat4 ComputeCascadeLightSpace(
        float cascadeNear,
        float cascadeFar,
        const glm::mat4& view,
        const glm::vec3& lightPos,
        const glm::vec3& lightDir,
        float windowAspect,
        float fitFov,
        int cascadeIndex,
        int shadowMapSize)
    {
        // Add generous overlap to cascade bounds to ensure continuous coverage
        // This is critical to prevent gaps at cascade boundaries
        float cascadeRange = cascadeFar - cascadeNear;
        float overlap = cascadeRange * 0.15f; // 15% overlap on each side (increased from 5%)
        
        // Extend near slightly back (except for first cascade)
        float effectiveNear = (cascadeIndex > 0) ? glm::max(0.01f, cascadeNear - overlap) : cascadeNear;
        // Extend far forward
        float effectiveFar = cascadeFar + overlap;
        
        // Create cascade-specific projection matrix with overlap
        glm::mat4 cascadeProj = glm::perspective(glm::radians(fitFov), windowAspect, effectiveNear, effectiveFar);
        glm::mat4 invCascadeVP = glm::inverse(cascadeProj * view);

        // Get frustum corners in world space
        std::vector<glm::vec3> frustumCorners = GetFrustumCorners(invCascadeVP);

        // Calculate frustum center for stable positioning
        glm::vec3 center(0.0f);
        for (const auto& corner : frustumCorners)
            center += corner;
        center /= static_cast<float>(frustumCorners.size());

        // Calculate tight bounding sphere for the frustum
        float radius = 0.0f;
        for (const auto& corner : frustumCorners) {
            float distance = glm::length(corner - center);
            radius = glm::max(radius, distance);
        }

        // Add margin to radius to ensure full coverage (increased from 2% to 10%)
        radius *= 1.10f;

        // Enhanced texel snapping for rock-solid stability
        float texelSize = (radius * 2.0f) / static_cast<float>(shadowMapSize);
        
        // Snap radius to texel boundaries for pixel-perfect stability
        radius = std::ceil(radius / texelSize) * texelSize;

        // Calculate optimal light camera position
        glm::vec3 lightDirNorm = glm::normalize(lightDir);
        
        // Distance calculation - push light back far enough to capture all geometry
        float lightDistance = radius * 4.0f; // Increased from 3x to 4x
        
        glm::vec3 shadowCamPos = center - lightDirNorm * lightDistance;
        
        // Enhanced up vector calculation to avoid gimbal lock
        glm::vec3 up = glm::abs(glm::dot(lightDirNorm, glm::vec3(0, 1, 0))) > 0.95f ? 
                       glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        
        // Create light view matrix
        glm::mat4 lightView = glm::lookAt(shadowCamPos, center, up);

        // Ultra-precise texel alignment for maximum temporal stability
        glm::vec4 centerLightSpace = lightView * glm::vec4(center, 1.0f);
        
        // Snap to texel grid in light space
        centerLightSpace.x = std::round(centerLightSpace.x / texelSize) * texelSize;
        centerLightSpace.y = std::round(centerLightSpace.y / texelSize) * texelSize;
        
        // Recompute light view with snapped center
        glm::vec3 snappedCenter = glm::vec3(glm::inverse(lightView) * centerLightSpace);
        shadowCamPos = snappedCenter - lightDirNorm * lightDistance;
        lightView = glm::lookAt(shadowCamPos, snappedCenter, up);

        // Create orthographic projection with very generous depth range
        float orthoNear = 0.1f;
        float orthoFar = lightDistance * 2.0f + radius * 3.0f; // More generous far plane
        
        glm::mat4 lightProj = glm::ortho(
            -radius, radius,
            -radius, radius,
            orthoNear, orthoFar
        );
        
        return lightProj * lightView;
    }

    // PCSS (Percentage Closer Soft Shadows) implementation helpers
    namespace PCSS {
        
        // Generate Poisson disk samples for high-quality PCSS filtering
        std::vector<glm::vec2> GeneratePoissonDisk(int sampleCount) {
            std::vector<glm::vec2> samples;
            samples.reserve(sampleCount);
            
            // Pre-calculated Poisson disk samples for different sample counts
            if (sampleCount <= 16) {
                // 16-sample Poisson disk
                const glm::vec2 poisson16[] = {
                    glm::vec2(-0.94201624f, -0.39906216f), glm::vec2(0.94558609f, -0.76890725f),
                    glm::vec2(-0.094184101f, -0.92938870f), glm::vec2(0.34495938f, 0.29387760f),
                    glm::vec2(-0.91588581f, 0.45771432f), glm::vec2(-0.81544232f, -0.87912464f),
                    glm::vec2(-0.38277543f, 0.27676845f), glm::vec2(0.97484398f, 0.75648379f),
                    glm::vec2(0.44323325f, -0.97511554f), glm::vec2(0.53742981f, -0.47373420f),
                    glm::vec2(-0.26496911f, -0.41893023f), glm::vec2(0.79197514f, 0.19090188f),
                    glm::vec2(-0.24188840f, 0.99706507f), glm::vec2(-0.81409955f, 0.91437590f),
                    glm::vec2(0.19984126f, 0.78641367f), glm::vec2(0.14383161f, -0.14100790f)
                };
                for (int i = 0; i < 16; ++i) {
                    samples.push_back(poisson16[i]);
                }
            } else {
                // 32-sample Poisson disk for higher quality
                const glm::vec2 poisson32[] = {
                    glm::vec2(-0.975402f, -0.0711386f), glm::vec2(-0.920347f, -0.41142f),
                    glm::vec2(-0.883908f, 0.217872f), glm::vec2(-0.884518f, 0.568041f),
                    glm::vec2(-0.811945f, 0.90521f), glm::vec2(-0.792474f, -0.779962f),
                    glm::vec2(-0.614856f, 0.386578f), glm::vec2(-0.580859f, -0.208777f),
                    glm::vec2(-0.53795f, 0.716666f), glm::vec2(-0.515427f, 0.0899991f),
                    glm::vec2(-0.454634f, -0.707938f), glm::vec2(-0.420942f, 0.991272f),
                    glm::vec2(-0.261147f, 0.588488f), glm::vec2(-0.211219f, 0.114841f),
                    glm::vec2(-0.146336f, -0.259194f), glm::vec2(-0.139439f, -0.888668f),
                    glm::vec2(0.0116886f, 0.326395f), glm::vec2(0.0380566f, 0.625477f),
                    glm::vec2(0.0625935f, -0.50853f), glm::vec2(0.125584f, 0.0469069f),
                    glm::vec2(0.169469f, -0.997253f), glm::vec2(0.320597f, 0.291055f),
                    glm::vec2(0.359172f, -0.143327f), glm::vec2(0.435713f, -0.396485f),
                    glm::vec2(0.507797f, -0.755471f), glm::vec2(0.507839f, 0.848101f),
                    glm::vec2(0.681467f, 0.11457f), glm::vec2(0.699901f, 0.503325f),
                    glm::vec2(0.728804f, -0.188618f), glm::vec2(0.742757f, -0.439693f),
                    glm::vec2(0.85963f, 0.243204f), glm::vec2(0.896665f, 0.56216f)
                };
                for (int i = 0; i < 32; ++i) {
                    samples.push_back(poisson32[i]);
                }
            }
            
            return samples;
        }

        // Calculate optimal search radius for blocker search
        float CalculateSearchRadius(float lightSize, float receiverDepth, float nearPlane) {
            return lightSize * (receiverDepth - nearPlane) / receiverDepth;
        }

        // Calculate penumbra width for PCF kernel size
        float CalculatePenumbraWidth(float lightSize, float blockerDepth, float receiverDepth) {
            if (blockerDepth <= 0.0f || receiverDepth <= blockerDepth) {
                return 0.0f;
            }
            return lightSize * (receiverDepth - blockerDepth) / blockerDepth;
        }
    }

    // Multi-light shadow management
    namespace MultiLight {
        
        // Calculate optimal shadow map resolution based on light importance
        int CalculateOptimalResolution(const ShadowLightData& light, int baseResolution) {
            float importanceFactor = 1.0f;
            
            // Adjust based on light type
            switch (light.type) {
                case ShadowLightData::Type::DIRECTIONAL:
                    importanceFactor = 1.5f; // Directional lights get highest priority
                    break;
                case ShadowLightData::Type::SPOT:
                    importanceFactor = 1.2f; // Spot lights are important for focused lighting
                    break;
                case ShadowLightData::Type::POINT:
                    importanceFactor = 1.0f; // Point lights get base resolution
                    break;
            }
            
            // Scale by intensity
            importanceFactor *= glm::clamp(light.intensity, 0.5f, 2.0f);
            
            // Calculate final resolution
            int finalResolution = static_cast<int>(baseResolution * importanceFactor);
            
            // Clamp to reasonable bounds and ensure power of 2
            finalResolution = glm::clamp(finalResolution, 512, 4096);
            
            // Round to nearest power of 2
            int powerOf2 = 1;
            while (powerOf2 < finalResolution) {
                powerOf2 <<= 1;
            }
            
            return powerOf2;
        }

        // Sort lights by importance for shadow casting priority
        void SortLightsByImportance(std::vector<ShadowLightData>& lights) {
            std::sort(lights.begin(), lights.end(), [](const ShadowLightData& a, const ShadowLightData& b) {
                // Directional lights first
                if (a.type != b.type) {
                    if (a.type == ShadowLightData::Type::DIRECTIONAL) return true;
                    if (b.type == ShadowLightData::Type::DIRECTIONAL) return false;
                }
                
                // Then by intensity
                return a.intensity > b.intensity;
            });
        }
    }

}
