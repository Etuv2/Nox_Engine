#include "SimdKernels.h"

#include <SDL/SDL_cpuinfo.h>
#include <algorithm>
#include <cmath>
#include <xmmintrin.h>
#include <emmintrin.h>
#if defined(__AVX2__) || defined(_MSC_VER)
#include <immintrin.h>
#endif

namespace {
inline float ExtractMaxScaleScalar(const glm::mat4& transform)
{
    const glm::vec3 basisX(transform[0]);
    const glm::vec3 basisY(transform[1]);
    const glm::vec3 basisZ(transform[2]);
    return std::max({ glm::length(basisX), glm::length(basisY), glm::length(basisZ), 1e-6f });
}

void UpdateWorldBoundsScalar(const std::vector<glm::mat4>& worldTransforms,
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
                             float* outRadius)
{
    for (size_t n = 0; n < itemIndexCount; ++n) {
        const uint32_t itemIndex = itemIndices ? itemIndices[n] : static_cast<uint32_t>(n);
        if (itemIndex >= itemCount) {
            continue;
        }

        const uint32_t runtimeIndex = runtimeIndices[itemIndex];
        if (runtimeIndex >= worldTransforms.size()) {
            continue;
        }

        const glm::mat4& world = worldTransforms[runtimeIndex];
        const glm::vec4 worldCenter = world * glm::vec4(localCenterX[itemIndex],
                                                        localCenterY[itemIndex],
                                                        localCenterZ[itemIndex],
                                                        1.0f);
        outCenterX[itemIndex] = worldCenter.x;
        outCenterY[itemIndex] = worldCenter.y;
        outCenterZ[itemIndex] = worldCenter.z;
        outRadius[itemIndex] = localRadius[itemIndex] * ExtractMaxScaleScalar(world);
    }
}

void CullSpheresScalar(const SimdFrustumPlanes& frustum,
                       const float* centerX,
                       const float* centerY,
                       const float* centerZ,
                       const float* radius,
                       size_t itemCount,
                       uint8_t* visibleMask)
{
    for (size_t i = 0; i < itemCount; ++i) {
        bool visible = true;
        for (size_t plane = 0; plane < 6; ++plane) {
            const float distance = frustum.nx[plane] * centerX[i] +
                                   frustum.ny[plane] * centerY[i] +
                                   frustum.nz[plane] * centerZ[i] +
                                   frustum.d[plane];
            if (distance < -radius[i]) {
                visible = false;
                break;
            }
        }
        visibleMask[i] = visible ? 1u : 0u;
    }
}

void UpdateWorldBoundsSSE2(const std::vector<glm::mat4>& worldTransforms,
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
                           float* outRadius)
{
    size_t n = 0;
    alignas(16) float centerX[4];
    alignas(16) float centerY[4];
    alignas(16) float centerZ[4];
    alignas(16) float radius[4];

    for (; n + 3 < itemIndexCount; n += 4) {
        uint32_t items[4];
        for (size_t lane = 0; lane < 4; ++lane) {
            items[lane] = itemIndices ? itemIndices[n + lane] : static_cast<uint32_t>(n + lane);
            if (items[lane] >= itemCount) {
                items[lane] = 0;
            }
        }

        auto gather = [&](int column, int row) -> __m128 {
            return _mm_set_ps(
                worldTransforms[runtimeIndices[items[3]]][column][row],
                worldTransforms[runtimeIndices[items[2]]][column][row],
                worldTransforms[runtimeIndices[items[1]]][column][row],
                worldTransforms[runtimeIndices[items[0]]][column][row]);
        };

        const __m128 lx = _mm_set_ps(localCenterX[items[3]], localCenterX[items[2]], localCenterX[items[1]], localCenterX[items[0]]);
        const __m128 ly = _mm_set_ps(localCenterY[items[3]], localCenterY[items[2]], localCenterY[items[1]], localCenterY[items[0]]);
        const __m128 lz = _mm_set_ps(localCenterZ[items[3]], localCenterZ[items[2]], localCenterZ[items[1]], localCenterZ[items[0]]);
        const __m128 lr = _mm_set_ps(localRadius[items[3]], localRadius[items[2]], localRadius[items[1]], localRadius[items[0]]);

        const __m128 m00 = gather(0, 0);
        const __m128 m01 = gather(0, 1);
        const __m128 m02 = gather(0, 2);
        const __m128 m10 = gather(1, 0);
        const __m128 m11 = gather(1, 1);
        const __m128 m12 = gather(1, 2);
        const __m128 m20 = gather(2, 0);
        const __m128 m21 = gather(2, 1);
        const __m128 m22 = gather(2, 2);
        const __m128 tx = gather(3, 0);
        const __m128 ty = gather(3, 1);
        const __m128 tz = gather(3, 2);

        const __m128 worldX = _mm_add_ps(_mm_add_ps(_mm_mul_ps(m00, lx), _mm_mul_ps(m10, ly)),
                                         _mm_add_ps(_mm_mul_ps(m20, lz), tx));
        const __m128 worldY = _mm_add_ps(_mm_add_ps(_mm_mul_ps(m01, lx), _mm_mul_ps(m11, ly)),
                                         _mm_add_ps(_mm_mul_ps(m21, lz), ty));
        const __m128 worldZ = _mm_add_ps(_mm_add_ps(_mm_mul_ps(m02, lx), _mm_mul_ps(m12, ly)),
                                         _mm_add_ps(_mm_mul_ps(m22, lz), tz));

        const __m128 scaleX = _mm_sqrt_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(m00, m00), _mm_mul_ps(m01, m01)), _mm_mul_ps(m02, m02)));
        const __m128 scaleY = _mm_sqrt_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(m10, m10), _mm_mul_ps(m11, m11)), _mm_mul_ps(m12, m12)));
        const __m128 scaleZ = _mm_sqrt_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(m20, m20), _mm_mul_ps(m21, m21)), _mm_mul_ps(m22, m22)));
        const __m128 maxScale = _mm_max_ps(_mm_max_ps(scaleX, scaleY), _mm_max_ps(scaleZ, _mm_set1_ps(1e-6f)));
        const __m128 worldR = _mm_mul_ps(lr, maxScale);

        _mm_store_ps(centerX, worldX);
        _mm_store_ps(centerY, worldY);
        _mm_store_ps(centerZ, worldZ);
        _mm_store_ps(radius, worldR);

        for (size_t lane = 0; lane < 4; ++lane) {
            const uint32_t itemIndex = items[lane];
            outCenterX[itemIndex] = centerX[lane];
            outCenterY[itemIndex] = centerY[lane];
            outCenterZ[itemIndex] = centerZ[lane];
            outRadius[itemIndex] = radius[lane];
        }
    }

    if (n < itemIndexCount) {
        UpdateWorldBoundsScalar(worldTransforms, runtimeIndices, localCenterX, localCenterY, localCenterZ,
                                localRadius, itemCount, itemIndices ? itemIndices + n : nullptr,
                                itemIndexCount - n, outCenterX, outCenterY, outCenterZ, outRadius);
    }
}

