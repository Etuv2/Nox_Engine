#pragma once

#include "../RenderPass.h"
#include "../ComputeShader.h"
#include <GL/glew.h>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>

class RenderSystem;

class SurfelGIPass : public RenderPass {
public:
    static constexpr uint32_t kMaxSurfels = 131072u;
    static constexpr uint32_t kStatsHistoryLength = 180u;

    struct Stats {
        uint32_t liveCount = 0;
        uint32_t freeCount = 0;
        uint32_t spawnedThisFrame = 0;
        uint32_t recycledThisFrame = 0;
        uint32_t dormantCount = 0;
        uint32_t spawnAttemptsThisFrame = 0;
        uint32_t coveragePreventedSpawns = 0;
        uint32_t duplicateRejectedSpawns = 0;
        uint32_t probabilityRejectedSpawns = 0;
        uint32_t freeStackExhaustedSpawns = 0;
        uint32_t oldCoverageContributions = 0;
        uint32_t newCoverageContributions = 0;
        uint32_t oldIntegratedContributions = 0;
        uint32_t newIntegratedContributions = 0;
        uint32_t recycleCandidateCount = 0;
        uint32_t budgetRecycledCount = 0;
        uint32_t invalidTransformRecycledCount = 0;
        uint32_t lifecycleViolationCount = 0;
        uint32_t visibleSurfacePixelCount = 0;
        uint32_t validCoveragePixelCount = 0;
        float validCoveragePercent = 0.0f;
        uint32_t tileCountX = 0;
        uint32_t tileCountY = 0;
        uint32_t gridCellCount = 0;
    };

    SurfelGIPass();
    ~SurfelGIPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
        const std::shared_ptr<SceneGraph>& sceneGraph,
        const std::shared_ptr<Camera>& camera,
        const std::shared_ptr<DirectionalLight>& dirLight,
        const std::shared_ptr<Skybox>& skybox) override;

    void RenderDebug(RenderContext& ctx) const;

    GLuint GetSurfelBuffer() const { return m_surfelSSBO; }
    GLuint GetGridHeaderBuffer() const { return m_gridHeaderSSBO; }
    GLuint GetGridEntryBuffer() const { return m_gridEntrySSBO; }
    const Stats& GetLastStats() const { return m_lastStats; }
    const std::array<float, kStatsHistoryLength>& GetLiveHistory() const { return m_liveHistory; }
    const std::array<float, kStatsHistoryLength>& GetFreeHistory() const { return m_freeHistory; }
    const std::array<float, kStatsHistoryLength>& GetSpawnHistory() const { return m_spawnHistory; }
    const std::array<float, kStatsHistoryLength>& GetRecycleHistory() const { return m_recycleHistory; }
    const std::array<float, kStatsHistoryLength>& GetPreventedSpawnHistory() const { return m_preventedSpawnHistory; }
    const std::array<float, kStatsHistoryLength>& GetOldContributionHistory() const { return m_oldContributionHistory; }
    const std::array<float, kStatsHistoryLength>& GetNewContributionHistory() const { return m_newContributionHistory; }
    uint32_t GetStatsHistoryHead() const { return m_statsHistoryHead; }

