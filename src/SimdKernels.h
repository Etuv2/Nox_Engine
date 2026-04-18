#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <array>
#include <cstdint>

enum class SimdBackend : uint8_t {
    Scalar = 0,
    SSE2,
    AVX2
};

struct SimdFrustumPlanes {
    std::array<float, 6> nx{};
    std::array<float, 6> ny{};
    std::array<float, 6> nz{};
    std::array<float, 6> d{};
};

class SimdKernels {
public:
    static SimdBackend DetectBestBackend();
    static const char* GetBackendName(SimdBackend backend);

    static void UpdateWorldBounds(SimdBackend backend,
                                  const std::vector<glm::mat4>& worldTransforms,
                                  const std::vector<uint32_t>& runtimeIndices,
                                  const float* localCenterX,
                                  const float* localCenterY,
                                  const float* localCenterZ,
                                  const float* localRadius,
                                  size_t itemCount,
                                  const uint32_t* itemIndices,
                                  size_t itemIndexCount,
                                  float* outCenterX,
                                  float* outCenterY,
                                  float* outCenterZ,
                                  float* outRadius);

    static void CullSpheres(SimdBackend backend,
                            const SimdFrustumPlanes& frustum,
                            const float* centerX,
                            const float* centerY,
                            const float* centerZ,
                            const float* radius,
                            size_t itemCount,
                            uint8_t* visibleMask);
};