void CullSpheresSSE2(const SimdFrustumPlanes& frustum,
                    const float* centerX,
                    const float* centerY,
                    const float* centerZ,
                    const float* radius,
                    size_t itemCount,
                    uint8_t* visibleMask)
{
    size_t i = 0;
    alignas(16) float maskValues[4];

    for (; i + 3 < itemCount; i += 4) {
        __m128 visible = _mm_castsi128_ps(_mm_set1_epi32(-1));
        const __m128 cx = _mm_loadu_ps(centerX + i);
        const __m128 cy = _mm_loadu_ps(centerY + i);
        const __m128 cz = _mm_loadu_ps(centerZ + i);
        const __m128 cr = _mm_loadu_ps(radius + i);

        for (size_t plane = 0; plane < 6; ++plane) {
            const __m128 dist = _mm_add_ps(
                _mm_add_ps(_mm_mul_ps(_mm_set1_ps(frustum.nx[plane]), cx),
                           _mm_mul_ps(_mm_set1_ps(frustum.ny[plane]), cy)),
                _mm_add_ps(_mm_mul_ps(_mm_set1_ps(frustum.nz[plane]), cz),
                           _mm_set1_ps(frustum.d[plane])));
            const __m128 inside = _mm_cmpge_ps(dist, _mm_sub_ps(_mm_setzero_ps(), cr));
            visible = _mm_and_ps(visible, inside);
        }

        _mm_store_ps(maskValues, visible);
        for (int lane = 0; lane < 4; ++lane) {
            visibleMask[i + lane] = (maskValues[lane] != 0.0f) ? 1u : 0u;
        }
    }

    if (i < itemCount) {
        CullSpheresScalar(frustum, centerX + i, centerY + i, centerZ + i, radius + i, itemCount - i, visibleMask + i);
    }
}