private:
    struct GpuSurfelRecord {
        glm::vec4 worldPositionRadius{ 0.0f };
        glm::vec4 localPositionAge{ 0.0f };
        glm::vec4 worldNormalRecycle{ 0.0f };
        glm::vec4 localNormalDebug{ 0.0f };
        glm::uvec4 ids{ 0u };
        glm::uvec4 frames{ 0u };
        glm::uvec4 grid{ 0u };
        glm::vec4 metrics{ 0.0f };
        glm::vec4 irradianceHistory{ 0.0f };
        glm::vec4 shortTermStats{ 0.0f };
        glm::vec4 longTermStats{ 0.0f };
        glm::vec4 recycleData{ 0.0f };
        glm::vec4 depthMoments{ 0.0f };
        glm::vec4 guidingState{ 0.0f };
    };
    static_assert(sizeof(GpuSurfelRecord) == 224, "GpuSurfelRecord must match shaders/includes/surfel_gi_common.glsl SurfelRecord std430 layout.");

    struct GpuPoolHeader {
        glm::uvec4 counts{ 0u };      // x=maxSurfels,y=live,z=free,w=frame
        glm::uvec4 frameStats{ 0u };  // x=spawned,y=recycled,z=dormant,w=spawn attempts
        glm::uvec4 tiling{ 0u };      // x=tileCountX,y=tileCountY,z=tileSize,w=gridCellCount
        glm::uvec4 gridDims{ 0u };    // x,y,z dims,w=maxEntriesPerCell
        glm::vec4 gridParams{ 0.0f }; // x=centralExtent,y=nearScale,z=farScale,w=cellWorldSize
        glm::uvec4 coverageStats{ 0u };
        glm::uvec4 contributionStats{ 0u };
        glm::uvec4 recycleStats{ 0u };
        glm::uvec4 coverageMetricStats{ 0u };
    };
    static_assert(sizeof(GpuPoolHeader) == 144, "GpuPoolHeader must match shaders/includes/surfel_gi_common.glsl SurfelPoolHeader std430 layout.");

    void EnsureResources(RenderContext& ctx);
    void ReleaseBuffers();
    void InitializePool();
    void ResetFrameStats(uint32_t frameIndex);
    void RunPersistentStateUpdate(RenderContext& ctx, RenderSystem& renderSystem, const std::shared_ptr<Camera>& camera);
    void RunRecycleDecision(RenderContext& ctx);
    void RebuildSpatialGrid(const glm::mat4& view);
    void RunPersistentTileCoverage(RenderContext& ctx);
    void RunProjectedCoverage(RenderContext& ctx, const glm::mat4& invViewProj, uint32_t maxTransformID);
    void RunCoverageDeficit(RenderContext& ctx);
    void RunCoverageGapFill(RenderContext& ctx, RenderSystem& renderSystem, const std::shared_ptr<Camera>& camera, const glm::mat4& invViewProj, const glm::mat4& invView);
    void RunIrradianceIntegration(RenderContext& ctx, const std::shared_ptr<DirectionalLight>& dirLight);
    void PublishResources(RenderContext& ctx) const;
    void ReadBackStats();
    void PushStatsHistory();
    void BindCommonBuffers(GLuint transformBuffer) const;

    GLuint m_surfelSSBO = 0;
    GLuint m_headerSSBO = 0;
    GLuint m_freeStackSSBO = 0;
    GLuint m_recycleStackSSBO = 0;
    GLuint m_tileCoverageSSBO = 0;
    GLuint m_gridHeaderSSBO = 0;
    GLuint m_gridEntrySSBO = 0;
    GLuint m_rawProjectedSupportTex = 0;
    GLuint m_projectedCoverageTex = 0;
    GLuint m_depthRejectTex = 0;
    GLuint m_normalRejectTex = 0;
    GLuint m_winnerIDTex = 0;
    GLuint m_coverageHistoryTex = 0;
    GLuint m_deficitTex = 0;
    GLuint m_statsReadbackPBO = 0;
    GLuint m_debugVAO = 0;
    uint32_t m_coverageWidth = 0;
    uint32_t m_coverageHeight = 0;

    std::unique_ptr<ComputeShader> m_initShader;
    std::unique_ptr<ComputeShader> m_lifecycleShader;
    std::unique_ptr<ComputeShader> m_recycleShader;
    std::unique_ptr<ComputeShader> m_tileClearShader;
    std::unique_ptr<ComputeShader> m_tileCoverageShader;
    std::unique_ptr<ComputeShader> m_projectCoverageShader;
    std::unique_ptr<ComputeShader> m_deficitShader;
    std::unique_ptr<ComputeShader> m_gridClearShader;
    std::unique_ptr<ComputeShader> m_gridBuildShader;
    std::unique_ptr<ComputeShader> m_spawnShader;
    std::unique_ptr<ComputeShader> m_integrateShader;
    GLuint m_debugProgram = 0;
    GLuint m_debugOverlayProgram = 0;

    uint32_t m_allocatedTileCountX = 0;
    uint32_t m_allocatedTileCountY = 0;
    uint32_t m_allocatedGridCellCount = 0;
    uint32_t m_projectedCoverageMaxTransformID = 0;
    uint32_t m_frameIndex = 0;
    bool m_needsPoolInit = true;

    Stats m_lastStats{};
    std::array<float, kStatsHistoryLength> m_liveHistory{};
    std::array<float, kStatsHistoryLength> m_freeHistory{};
    std::array<float, kStatsHistoryLength> m_spawnHistory{};
    std::array<float, kStatsHistoryLength> m_recycleHistory{};
    std::array<float, kStatsHistoryLength> m_preventedSpawnHistory{};
    std::array<float, kStatsHistoryLength> m_oldContributionHistory{};
    std::array<float, kStatsHistoryLength> m_newContributionHistory{};
    uint32_t m_statsHistoryHead = 0;
};
