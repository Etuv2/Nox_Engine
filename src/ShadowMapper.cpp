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

    FrustumSliceSphere ComputeFrustumSliceSphere(float sliceNear, float sliceFar, float fovYDegrees, float aspect)
    {
        const float tanY = std::tan(glm::radians(fovYDegrees) * 0.5f);
        const float tanX = tanY * aspect;
        // Squared slope of the frustum's corner rays: a corner at view depth z sits z * sqrt(k2)
        // away from the forward axis.
        const float k2 = tanX * tanX + tanY * tanY;

        // Center on the forward axis equidistant from the near and far corners.
        const float center = 0.5f * (sliceFar + sliceNear) * (1.0f + k2);
        if (center >= sliceFar) {
            // Wide or thin slices: the far cap alone bounds the slice.
            return { sliceFar, sliceFar * std::sqrt(k2) };
        }

        const float dz = sliceFar - center;
        return { center, std::sqrt(dz * dz + sliceFar * sliceFar * k2) };
    }

    glm::mat4 ComputeCascadeLightSpace(
        float cascadeNear,
        float cascadeFar,
        const glm::mat4& view,
        const glm::vec3& /*lightPos*/,
        const glm::vec3& lightDir,
        float windowAspect,
        float fitFov,
        int cascadeIndex,
        int shadowMapSize)
    {
        const float cascadeRange = std::max(cascadeFar - cascadeNear, 1e-3f);
        const float sliceNear = (cascadeIndex > 0)
            ? std::max(0.01f, cascadeNear - cascadeRange * kCascadeBlendOverlap)
            : cascadeNear;
        const FrustumSliceSphere sphere = ComputeFrustumSliceSphere(sliceNear, cascadeFar, fitFov, windowAspect);

        const float resolution = static_cast<float>(std::max(shadowMapSize, 1));
        // Keep the texel size on a fixed grid so float noise in the inputs can never nudge it.
        const float radius = std::ceil(sphere.radius * 64.0f) / 64.0f;
        const float texelWorld = (2.0f * radius) / resolution;

        const glm::mat4 invView = glm::inverse(view);
        const glm::vec3 cameraPos = glm::vec3(invView[3]);
        const glm::vec3 cameraForward = -glm::normalize(glm::vec3(invView[2]));
        const glm::vec3 center = cameraPos + cameraForward * sphere.centerDistance;

        // Rotation-only light basis: it does not move with the camera, so rounding the cascade
        // center to whole texels in it pins the shadow texel grid to world space.
        const glm::vec3 dir = glm::normalize(lightDir);
        const glm::vec3 up = std::abs(dir.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 lightRotation = glm::lookAt(glm::vec3(0.0f), dir, up);

        glm::vec3 lightCenter = glm::vec3(lightRotation * glm::vec4(center, 1.0f));
        lightCenter.x = std::round(lightCenter.x / texelWorld) * texelWorld;
        lightCenter.y = std::round(lightCenter.y / texelWorld) * texelWorld;

        // The light looks down -Z, so the sphere spans view depths [-z - r, -z + r].
        const float depthNear = -lightCenter.z - radius;
        const float depthFar = -lightCenter.z + radius;
        const glm::mat4 lightProj = glm::ortho(
            lightCenter.x - radius, lightCenter.x + radius,
            lightCenter.y - radius, lightCenter.y + radius,
            depthNear, depthFar);

        return lightProj * lightRotation;
    }

    bool SphereIntersectsShadowCasterVolume(const glm::mat4& lightSpace, const glm::vec3& center, float radius)
    {
        // Gribb/Hartmann plane extraction; glm is column-major, so row i is (m[0][i], m[1][i], m[2][i], m[3][i]).
        auto row = [&](int i) {
            return glm::vec4(lightSpace[0][i], lightSpace[1][i], lightSpace[2][i], lightSpace[3][i]);
        };
        const glm::vec4 r0 = row(0);
        const glm::vec4 r1 = row(1);
        const glm::vec4 r2 = row(2);
        const glm::vec4 r3 = row(3);
        const glm::vec4 planes[5] = {
            r3 + r0, // left
            r3 - r0, // right
            r3 + r1, // bottom
            r3 - r1, // top
            r3 - r2, // far
        };

        for (const glm::vec4& plane : planes) {
            const float normalLength = glm::length(glm::vec3(plane));
            if (normalLength <= 1e-12f) {
                continue;
            }
            const float distance = (glm::dot(glm::vec3(plane), center) + plane.w) / normalLength;
            if (distance < -radius) {
                return false;
            }
        }
        return true;
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