void UpdateWorldBoundsAVX2(const std::vector<glm::mat4>& worldTransforms,
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
                           float* outRadius)
{
#if defined(__AVX2__)
    size_t n = 0;
    alignas(32) float centerX[8];
    alignas(32) float centerY[8];
    alignas(32) float centerZ[8];
    alignas(32) float radius[8];

    for (; n + 7 < itemIndexCount; n += 8) {
        uint32_t items[8];
        for (size_t lane = 0; lane < 8; ++lane) {
            items[lane] = itemIndices ? itemIndices[n + lane] : static_cast<uint32_t>(n + lane);
            if (items[lane] >= itemCount) {
                items[lane] = 0;
            }
        }

        auto gather = [&](int column, int row) -> __m256 {
            return _mm256_set_ps(
                worldTransforms[runtimeIndices[items[7]]][column][row],
                worldTransforms[runtimeIndices[items[6]]][column][row],
                worldTransforms[runtimeIndices[items[5]]][column][row],
                worldTransforms[runtimeIndices[items[4]]][column][row],
                worldTransforms[runtimeIndices[items[3]]][column][row],
                worldTransforms[runtimeIndices[items[2]]][column][row],
                worldTransforms[runtimeIndices[items[1]]][column][row],
                worldTransforms[runtimeIndices[items[0]]][column][row]);
        };

        const __m256 lx = _mm256_set_ps(localCenterX[items[7]], localCenterX[items[6]], localCenterX[items[5]], localCenterX[items[4]],
                                        localCenterX[items[3]], localCenterX[items[2]], localCenterX[items[1]], localCenterX[items[0]]);
        const __m256 ly = _mm256_set_ps(localCenterY[items[7]], localCenterY[items[6]], localCenterY[items[5]], localCenterY[items[4]],
                                        localCenterY[items[3]], localCenterY[items[2]], localCenterY[items[1]], localCenterY[items[0]]);
        const __m256 lz = _mm256_set_ps(localCenterZ[items[7]], localCenterZ[items[6]], localCenterZ[items[5]], localCenterZ[items[4]],
                                        localCenterZ[items[3]], localCenterZ[items[2]], localCenterZ[items[1]], localCenterZ[items[0]]);
        const __m256 lr = _mm256_set_ps(localRadius[items[7]], localRadius[items[6]], localRadius[items[5]], localRadius[items[4]],
                                        localRadius[items[3]], localRadius[items[2]], localRadius[items[1]], localRadius[items[0]]);

        const __m256 m00 = gather(0, 0);
        const __m256 m01 = gather(0, 1);
        const __m256 m02 = gather(0, 2);
        const __m256 m10 = gather(1, 0);
        const __m256 m11 = gather(1, 1);
        const __m256 m12 = gather(1, 2);
        const __m256 m20 = gather(2, 0);
        const __m256 m21 = gather(2, 1);
        const __m256 m22 = gather(2, 2);
        const __m256 tx = gather(3, 0);
        const __m256 ty = gather(3, 1);
        const __m256 tz = gather(3, 2);

        const __m256 worldX = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m00, lx), _mm256_mul_ps(m10, ly)),
                                            _mm256_add_ps(_mm256_mul_ps(m20, lz), tx));
        const __m256 worldY = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m01, lx), _mm256_mul_ps(m11, ly)),
                                            _mm256_add_ps(_mm256_mul_ps(m21, lz), ty));
        const __m256 worldZ = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m02, lx), _mm256_mul_ps(m12, ly)),
                                            _mm256_add_ps(_mm256_mul_ps(m22, lz), tz));

        const __m256 scaleX = _mm256_sqrt_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m00, m00), _mm256_mul_ps(m01, m01)), _mm256_mul_ps(m02, m02)));
        const __m256 scaleY = _mm256_sqrt_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m10, m10), _mm256_mul_ps(m11, m11)), _mm256_mul_ps(m12, m12)));
        const __m256 scaleZ = _mm256_sqrt_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(m20, m20), _mm256_mul_ps(m21, m21)), _mm256_mul_ps(m22, m22)));
        const __m256 maxScale = _mm256_max_ps(_mm256_max_ps(scaleX, scaleY), _mm256_max_ps(scaleZ, _mm256_set1_ps(1e-6f)));
        const __m256 worldR = _mm256_mul_ps(lr, maxScale);

        _mm256_store_ps(centerX, worldX);
        _mm256_store_ps(centerY, worldY);
        _mm256_store_ps(centerZ, worldZ);
        _mm256_store_ps(radius, worldR);

        for (size_t lane = 0; lane < 8; ++lane) {
            const uint32_t itemIndex = items[lane];
            outCenterX[itemIndex] = centerX[lane];
            outCenterY[itemIndex] = centerY[lane];
            outCenterZ[itemIndex] = centerZ[lane];
            outRadius[itemIndex] = radius[lane];
        }
    }

    if (n < itemIndexCount) {
        UpdateWorldBoundsSSE2(worldTransforms, runtimeIndices, localCenterX, localCenterY, localCenterZ,
                              localRadius, itemCount, itemIndices ? itemIndices + n : nullptr,
                              itemIndexCount - n, outCenterX, outCenterY, outCenterZ, outRadius);
    }
#else
    UpdateWorldBoundsSSE2(worldTransforms, runtimeIndices, localCenterX, localCenterY, localCenterZ,
                          localRadius, itemCount, itemIndices, itemIndexCount,
                          outCenterX, outCenterY, outCenterZ, outRadius);
#endif
}

void CullSpheresAVX2(const SimdFrustumPlanes& frustum,
                    const float* centerX,
                    const float* centerY,
                    const float* centerZ,
                    const float* radius,
                    size_t itemCount,
                    uint8_t* visibleMask)
{
#if defined(__AVX2__)
    size_t i = 0;
    alignas(32) float maskValues[8];

    for (; i + 7 < itemCount; i += 8) {
        __m256 visible = _mm256_castsi256_ps(_mm256_set1_epi32(-1));
        const __m256 cx = _mm256_loadu_ps(centerX + i);
        const __m256 cy = _mm256_loadu_ps(centerY + i);
        const __m256 cz = _mm256_loadu_ps(centerZ + i);
        const __m256 cr = _mm256_loadu_ps(radius + i);

        for (size_t plane = 0; plane < 6; ++plane) {
            const __m256 dist = _mm256_add_ps(
                _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(frustum.nx[plane]), cx),
                              _mm256_mul_ps(_mm256_set1_ps(frustum.ny[plane]), cy)),
                _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(frustum.nz[plane]), cz),
                              _mm256_set1_ps(frustum.d[plane])));
            const __m256 inside = _mm256_cmp_ps(dist, _mm256_sub_ps(_mm256_setzero_ps(), cr), _CMP_GE_OQ);
            visible = _mm256_and_ps(visible, inside);
        }

        _mm256_store_ps(maskValues, visible);
        for (int lane = 0; lane < 8; ++lane) {
            visibleMask[i + lane] = (maskValues[lane] != 0.0f) ? 1u : 0u;
        }
    }

    if (i < itemCount) {
        CullSpheresSSE2(frustum, centerX + i, centerY + i, centerZ + i, radius + i, itemCount - i, visibleMask + i);
    }
#else
    CullSpheresSSE2(frustum, centerX, centerY, centerZ, radius, itemCount, visibleMask);
#endif
}
}

SimdBackend SimdKernels::DetectBestBackend()
{
    if (SDL_HasAVX2()) {
        return SimdBackend::AVX2;
    }
    if (SDL_HasSSE2()) {
        return SimdBackend::SSE2;
    }
    return SimdBackend::Scalar;
}

const char* SimdKernels::GetBackendName(SimdBackend backend)
{
    switch (backend) {
    case SimdBackend::AVX2: return "AVX2";
    case SimdBackend::SSE2: return "SSE2";
    default: return "Scalar";
    }
}

void SimdKernels::UpdateWorldBounds(SimdBackend backend,
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
                                    float* outRadius)
{
    switch (backend) {
    case SimdBackend::AVX2:
        UpdateWorldBoundsAVX2(worldTransforms, runtimeIndices, localCenterX, localCenterY, localCenterZ,
                              localRadius, itemCount, itemIndices, itemIndexCount,
                              outCenterX, outCenterY, outCenterZ, outRadius);
        break;
    case SimdBackend::SSE2:
        UpdateWorldBoundsSSE2(worldTransforms, runtimeIndices, localCenterX, localCenterY, localCenterZ,
                              localRadius, itemCount, itemIndices, itemIndexCount,
                              outCenterX, outCenterY, outCenterZ, outRadius);
        break;
    default:
        UpdateWorldBoundsScalar(worldTransforms, runtimeIndices, localCenterX, localCenterY, localCenterZ,
                                localRadius, itemCount, itemIndices, itemIndexCount,
                                outCenterX, outCenterY, outCenterZ, outRadius);
        break;
    }
}

void SimdKernels::CullSpheres(SimdBackend backend,
                              const SimdFrustumPlanes& frustum,
                              const float* centerX,
                              const float* centerY,
                              const float* centerZ,
                              const float* radius,
                              size_t itemCount,
                              uint8_t* visibleMask)
{
    switch (backend) {
    case SimdBackend::AVX2:
        CullSpheresAVX2(frustum, centerX, centerY, centerZ, radius, itemCount, visibleMask);
        break;
    case SimdBackend::SSE2:
        CullSpheresSSE2(frustum, centerX, centerY, centerZ, radius, itemCount, visibleMask);
        break;
    default:
        CullSpheresScalar(frustum, centerX, centerY, centerZ, radius, itemCount, visibleMask);
        break;
    }
}
