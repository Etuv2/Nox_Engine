#include "SurfelGIPass.h"

#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../RenderSystem.h"
#include "../ScreenQuad.h"
#include "../SceneGraph.h"
#include "../ShaderLoader.h"
#include "../RTSceneResources.h"
#include "../TextureUnits.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <vector>

namespace {
    constexpr GLuint kBindingSurfels = 20;
    constexpr GLuint kBindingHeader = 21;
    constexpr GLuint kBindingFreeStack = 22;
    constexpr GLuint kBindingRecycleStack = 23;
    constexpr GLuint kBindingTileCoverage = 24;
    constexpr GLuint kBindingGridHeaders = 25;
    constexpr GLuint kBindingGridEntries = 26;
    constexpr GLuint kBindingTileQueue = 27;
    constexpr GLuint kBindingIrradianceHeader = 28;
    constexpr GLuint kBindingGuidingBins = 29;
    constexpr GLuint kBindingRadialDepthBins = 30;
    constexpr GLuint kBindingTransforms = 6;
    constexpr GLuint kImageProjectedCoverage = 0;
    constexpr GLuint kImageDeficit = 1;
    constexpr GLuint kImageRawProjectedSupport = 2;
    constexpr GLuint kImageDepthReject = 3;
    constexpr GLuint kImageNormalReject = 4;
    constexpr GLuint kImageWinnerID = 5;
    constexpr GLuint kImageCoverageHistory = 6;

    constexpr uint32_t kSurfelTileSize = 16;
    constexpr uint32_t kTileQueueBucketCount = 4;
    constexpr float kFreePoolReserveFraction = 0.02f;
    constexpr float kSurfelLightingApplyStrength = 0.35f;
    constexpr uint32_t kGridDimX = 32;
    constexpr uint32_t kGridDimY = 18;
    constexpr uint32_t kGridDimZ = 32;
    constexpr uint32_t kMaxEntriesPerCell = 64;
    constexpr float kCentralGridExtent = 24.0f;
    constexpr float kNearScale = 1.0f;
    constexpr float kFarScale = 120.0f;
    constexpr uint32_t kStatsReadbackInterval = 4;
    constexpr uint32_t kMaxDebugSurfelInstances = 16384;
    constexpr uint32_t kMinTileScanBudget = 32;
    constexpr uint32_t kMinSpawnCandidateBudget = 8;
    constexpr uint32_t kMinSpawnBudget = 2;
    constexpr uint32_t kMinLifecycleBudget = 1024;
    constexpr uint32_t kMinRecycleBudget = 256;
    constexpr uint32_t kMinProjectedBudget = 1024;
    constexpr uint32_t kMinIntegrateBudget = 512;
    constexpr uint32_t kMinIrradianceRayBudget = 256;
    constexpr uint32_t kMinRayTracedSurfels = 512;
    constexpr uint32_t kGuidingBinsPerSurfel = 36;
    constexpr uint32_t kRadialDepthBinsPerSurfel = 16;
    constexpr uint32_t kDefaultGridRebuildInterval = 1;

    static GLuint CreateBuffer(GLsizeiptr sizeBytes, const void* data = nullptr)
    {
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeBytes, data, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        return buffer;
    }

    static void ResizeBuffer(GLuint& buffer, GLsizeiptr sizeBytes, const void* data = nullptr)
    {
        if (!buffer) {
            buffer = CreateBuffer(sizeBytes, data);
            return;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeBytes, data, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    static void ResizeTexture2D(GLuint& texture,
        int width,
        int height,
        GLenum internalFormat,
        GLenum minFilter = GL_NEAREST,
        GLenum magFilter = GL_NEAREST)
    {
        if (!texture) {
            glGenTextures(1, &texture);
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
            0,
            static_cast<GLint>(internalFormat),
            std::max(width, 1),
            std::max(height, 1),
            0,
            internalFormat == GL_R32UI ? GL_RED_INTEGER : GL_RED,
            internalFormat == GL_R32UI ? GL_UNSIGNED_INT : GL_FLOAT,
            nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    static bool ShouldReadBackStats(uint32_t frameIndex)
    {
        return (frameIndex % kStatsReadbackInterval) == 0u;
    }

    static float LerpFloat(float a, float b, float t)
    {
        return a + (b - a) * t;
    }
}

SurfelGIPass::SurfelGIPass() = default;

SurfelGIPass::~SurfelGIPass()
{
    ReleaseBuffers();
    if (m_debugVAO) {
        glDeleteVertexArrays(1, &m_debugVAO);
        m_debugVAO = 0;
    }
    if (m_debugProgram) {
        glDeleteProgram(m_debugProgram);
        m_debugProgram = 0;
    }
    if (m_debugOverlayProgram) {
        glDeleteProgram(m_debugOverlayProgram);
        m_debugOverlayProgram = 0;
    }
}

bool SurfelGIPass::Initialize(RenderContext&)
{
    m_initShader = std::make_unique<ComputeShader>();
    m_lifecycleShader = std::make_unique<ComputeShader>();
    m_recycleShader = std::make_unique<ComputeShader>();
    m_tileClearShader = std::make_unique<ComputeShader>();
    m_tileCoverageShader = std::make_unique<ComputeShader>();
    m_tileSelectShader = std::make_unique<ComputeShader>();
    m_projectCoverageShader = std::make_unique<ComputeShader>();
    m_deficitShader = std::make_unique<ComputeShader>();
    m_gridClearShader = std::make_unique<ComputeShader>();
    m_gridBuildShader = std::make_unique<ComputeShader>();
    m_spawnShader = std::make_unique<ComputeShader>();
    m_integrateShader = std::make_unique<ComputeShader>();
    m_rayRequestShader = std::make_unique<ComputeShader>();
    m_rayAllocateShader = std::make_unique<ComputeShader>();
    m_rayTraceShader = std::make_unique<ComputeShader>();
    m_temporalAccumulateShader = std::make_unique<ComputeShader>();
    m_guidingUpdateShader = std::make_unique<ComputeShader>();
    m_neighbourShareShader = std::make_unique<ComputeShader>();
    m_radialDepthUpdateShader = std::make_unique<ComputeShader>();

    bool ok = true;
    ok &= m_initShader->CreateFromFile("shaders/surfel_gi_init_comp.glsl");
    ok &= m_lifecycleShader->CreateFromFile("shaders/surfel_gi_lifecycle_comp.glsl");
    ok &= m_recycleShader->CreateFromFile("shaders/surfel_gi_recycle_comp.glsl");
    ok &= m_tileClearShader->CreateFromFile("shaders/surfel_gi_tile_clear_comp.glsl");
    ok &= m_tileCoverageShader->CreateFromFile("shaders/surfel_gi_tile_coverage_comp.glsl");
    ok &= m_tileSelectShader->CreateFromFile("shaders/surfel_gi_tile_select_comp.glsl");
    ok &= m_projectCoverageShader->CreateFromFile("shaders/surfel_gi_coverage_project_comp.glsl");
    ok &= m_deficitShader->CreateFromFile("shaders/surfel_gi_deficit_comp.glsl");
    ok &= m_gridClearShader->CreateFromFile("shaders/surfel_gi_grid_clear_comp.glsl");
    ok &= m_gridBuildShader->CreateFromFile("shaders/surfel_gi_grid_build_comp.glsl");
    ok &= m_spawnShader->CreateFromFile("shaders/surfel_gi_spawn_comp.glsl");
    ok &= m_integrateShader->CreateFromFile("shaders/surfel_gi_integrate_comp.glsl");
    ok &= m_rayRequestShader->CreateFromFile("shaders/surfel_gi_ray_request_comp.glsl");
    ok &= m_rayAllocateShader->CreateFromFile("shaders/surfel_gi_ray_allocate_comp.glsl");
    ok &= m_rayTraceShader->CreateFromFile("shaders/surfel_gi_ray_trace_comp.glsl");
    ok &= m_temporalAccumulateShader->CreateFromFile("shaders/surfel_gi_temporal_accumulate_comp.glsl");
    ok &= m_guidingUpdateShader->CreateFromFile("shaders/surfel_gi_guiding_update_comp.glsl");
    ok &= m_neighbourShareShader->CreateFromFile("shaders/surfel_gi_neighbour_share_comp.glsl");
    ok &= m_radialDepthUpdateShader->CreateFromFile("shaders/surfel_gi_radial_depth_update_comp.glsl");

    m_debugProgram = CreateShaderProgram("shaders/surfel_gi_debug_vert.glsl", "shaders/surfel_gi_debug_frag.glsl");
    m_debugOverlayProgram = CreateShaderProgram("shaders/fullscreen_vert.glsl", "shaders/surfel_gi_debug_overlay_frag.glsl");
    ok &= (m_debugProgram != 0);
    ok &= (m_debugOverlayProgram != 0);
    glGenVertexArrays(1, &m_debugVAO);
    ok &= (m_debugVAO != 0);

    if (!ok) {
        std::cerr << "[SurfelGIPass] Failed to initialize shaders.\n";
        return false;
    }

    std::cout << "[SurfelGIPass] Initialized successfully.\n";
    return true;
}

void SurfelGIPass::Resize(RenderContext&, int, int)
{
    // A resolution change must not discard the world-space cache. EnsureResources
    // will resize screen-tile bookkeeping while leaving the fixed surfel pool alive.
}

void SurfelGIPass::ReleaseBuffers()
{
    const std::array<GLuint, 12> buffers = {
        m_surfelSSBO,
        m_headerSSBO,
        m_freeStackSSBO,
        m_recycleStackSSBO,
        m_tileCoverageSSBO,
        m_tileQueueSSBO,
        m_gridHeaderSSBO,
        m_gridEntrySSBO,
        m_irradianceHeaderSSBO,
        m_guidingBinsSSBO,
        m_radialDepthBinsSSBO,
        m_statsReadbackPBO
    };
    for (GLuint buffer : buffers) {
        if (buffer) {
            glDeleteBuffers(1, &buffer);
        }
    }
    m_surfelSSBO = 0;
    m_headerSSBO = 0;
    m_freeStackSSBO = 0;
    m_recycleStackSSBO = 0;
    m_tileCoverageSSBO = 0;
    m_tileQueueSSBO = 0;
    m_gridHeaderSSBO = 0;
    m_gridEntrySSBO = 0;
    m_irradianceHeaderSSBO = 0;
    m_guidingBinsSSBO = 0;
    m_radialDepthBinsSSBO = 0;
    m_statsReadbackPBO = 0;

    if (m_projectedCoverageTex) {
        glDeleteTextures(1, &m_projectedCoverageTex);
        m_projectedCoverageTex = 0;
    }
    if (m_rawProjectedSupportTex) {
        glDeleteTextures(1, &m_rawProjectedSupportTex);
        m_rawProjectedSupportTex = 0;
    }
    if (m_depthRejectTex) {
        glDeleteTextures(1, &m_depthRejectTex);
        m_depthRejectTex = 0;
    }
    if (m_normalRejectTex) {
        glDeleteTextures(1, &m_normalRejectTex);
        m_normalRejectTex = 0;
    }
    if (m_winnerIDTex) {
        glDeleteTextures(1, &m_winnerIDTex);
        m_winnerIDTex = 0;
    }
    if (m_coverageHistoryTex) {
        glDeleteTextures(1, &m_coverageHistoryTex);
        m_coverageHistoryTex = 0;
    }
    if (m_deficitTex) {
        glDeleteTextures(1, &m_deficitTex);
        m_deficitTex = 0;
    }
    m_coverageWidth = 0;
    m_coverageHeight = 0;
}

void SurfelGIPass::EnsureResources(RenderContext& ctx)
{
    const uint32_t tileSize = kSurfelTileSize;
    const uint32_t coverageWidth = static_cast<uint32_t>(std::max(ctx.width, 1));
    const uint32_t coverageHeight = static_cast<uint32_t>(std::max(ctx.height, 1));
    const uint32_t tileCountX = (static_cast<uint32_t>(std::max(ctx.width, 1)) + tileSize - 1u) / tileSize;
    const uint32_t tileCountY = (static_cast<uint32_t>(std::max(ctx.height, 1)) + tileSize - 1u) / tileSize;
    const uint32_t gridCellCount = kGridDimX * kGridDimY * kGridDimZ;

    const bool tilingChanged =
        tileCountX != m_allocatedTileCountX ||
        tileCountY != m_allocatedTileCountY;
    const bool gridChanged = gridCellCount != m_allocatedGridCellCount;
    const bool coverageResolutionChanged =
        coverageWidth != m_coverageWidth ||
        coverageHeight != m_coverageHeight ||
        m_projectedCoverageTex == 0 ||
        m_rawProjectedSupportTex == 0 ||
        m_depthRejectTex == 0 ||
        m_normalRejectTex == 0 ||
        m_winnerIDTex == 0 ||
        m_coverageHistoryTex == 0 ||
        m_deficitTex == 0;
    const bool firstAllocation =
        m_headerSSBO == 0 ||
        m_surfelSSBO == 0 ||
        m_freeStackSSBO == 0 ||
        m_irradianceHeaderSSBO == 0 ||
        m_guidingBinsSSBO == 0 ||
        m_radialDepthBinsSSBO == 0;

    if (!tilingChanged && !gridChanged && !firstAllocation && !coverageResolutionChanged) {
        return;
    }

    m_allocatedTileCountX = tileCountX;
    m_allocatedTileCountY = tileCountY;
    m_allocatedGridCellCount = gridCellCount;
    m_coverageWidth = coverageWidth;
    m_coverageHeight = coverageHeight;

    const GpuPoolHeader header{
        glm::uvec4(kMaxSurfels, 0u, kMaxSurfels, m_frameIndex),
        glm::uvec4(0u),
        glm::uvec4(tileCountX, tileCountY, tileSize, gridCellCount),
        glm::uvec4(kGridDimX, kGridDimY, kGridDimZ, kMaxEntriesPerCell),
        glm::vec4(kCentralGridExtent, kNearScale, kFarScale, (kCentralGridExtent * 2.0f) / float(kGridDimX)),
        glm::uvec4(0u),
        glm::uvec4(0u),
        glm::uvec4(0u),
        glm::uvec4(0u),
        glm::uvec4(0u),
        glm::uvec4(0u),
        glm::uvec4(0u),
        glm::uvec4(0u)
    };

    if (firstAllocation) {
        ResizeBuffer(m_headerSSBO, sizeof(GpuPoolHeader), &header);
        ResizeBuffer(m_surfelSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(GpuSurfelRecord)));
        ResizeBuffer(m_freeStackSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(uint32_t)));
        ResizeBuffer(m_recycleStackSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(uint32_t)));
        ResizeBuffer(m_irradianceHeaderSSBO, sizeof(GpuIrradianceHeader));
        ResizeBuffer(m_guidingBinsSSBO, static_cast<GLsizeiptr>(kMaxSurfels * kGuidingBinsPerSurfel * sizeof(float)));
        ResizeBuffer(m_radialDepthBinsSSBO, static_cast<GLsizeiptr>(kMaxSurfels * kRadialDepthBinsPerSurfel * sizeof(glm::vec4)));
        ResizeBuffer(m_statsReadbackPBO, sizeof(GpuPoolHeader));
        m_needsPoolInit = true;
    } else {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_headerSSBO);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
            static_cast<GLintptr>(offsetof(GpuPoolHeader, tiling)),
            sizeof(glm::uvec4),
            &header.tiling);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
            static_cast<GLintptr>(offsetof(GpuPoolHeader, gridDims)),
            sizeof(glm::uvec4),
            &header.gridDims);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
            static_cast<GLintptr>(offsetof(GpuPoolHeader, gridParams)),
            sizeof(glm::vec4),
            &header.gridParams);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    if (tilingChanged || !m_tileCoverageSSBO) {
        std::vector<GpuTileMeta> zeroTiles(static_cast<size_t>(tileCountX) * tileCountY, GpuTileMeta{});
        ResizeBuffer(m_tileCoverageSSBO,
            static_cast<GLsizeiptr>(zeroTiles.size() * sizeof(GpuTileMeta)),
            zeroTiles.empty() ? nullptr : zeroTiles.data());
        m_tileScanCursor = 0;
    }
    if (tilingChanged || !m_tileQueueSSBO) {
        const uint32_t totalTiles = std::max(tileCountX * tileCountY, 1u);
        const uint32_t maxTilesPerBucket = totalTiles;
        const GLsizeiptr queueBytes =
            static_cast<GLsizeiptr>(sizeof(GpuTileQueueHeader)) +
            static_cast<GLsizeiptr>(kTileQueueBucketCount * maxTilesPerBucket * sizeof(uint32_t));
        ResizeBuffer(m_tileQueueSSBO, queueBytes);
        ResetTileQueue(maxTilesPerBucket, RuntimeBudget{});
    }
    if (gridChanged || !m_gridHeaderSSBO || !m_gridEntrySSBO) {
        ResizeBuffer(m_gridHeaderSSBO, static_cast<GLsizeiptr>(gridCellCount * sizeof(glm::uvec4)));
        ResizeBuffer(m_gridEntrySSBO, static_cast<GLsizeiptr>(gridCellCount * kMaxEntriesPerCell * sizeof(uint32_t)));
    }

    if (coverageResolutionChanged) {
        ResizeTexture2D(
            m_rawProjectedSupportTex,
            static_cast<int>(coverageWidth),
            static_cast<int>(coverageHeight),
            GL_R32UI);
        ResizeTexture2D(
            m_projectedCoverageTex,
            static_cast<int>(coverageWidth),
            static_cast<int>(coverageHeight),
            GL_R32UI);
        ResizeTexture2D(
            m_depthRejectTex,
            static_cast<int>(coverageWidth),
            static_cast<int>(coverageHeight),
            GL_R32UI);
        ResizeTexture2D(
            m_normalRejectTex,
            static_cast<int>(coverageWidth),
            static_cast<int>(coverageHeight),
            GL_R32UI);
        ResizeTexture2D(
            m_winnerIDTex,
            static_cast<int>(coverageWidth),
            static_cast<int>(coverageHeight),
            GL_R32UI);
        ResizeTexture2D(
            m_coverageHistoryTex,
            static_cast<int>(coverageWidth),
            static_cast<int>(coverageHeight),
            GL_R32F,
            GL_LINEAR,
            GL_LINEAR);
        ResizeTexture2D(
            m_deficitTex,
            static_cast<int>(coverageWidth),
            static_cast<int>(coverageHeight),
            GL_R32F,
            GL_LINEAR,
            GL_LINEAR);

        const float zeroFloat = 0.0f;
        glClearTexImage(m_coverageHistoryTex, 0, GL_RED, GL_FLOAT, &zeroFloat);
    }
}

void SurfelGIPass::BindCommonBuffers(GLuint transformBuffer) const
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSurfels, m_surfelSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingHeader, m_headerSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingFreeStack, m_freeStackSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingRecycleStack, m_recycleStackSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingTileCoverage, m_tileCoverageSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingGridHeaders, m_gridHeaderSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingGridEntries, m_gridEntrySSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingTileQueue, m_tileQueueSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingIrradianceHeader, m_irradianceHeaderSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingGuidingBins, m_guidingBinsSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingRadialDepthBins, m_radialDepthBinsSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingTransforms, transformBuffer);
}

void SurfelGIPass::InitializePool()
{
    if (!m_initShader || !m_initShader->IsValid() || !m_surfelSSBO || !m_headerSSBO) {
        return;
    }

    glUseProgram(m_initShader->GetProgramID());
    BindCommonBuffers(0);
    m_initShader->SetUniform("uTileCount", glm::vec2(float(m_allocatedTileCountX), float(m_allocatedTileCountY)));
    m_initShader->SetUniform("uGridCellCount", static_cast<int>(m_allocatedGridCellCount));
    m_initShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_initShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
    glUseProgram(0);
    m_tileScanCursor = 0;
    m_lifecycleCursor = 0;
    m_recycleCursor = 0;
    m_projectCursor = 0;
    m_integrationCursor = 0;
    m_rayCursor = 0;
    m_sharingCursor = 0;
    m_coarseCoverageCursor = 0;
    m_gridRebuildCountdown = 0;
    m_dynamicBudgetScale = 1.0f;
    m_needsPoolInit = false;

    const float zeroFloat = 0.0f;
    const glm::vec4 zeroVec(0.0f);
    GpuIrradianceHeader zeroHeader{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_irradianceHeaderSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GpuIrradianceHeader), &zeroHeader);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_guidingBinsSSBO);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32F, GL_RED, GL_FLOAT, &zeroFloat);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_radialDepthBinsSSBO);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_RGBA32F, GL_RGBA, GL_FLOAT, &zeroVec);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void SurfelGIPass::ResetFrameStats(uint32_t frameIndex)
{
    const glm::uvec4 zeroStats(0u);
    const glm::uvec4 runtimeState(
        m_tileScanCursor,
        m_lifecycleCursor,
        m_recycleCursor,
        m_projectCursor);
    const glm::uvec4 queueState(
        0u,
        0u,
        std::max(m_lastStats.gridRebuildInterval, 1u),
        m_gridRebuildCountdown);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_headerSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, counts) + sizeof(uint32_t) * 3u),
        sizeof(uint32_t),
        &frameIndex);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, frameStats)),
        sizeof(glm::uvec4),
        &zeroStats);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, coverageStats)),
        sizeof(glm::uvec4),
        &zeroStats);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, contributionStats)),
        sizeof(glm::uvec4),
        &zeroStats);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, recycleStats)),
        sizeof(glm::uvec4),
        &zeroStats);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, coverageMetricStats)),
        sizeof(glm::uvec4),
        &zeroStats);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, tileWorkStats)),
        sizeof(glm::uvec4),
        &zeroStats);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, budgetStats)),
        sizeof(glm::uvec4),
        &zeroStats);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, runtimeState)),
        sizeof(glm::uvec4),
        &runtimeState);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, queueStats)),
        sizeof(glm::uvec4),
        &queueState);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

SurfelGIPass::RuntimeBudget SurfelGIPass::ComputeRuntimeBudget(const RenderContext& ctx) const
{
    const uint32_t totalTiles = std::max(m_allocatedTileCountX * m_allocatedTileCountY, 1u);
    const float userScale = std::clamp(ctx.surfelGIBudgetScale, 0.25f, 2.0f);
    const float effectiveScale = std::clamp(m_dynamicBudgetScale * userScale, 0.20f, 2.25f);
    const auto scaledBudget = [effectiveScale](int baseValue, uint32_t minValue, uint32_t maxValue) {
        const float scaled = static_cast<float>(std::max(baseValue, 0)) * effectiveScale;
        const uint32_t rounded = static_cast<uint32_t>(std::max(0.0f, std::round(scaled)));
        return std::clamp(rounded, minValue, maxValue);
    };

    RuntimeBudget budget{};
    budget.maxTilesToScan = scaledBudget(ctx.surfelGIMaxTilesScanned, kMinTileScanBudget, totalTiles);
    budget.maxSpawnCandidates = scaledBudget(
        ctx.surfelGIMaxSpawnCandidates,
        kMinSpawnCandidateBudget,
        std::max(budget.maxTilesToScan, 1u));
    budget.maxSurfelsToSpawn = scaledBudget(
        ctx.surfelGIMaxSpawns,
        kMinSpawnBudget,
        std::max(budget.maxSpawnCandidates, 1u));
    budget.maxRecycleDecisions = scaledBudget(
        ctx.surfelGIMaxRecycleDecisions,
        kMinRecycleBudget,
        kMaxSurfels);
    budget.maxProjectedSurfels = scaledBudget(
        ctx.surfelGIMaxProjectedSurfels,
        kMinProjectedBudget,
        kMaxSurfels);
    budget.maxLifecycleUpdates = scaledBudget(
        ctx.surfelGIMaxLifecycleUpdates,
        kMinLifecycleBudget,
        kMaxSurfels);
    budget.maxIntegrationUpdates = scaledBudget(
        ctx.surfelGIMaxIntegrationUpdates,
        kMinIntegrateBudget,
        kMaxSurfels);
    budget.maxCoarseCoverageSurfels = scaledBudget(
        ctx.surfelGIMaxCoarseCoverageSurfels,
        kMinProjectedBudget,
        kMaxSurfels);
    {
        const uint32_t userRayCap = static_cast<uint32_t>(std::max(ctx.surfelGIMaxIrradianceRays, 0));
        const float scaled = static_cast<float>(userRayCap) * effectiveScale;
        const uint32_t rounded = static_cast<uint32_t>(std::max(0.0f, std::round(scaled)));
        budget.maxIrradianceRays = std::min(rounded, userRayCap);
    }
    budget.maxRayTracedSurfels = scaledBudget(
        ctx.surfelGIMaxRayTracedSurfels,
        kMinRayTracedSurfels,
        kMaxSurfels);

    const float inverseScale = std::clamp(1.0f / std::max(effectiveScale, 0.20f), 0.5f, 4.0f);
    const uint32_t rebuildInterval = std::clamp(
        static_cast<uint32_t>(std::max(1.0f, std::round(static_cast<float>(ctx.surfelGIGridRebuildInterval) * inverseScale))),
        1u,
        8u);
    budget.gridRebuildInterval = std::max(rebuildInterval, kDefaultGridRebuildInterval);
    return budget;
}

void SurfelGIPass::UpdateDynamicBudgetScale(const RenderContext& ctx)
{
    const float targetMs = std::max(ctx.surfelGIFrameBudgetMs, 0.5f);
    m_lastStats.targetBudgetMs = targetMs;
    if (!ctx.surfelGIAdaptiveBudget) {
        m_dynamicBudgetScale = 1.0f;
        return;
    }

    const float elapsed = std::max(m_lastStats.totalTimeMs, 0.0f);
    if (elapsed > targetMs) {
        const float overshoot = std::clamp((elapsed - targetMs) / std::max(targetMs, 0.001f), 0.0f, 2.0f);
        m_dynamicBudgetScale *= std::max(0.55f, 0.90f - overshoot * 0.22f);
    } else if (elapsed < targetMs * 0.72f) {
        const float slack = std::clamp((targetMs * 0.72f - elapsed) / std::max(targetMs, 0.001f), 0.0f, 1.0f);
        m_dynamicBudgetScale *= (1.02f + slack * 0.06f);
    }

    m_dynamicBudgetScale = std::clamp(m_dynamicBudgetScale, 0.25f, 1.50f);
}

void SurfelGIPass::ResetTileQueue(uint32_t maxTilesPerBucket, const RuntimeBudget& budget)
{
    if (!m_tileQueueSSBO) {
        return;
    }

    GpuTileQueueHeader queueHeader{};
    queueHeader.config = glm::uvec4(
        std::max(maxTilesPerBucket, 1u),
        std::max(m_allocatedTileCountX * m_allocatedTileCountY, 1u),
        std::max(budget.maxSpawnCandidates, 1u),
        std::max(budget.maxSurfelsToSpawn, 1u));

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_tileQueueSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GpuTileQueueHeader), &queueHeader);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void SurfelGIPass::RunPersistentStateUpdate(RenderContext& ctx,
    RenderSystem& renderSystem,
    const std::shared_ptr<Camera>& camera,
    const RuntimeBudget& budget)
{
    if (!m_lifecycleShader || !m_lifecycleShader->IsValid() || !camera) {
        return;
    }

    const uint32_t surfelCount = std::min(budget.maxLifecycleUpdates, kMaxSurfels);
    if (surfelCount == 0u) {
        return;
    }

    const glm::vec3 cameraPos = camera->GetCameraPosition();

    glUseProgram(m_lifecycleShader->GetProgramID());
    m_lifecycleShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_lifecycleShader->SetUniform("uMaxTransformID", static_cast<int>(renderSystem.GetTransformRecordCount()));
    m_lifecycleShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
    m_lifecycleShader->SetUniform("uView", ctx.view);
    m_lifecycleShader->SetUniform("uProjection", ctx.proj);
    m_lifecycleShader->SetUniform("uCameraPos", cameraPos);
    m_lifecycleShader->SetUniform("uTargetRadiusPixels", ctx.surfelGITargetRadiusPixels);
    m_lifecycleShader->SetUniform("uCoverageThreshold", ctx.surfelGICoverageThreshold);
    m_lifecycleShader->SetUniform("uRecyclePressure", ctx.surfelGIRecyclePressure);
    m_lifecycleShader->SetUniform("uFreePoolReserveFraction", kFreePoolReserveFraction);
    m_lifecycleShader->SetUniform("uSurfelStart", static_cast<int>(m_lifecycleCursor));
    m_lifecycleShader->SetUniform("uSurfelCount", static_cast<int>(surfelCount));
    m_lifecycleShader->Dispatch(ComputeShader::CalculateWorkGroups(surfelCount, 256u), 1u, 1u);
    m_lifecycleShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);

    m_lastStats.lifecycleSurfelsProcessed = surfelCount;
    m_lifecycleCursor = (m_lifecycleCursor + surfelCount) % kMaxSurfels;
}

void SurfelGIPass::RunRecycleDecision(RenderContext& ctx, const RuntimeBudget& budget)
{
    if (!m_recycleShader || !m_recycleShader->IsValid()) {
        return;
    }

    const uint32_t recycleCount = std::min(budget.maxRecycleDecisions, kMaxSurfels);
    if (recycleCount == 0u) {
        return;
    }

    const uint32_t tileCount = std::max(m_lastStats.tileCountX * m_lastStats.tileCountY,
        m_allocatedTileCountX * m_allocatedTileCountY);
    const float normalizedTiles = std::max(float(tileCount), 1.0f);
    const float attemptRatio = float(m_lastStats.spawnAttemptsThisFrame) / normalizedTiles;
    const float rejectedRatio = float(m_lastStats.probabilityRejectedSpawns) / normalizedTiles;
    const float freeFraction = float(m_lastStats.freeCount) / float(kMaxSurfels);
    const float lowFreePressure = std::clamp(
        (kFreePoolReserveFraction * 1.5f - freeFraction) / std::max(kFreePoolReserveFraction * 1.5f, 0.001f),
        0.0f,
        1.0f);
    const float attemptPressure = std::clamp((attemptRatio - 0.002f) / 0.045f, 0.0f, 1.0f);
    const float rejectedPressure = std::clamp((rejectedRatio - 0.001f) / 0.030f, 0.0f, 1.0f);
    const float exhaustedPressure = m_lastStats.freeStackExhaustedSpawns > 0u ? 1.0f : 0.0f;
    const float coverageDemandPressure = std::clamp(
        attemptPressure * 0.80f + rejectedPressure * 0.55f + lowFreePressure * 0.35f + exhaustedPressure,
        0.0f,
        1.0f);

    glUseProgram(m_recycleShader->GetProgramID());
    m_recycleShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_recycleShader->SetUniform("uFreePoolReserveFraction", kFreePoolReserveFraction);
    m_recycleShader->SetUniform("uRecyclePressure", ctx.surfelGIRecyclePressure);
    m_recycleShader->SetUniform("uCoverageDemandPressure", coverageDemandPressure);
    m_recycleShader->SetUniform("uSurfelStart", static_cast<int>(m_recycleCursor));
    m_recycleShader->SetUniform("uSurfelCount", static_cast<int>(recycleCount));
    m_recycleShader->SetUniform("uMaxRecycleDecisions", static_cast<int>(recycleCount));
    m_recycleShader->Dispatch(ComputeShader::CalculateWorkGroups(recycleCount, 256u), 1u, 1u);
    m_recycleShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);

    m_lastStats.recycleSurfelsProcessed = recycleCount;
    m_recycleCursor = (m_recycleCursor + recycleCount) % kMaxSurfels;
}

void SurfelGIPass::RebuildSpatialGrid(const glm::vec3& cameraPos)
{
    if (!m_gridClearShader || !m_gridBuildShader ||
        !m_gridClearShader->IsValid() || !m_gridBuildShader->IsValid()) {
        return;
    }

    glUseProgram(m_gridClearShader->GetProgramID());
    m_gridClearShader->SetUniform("uGridCellCount", static_cast<int>(m_allocatedGridCellCount));
    m_gridClearShader->Dispatch(ComputeShader::CalculateWorkGroups(m_allocatedGridCellCount, 256u), 1u, 1u);
    m_gridClearShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(m_gridBuildShader->GetProgramID());
    m_gridBuildShader->SetUniform("uCameraPos", cameraPos);
    m_gridBuildShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_gridBuildShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunPersistentTileCoverage(RenderContext& ctx, const RuntimeBudget& budget, float cameraMotion)
{
    if (!m_tileCoverageShader || !m_tileCoverageShader->IsValid()) {
        return;
    }

    using Clock = std::chrono::high_resolution_clock;
    const uint32_t tileCount = std::max(m_allocatedTileCountX * m_allocatedTileCountY, 1u);

    if (m_tileClearShader && m_tileClearShader->IsValid()) {
        glUseProgram(m_tileClearShader->GetProgramID());
        m_tileClearShader->SetUniform("uTileCount", static_cast<int>(tileCount));
        m_tileClearShader->Dispatch(ComputeShader::CalculateWorkGroups(tileCount, 256u), 1u, 1u);
        m_tileClearShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
    }

    const uint32_t coarseCount = std::min(budget.maxCoarseCoverageSurfels, kMaxSurfels);
    if (coarseCount > 0u) {
        const auto coarseBegin = Clock::now();
        glUseProgram(m_tileCoverageShader->GetProgramID());
        m_tileCoverageShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
        m_tileCoverageShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
        m_tileCoverageShader->SetUniform("uView", ctx.view);
        m_tileCoverageShader->SetUniform("uProjection", ctx.proj);
        m_tileCoverageShader->SetUniform("uCoverageThreshold", ctx.surfelGICoverageThreshold);
        m_tileCoverageShader->SetUniform("uSurfelStart", static_cast<int>(m_coarseCoverageCursor));
        m_tileCoverageShader->SetUniform("uSurfelCount", static_cast<int>(coarseCount));
        m_tileCoverageShader->Dispatch(ComputeShader::CalculateWorkGroups(coarseCount, 256u), 1u, 1u);
        m_tileCoverageShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
        m_lastStats.coarseCoverageTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - coarseBegin).count();
        m_coarseCoverageCursor = (m_coarseCoverageCursor + coarseCount) % kMaxSurfels;
    } else {
        m_lastStats.coarseCoverageTimeMs = 0.0f;
    }

    const glm::mat4 invViewProj = glm::inverse(ctx.proj * ctx.view);
    auto exactBegin = Clock::now();
    RunProjectedCoverage(ctx, invViewProj, m_projectedCoverageMaxTransformID, budget);
    m_lastStats.exactCoverageTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - exactBegin).count();
    auto deficitBegin = Clock::now();
    RunCoverageDeficit(ctx, budget, cameraMotion);
    m_lastStats.deficitTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - deficitBegin).count();

    auto selectionBegin = Clock::now();
    RunTileSelection(ctx, budget, cameraMotion);
    m_lastStats.tileSelectTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - selectionBegin).count();
}

void SurfelGIPass::RunProjectedCoverage(
    RenderContext& ctx,
    const glm::mat4& invViewProj,
    uint32_t maxTransformID,
    const RuntimeBudget& budget)
{
    if (!m_projectCoverageShader || !m_projectCoverageShader->IsValid() ||
        !m_rawProjectedSupportTex || !m_projectedCoverageTex ||
        !m_depthRejectTex || !m_normalRejectTex || !m_winnerIDTex || !ctx.gbufferFBO) {
        return;
    }

    const GLuint clearValue = 0u;
    const GLuint winnerClearValue = 0xffffffffu;
    glClearTexImage(m_rawProjectedSupportTex, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &clearValue);
    glClearTexImage(m_projectedCoverageTex, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &clearValue);
    glClearTexImage(m_depthRejectTex, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &clearValue);
    glClearTexImage(m_normalRejectTex, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &clearValue);
    glClearTexImage(m_winnerIDTex, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &winnerClearValue);

    const uint32_t projectedCount = std::min(budget.maxProjectedSurfels, kMaxSurfels);
    if (projectedCount == 0u) {
        return;
    }

    glUseProgram(m_projectCoverageShader->GetProgramID());
    glBindImageTexture(
        kImageRawProjectedSupport,
        m_rawProjectedSupportTex,
        0,
        GL_FALSE,
        0,
        GL_READ_WRITE,
        GL_R32UI);
    glBindImageTexture(
        kImageProjectedCoverage,
        m_projectedCoverageTex,
        0,
        GL_FALSE,
        0,
        GL_READ_WRITE,
        GL_R32UI);
    glBindImageTexture(
        kImageDepthReject,
        m_depthRejectTex,
        0,
        GL_FALSE,
        0,
        GL_READ_WRITE,
        GL_R32UI);
    glBindImageTexture(
        kImageNormalReject,
        m_normalRejectTex,
        0,
        GL_FALSE,
        0,
        GL_READ_WRITE,
        GL_R32UI);
    glBindImageTexture(
        kImageWinnerID,
        m_winnerIDTex,
        0,
        GL_FALSE,
        0,
        GL_READ_WRITE,
        GL_R32UI);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    if (GLint loc = glGetUniformLocation(m_projectCoverageShader->GetProgramID(), "uPackedNormalRM"); loc >= 0) {
        glUniform1i(loc, 0);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(5));
    if (GLint loc = glGetUniformLocation(m_projectCoverageShader->GetProgramID(), "uTransformIDTex"); loc >= 0) {
        glUniform1i(loc, 1);
    }

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    if (GLint loc = glGetUniformLocation(m_projectCoverageShader->GetProgramID(), "uDepthTex"); loc >= 0) {
        glUniform1i(loc, 2);
    }

    m_projectCoverageShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_projectCoverageShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
    m_projectCoverageShader->SetUniform("uView", ctx.view);
    m_projectCoverageShader->SetUniform("uProjection", ctx.proj);
    m_projectCoverageShader->SetUniform("uInvViewProj", invViewProj);
    m_projectCoverageShader->SetUniform("uNormalReject", ctx.surfelGINormalReject);
    m_projectCoverageShader->SetUniform("uDepthThicknessScale", 0.75f);
    m_projectCoverageShader->SetUniform("uMaxTransformID", static_cast<int>(maxTransformID));
    m_projectCoverageShader->SetUniform("uSurfelStart", static_cast<int>(m_projectCursor));
    m_projectCoverageShader->SetUniform("uSurfelCount", static_cast<int>(projectedCount));

    m_projectCoverageShader->Dispatch(ComputeShader::CalculateWorkGroups(projectedCount, 64u), 1u, 1u);
    m_projectCoverageShader->WaitForCompletion(
        GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
        GL_SHADER_STORAGE_BARRIER_BIT |
        GL_TEXTURE_FETCH_BARRIER_BIT);

    m_lastStats.projectedSurfelsProcessed = projectedCount;
    m_projectCursor = (m_projectCursor + projectedCount) % kMaxSurfels;
}

void SurfelGIPass::RunCoverageDeficit(RenderContext& ctx, const RuntimeBudget& budget, float cameraMotion)
{
    if (!m_deficitShader || !m_deficitShader->IsValid() ||
        !m_projectedCoverageTex || !m_coverageHistoryTex || !m_deficitTex || !ctx.gbufferFBO) {
        return;
    }

    glUseProgram(m_deficitShader->GetProgramID());
    glBindImageTexture(
        kImageProjectedCoverage,
        m_projectedCoverageTex,
        0,
        GL_FALSE,
        0,
        GL_READ_ONLY,
        GL_R32UI);
    glBindImageTexture(
        kImageDeficit,
        m_deficitTex,
        0,
        GL_FALSE,
        0,
        GL_WRITE_ONLY,
        GL_R32F);
    glBindImageTexture(
        kImageCoverageHistory,
        m_coverageHistoryTex,
        0,
        GL_FALSE,
        0,
        GL_READ_WRITE,
        GL_R32F);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    if (GLint loc = glGetUniformLocation(m_deficitShader->GetProgramID(), "uDepthTex"); loc >= 0) {
        glUniform1i(loc, 0);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(5));
    if (GLint loc = glGetUniformLocation(m_deficitShader->GetProgramID(), "uTransformIDTex"); loc >= 0) {
        glUniform1i(loc, 1);
    }

    m_deficitShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
    m_deficitShader->SetUniform("uCoverageThreshold", ctx.surfelGICoverageThreshold);
    const float motionPressure = std::clamp((cameraMotion - 0.015f) / 0.22f, 0.0f, 1.0f);
    m_deficitShader->SetUniform("uCoverageHistoryHysteresis", LerpFloat(0.92f, 0.35f, motionPressure));
    const uint32_t totalTiles = std::max(m_allocatedTileCountX * m_allocatedTileCountY, 1u);
    const float tileFraction = float(std::max(budget.maxTilesToScan, 1u)) / float(totalTiles);
    uint32_t pixelUpdateModulo = 1u;
    if (motionPressure > 0.10f) {
        pixelUpdateModulo = 1u;
    } else if (tileFraction < 0.35f) {
        pixelUpdateModulo = 4u;
    } else if (tileFraction < 0.70f) {
        pixelUpdateModulo = 2u;
    }
    m_deficitShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_deficitShader->SetUniform("uPixelUpdateModulo", static_cast<int>(pixelUpdateModulo));
    m_deficitShader->SetUniform("uPixelUpdatePhase", static_cast<int>(m_frameIndex % pixelUpdateModulo));

    const GLuint groupsX = ComputeShader::CalculateWorkGroups(static_cast<GLuint>(std::max(ctx.width, 1)), 16u);
    const GLuint groupsY = ComputeShader::CalculateWorkGroups(static_cast<GLuint>(std::max(ctx.height, 1)), 16u);
    m_deficitShader->Dispatch(groupsX, groupsY, 1u);
    m_deficitShader->WaitForCompletion(
        GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
        GL_TEXTURE_FETCH_BARRIER_BIT |
        GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunTileSelection(RenderContext& ctx, const RuntimeBudget& budget, float cameraMotion)
{
    if (!m_tileSelectShader || !m_tileSelectShader->IsValid() || !ctx.gbufferFBO || !m_tileQueueSSBO) {
        return;
    }

    const uint32_t totalTiles = std::max(m_allocatedTileCountX * m_allocatedTileCountY, 1u);
    const uint32_t scanCount = std::min(std::max(budget.maxTilesToScan, 1u), totalTiles);
    if (scanCount == 0u) {
        return;
    }

    ResetTileQueue(totalTiles, budget);

    glUseProgram(m_tileSelectShader->GetProgramID());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    if (GLint loc = glGetUniformLocation(m_tileSelectShader->GetProgramID(), "uDepthTex"); loc >= 0) {
        glUniform1i(loc, 0);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    if (GLint loc = glGetUniformLocation(m_tileSelectShader->GetProgramID(), "uPackedNormalRM"); loc >= 0) {
        glUniform1i(loc, 1);
    }

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(5));
    if (GLint loc = glGetUniformLocation(m_tileSelectShader->GetProgramID(), "uTransformIDTex"); loc >= 0) {
        glUniform1i(loc, 2);
    }

    glBindImageTexture(kImageDeficit, m_deficitTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);

    m_tileSelectShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_tileSelectShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
    m_tileSelectShader->SetUniform("uCoverageThreshold", ctx.surfelGICoverageThreshold);
    m_tileSelectShader->SetUniform("uTileScanCursor", static_cast<int>(m_tileScanCursor));
    m_tileSelectShader->SetUniform("uMaxTilesToScan", static_cast<int>(scanCount));
    m_tileSelectShader->SetUniform("uCameraMotion", cameraMotion);
    m_tileSelectShader->SetUniform("uQualityScale", m_dynamicBudgetScale);

    m_tileSelectShader->Dispatch(scanCount, 1u, 1u);
    m_tileSelectShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    m_tileScanCursor = (m_tileScanCursor + scanCount) % totalTiles;
}

void SurfelGIPass::RunCoverageGapFill(RenderContext& ctx,
    RenderSystem& renderSystem,
    const std::shared_ptr<Camera>& camera,
    const glm::mat4& invViewProj,
    const glm::mat4& invView,
    const RuntimeBudget& budget)
{
    if (!m_spawnShader || !m_spawnShader->IsValid() || !camera) {
        return;
    }

    const uint32_t candidateBudget = std::min(
        std::max(budget.maxSpawnCandidates, 1u),
        std::max(m_allocatedTileCountX * m_allocatedTileCountY, 1u));
    if (candidateBudget == 0u) {
        return;
    }

    const glm::vec3 cameraPos = camera->GetCameraPosition();

    const auto bindGBufferTextures = [&ctx](GLuint program) {
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
        if (GLint loc = glGetUniformLocation(program, "uPackedNormalRM"); loc >= 0) glUniform1i(loc, 0);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(5));
        if (GLint loc = glGetUniformLocation(program, "uTransformIDTex"); loc >= 0) glUniform1i(loc, 1);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
        if (GLint loc = glGetUniformLocation(program, "uDepthTex"); loc >= 0) glUniform1i(loc, 2);
    };

    bindGBufferTextures(m_spawnShader->GetProgramID());
    glBindImageTexture(
        kImageDeficit,
        m_deficitTex,
        0,
        GL_FALSE,
        0,
        GL_READ_ONLY,
        GL_R32F);
    m_spawnShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_spawnShader->SetUniform("uMaxTransformID", static_cast<int>(renderSystem.GetTransformRecordCount()));
    m_spawnShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
    m_spawnShader->SetUniform("uView", ctx.view);
    m_spawnShader->SetUniform("uProjection", ctx.proj);
    m_spawnShader->SetUniform("uInvViewProj", invViewProj);
    m_spawnShader->SetUniform("uInvView", invView);
    m_spawnShader->SetUniform("uTargetRadiusPixels", ctx.surfelGITargetRadiusPixels);
    m_spawnShader->SetUniform("uCoverageThreshold", ctx.surfelGICoverageThreshold);
    m_spawnShader->SetUniform("uNormalReject", ctx.surfelGINormalReject);
    m_spawnShader->SetUniform("uCameraPos", cameraPos);
    m_spawnShader->SetUniform("uFreePoolReserveFraction", kFreePoolReserveFraction);
    m_spawnShader->SetUniform("uMaxSpawnCandidates", static_cast<int>(candidateBudget));
    m_spawnShader->SetUniform("uMaxSpawns", static_cast<int>(std::max(budget.maxSurfelsToSpawn, 1u)));
    m_spawnShader->Dispatch(ComputeShader::CalculateWorkGroups(candidateBudget, 64u), 1u, 1u);
    m_spawnShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void SurfelGIPass::RunIrradianceIntegration(
    RenderContext& ctx,
    const std::shared_ptr<DirectionalLight>& dirLight,
    const RuntimeBudget& budget)
{
    if (!m_integrateShader || !m_integrateShader->IsValid()) {
        return;
    }
    const uint32_t integrationCount = std::min(budget.maxIntegrationUpdates, kMaxSurfels);
    if (integrationCount == 0u) {
        return;
    }

    const glm::vec3 lightDirection = dirLight ? dirLight->GetLightDirection() : glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 lightColor = dirLight && dirLight->IsEnabled() ? dirLight->GetEffectiveColor() : glm::vec3(0.0f);

    glUseProgram(m_integrateShader->GetProgramID());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    if (GLint loc = glGetUniformLocation(m_integrateShader->GetProgramID(), "uPackedNormalRM"); loc >= 0) glUniform1i(loc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(1));
    if (GLint loc = glGetUniformLocation(m_integrateShader->GetProgramID(), "uAlbedoAO"); loc >= 0) glUniform1i(loc, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(4));
    if (GLint loc = glGetUniformLocation(m_integrateShader->GetProgramID(), "uEmissive"); loc >= 0) glUniform1i(loc, 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(5));
    if (GLint loc = glGetUniformLocation(m_integrateShader->GetProgramID(), "uTransformIDTex"); loc >= 0) glUniform1i(loc, 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    if (GLint loc = glGetUniformLocation(m_integrateShader->GetProgramID(), "uDepthTex"); loc >= 0) glUniform1i(loc, 4);

    m_integrateShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_integrateShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
    m_integrateShader->SetUniform("uView", ctx.view);
    m_integrateShader->SetUniform("uProjection", ctx.proj);
    m_integrateShader->SetUniform("uInvViewProj", glm::inverse(ctx.proj * ctx.view));
    m_integrateShader->SetUniform("uDirectionalLightDir", lightDirection);
    m_integrateShader->SetUniform("uDirectionalLightColor", lightColor);
    m_integrateShader->SetUniform("uSurfelStart", static_cast<int>(m_integrationCursor));
    m_integrateShader->SetUniform("uSurfelCount", static_cast<int>(integrationCount));
    m_integrateShader->Dispatch(ComputeShader::CalculateWorkGroups(integrationCount, 256u), 1u, 1u);
    m_integrateShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    m_lastStats.integratedSurfelsProcessed = integrationCount;
    m_integrationCursor = (m_integrationCursor + integrationCount) % kMaxSurfels;
}

void SurfelGIPass::ResetIrradianceFrameState(const RuntimeBudget& budget)
{
    if (!m_irradianceHeaderSSBO) {
        return;
    }

    GpuIrradianceHeader header{};
    header.config = glm::uvec4(
        budget.maxIrradianceRays,
        0u,
        0u,
        kMaxSurfels);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_irradianceHeaderSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(GpuIrradianceHeader), &header);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void SurfelGIPass::RunAdaptiveRayRequest(RenderContext& ctx, const RuntimeBudget& budget)
{
    if (!m_rayRequestShader || !m_rayRequestShader->IsValid()) {
        return;
    }
    const uint32_t raySurfelCount = kMaxSurfels;
    if (raySurfelCount == 0u) {
        return;
    }

    glUseProgram(m_rayRequestShader->GetProgramID());
    m_rayRequestShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_rayRequestShader->SetUniform("uSurfelStart", 0);
    m_rayRequestShader->SetUniform("uSurfelCount", static_cast<int>(raySurfelCount));
    m_rayRequestShader->SetUniform("uMaxRaysPerSurfel", std::max(ctx.surfelGIMaxRaysPerSurfel, 1));
    m_rayRequestShader->Dispatch(ComputeShader::CalculateWorkGroups(raySurfelCount, 256u), 1u, 1u);
    m_rayRequestShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunGlobalRayAllocation(RenderContext&, const RuntimeBudget& budget)
{
    if (!m_rayAllocateShader || !m_rayAllocateShader->IsValid()) {
        return;
    }
    const uint32_t raySurfelCount = kMaxSurfels;
    if (raySurfelCount == 0u || budget.maxIrradianceRays == 0u) {
        return;
    }

    glUseProgram(m_rayAllocateShader->GetProgramID());
    m_rayAllocateShader->SetUniform("uSurfelStart", 0);
    m_rayAllocateShader->SetUniform("uSurfelCount", static_cast<int>(raySurfelCount));
    m_rayAllocateShader->SetUniform("uGlobalRayBudget", static_cast<int>(budget.maxIrradianceRays));
    m_rayAllocateShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_rayAllocateShader->Dispatch(ComputeShader::CalculateWorkGroups(raySurfelCount, 256u), 1u, 1u);
    m_rayAllocateShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunBoundedRayTrace(
    RenderContext& ctx,
    const std::shared_ptr<DirectionalLight>& dirLight,
    const RuntimeBudget& budget)
{
    if (!m_rayTraceShader || !m_rayTraceShader->IsValid()) {
        return;
    }
    const uint32_t raySurfelCount = kMaxSurfels;
    if (raySurfelCount == 0u) {
        return;
    }
    if (!ctx.rtSceneResources || !ctx.rtSceneResources->IsIncrementalReady()) {
        return;
    }

    const glm::vec3 lightDirection = dirLight ? dirLight->GetLightDirection() : glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 lightColor = dirLight && dirLight->IsEnabled() ? dirLight->GetEffectiveColor() : glm::vec3(0.0f);
    ctx.rtSceneResources->UpdateLights(ctx.lightManager);

    glUseProgram(m_rayTraceShader->GetProgramID());
    ctx.rtSceneResources->BindIncrementalForTracing(0u, 1u, 31u, 32u);
    ctx.rtSceneResources->BindLights(2u);
    m_rayTraceShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_rayTraceShader->SetUniform("uSurfelStart", 0);
    m_rayTraceShader->SetUniform("uSurfelCount", static_cast<int>(raySurfelCount));
    m_rayTraceShader->SetUniform("u_triangleCount", static_cast<int>(ctx.rtSceneResources->GetIncrementalTriangleCount()));
    m_rayTraceShader->SetUniform("u_bvhNodeCount", static_cast<int>(ctx.rtSceneResources->GetIncrementalNodeCount()));
    m_rayTraceShader->SetUniform("u_rtInstanceCount", static_cast<int>(ctx.rtSceneResources->GetIncrementalInstanceCount()));
    m_rayTraceShader->SetUniform("u_rtInstanceNodeCount", static_cast<int>(ctx.rtSceneResources->GetIncrementalInstanceNodeCount()));
    m_rayTraceShader->SetUniform("u_lightCount", static_cast<int>(ctx.rtSceneResources->GetLightCount()));
    m_rayTraceShader->SetUniform("uDirectionalLightDir", lightDirection);
    m_rayTraceShader->SetUniform("uDirectionalLightColor", lightColor);
    m_rayTraceShader->SetUniform("uCameraPos", m_prevCameraPos);
    m_rayTraceShader->SetUniform("uMaxRayDistance", 8.0f);
    m_rayTraceShader->Dispatch(ComputeShader::CalculateWorkGroups(raySurfelCount, 256u), 1u, 1u);
    m_rayTraceShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunTemporalAccumulation(RenderContext&, const RuntimeBudget& budget)
{
    if (!m_temporalAccumulateShader || !m_temporalAccumulateShader->IsValid()) {
        return;
    }
    const uint32_t raySurfelCount = kMaxSurfels;
    if (raySurfelCount == 0u) {
        return;
    }

    glUseProgram(m_temporalAccumulateShader->GetProgramID());
    m_temporalAccumulateShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_temporalAccumulateShader->SetUniform("uSurfelStart", 0);
    m_temporalAccumulateShader->SetUniform("uSurfelCount", static_cast<int>(raySurfelCount));
    m_temporalAccumulateShader->Dispatch(ComputeShader::CalculateWorkGroups(raySurfelCount, 256u), 1u, 1u);
    m_temporalAccumulateShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunGuidingUpdate(RenderContext&, const RuntimeBudget& budget)
{
    if (!m_guidingUpdateShader || !m_guidingUpdateShader->IsValid()) {
        return;
    }
    const uint32_t raySurfelCount = kMaxSurfels;
    if (raySurfelCount == 0u) {
        return;
    }

    glUseProgram(m_guidingUpdateShader->GetProgramID());
    m_guidingUpdateShader->SetUniform("uSurfelStart", 0);
    m_guidingUpdateShader->SetUniform("uSurfelCount", static_cast<int>(raySurfelCount));
    m_guidingUpdateShader->Dispatch(ComputeShader::CalculateWorkGroups(raySurfelCount, 256u), 1u, 1u);
    m_guidingUpdateShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunNeighbourSharing(RenderContext&, const RuntimeBudget& budget)
{
    if (!m_neighbourShareShader || !m_neighbourShareShader->IsValid()) {
        return;
    }
    const uint32_t shareCount = kMaxSurfels;
    if (shareCount == 0u) {
        return;
    }

    glUseProgram(m_neighbourShareShader->GetProgramID());
    m_neighbourShareShader->SetUniform("uSurfelStart", static_cast<int>(m_sharingCursor));
    m_neighbourShareShader->SetUniform("uSurfelCount", static_cast<int>(shareCount));
    m_neighbourShareShader->Dispatch(ComputeShader::CalculateWorkGroups(shareCount, 256u), 1u, 1u);
    m_neighbourShareShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
    m_sharingCursor = (m_sharingCursor + shareCount) % kMaxSurfels;
}

void SurfelGIPass::RunRadialDepthValidityUpdate(RenderContext&, const RuntimeBudget& budget)
{
    if (!m_radialDepthUpdateShader || !m_radialDepthUpdateShader->IsValid()) {
        return;
    }
    const uint32_t raySurfelCount = kMaxSurfels;
    if (raySurfelCount == 0u) {
        return;
    }

    glUseProgram(m_radialDepthUpdateShader->GetProgramID());
    m_radialDepthUpdateShader->SetUniform("uSurfelStart", 0);
    m_radialDepthUpdateShader->SetUniform("uSurfelCount", static_cast<int>(raySurfelCount));
    m_radialDepthUpdateShader->Dispatch(ComputeShader::CalculateWorkGroups(raySurfelCount, 256u), 1u, 1u);
    m_radialDepthUpdateShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
    m_rayCursor = 0;
}

void SurfelGIPass::PublishResources(RenderContext& ctx) const
{
    ctx.surfelGISurfelBuffer = m_surfelSSBO;
    ctx.surfelGIHeaderBuffer = m_headerSSBO;
    ctx.surfelGIGridHeaderBuffer = m_gridHeaderSSBO;
    ctx.surfelGIGridEntryBuffer = m_gridEntrySSBO;
    ctx.surfelGIRadialDepthBinsBuffer = m_radialDepthBinsSSBO;
    ctx.surfelGIApplyStrength = kSurfelLightingApplyStrength;
    ctx.surfelGIGridReady =
        m_surfelSSBO != 0 &&
        m_headerSSBO != 0 &&
        m_gridHeaderSSBO != 0 &&
        m_gridEntrySSBO != 0 &&
        m_radialDepthBinsSSBO != 0;
}

void SurfelGIPass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>& sceneGraph,
    const std::shared_ptr<Camera>& camera,
    const std::shared_ptr<DirectionalLight>& dirLight,
    const std::shared_ptr<Skybox>&)
{
    if (!ctx.enableSurfelGI || !ctx.gbufferFBO || !sceneGraph || !camera) {
        ctx.surfelGIGridReady = false;
        return;
    }

    RenderSystem* renderSystem = sceneGraph->GetRenderSystem();
    if (!renderSystem || !renderSystem->GetTransformBufferID()) {
        ctx.surfelGIGridReady = false;
        return;
    }

    EnsureResources(ctx);
    if (m_needsPoolInit) {
        InitializePool();
    }
    if (!ctx.rtSceneResources) {
        ctx.rtSceneResources = std::make_shared<RTSceneResources>();
    }
    RTSceneResources::IncrementalBuildSettings rtBuildSettings;
    rtBuildSettings.maxBuildMsPerFrame = std::max(ctx.surfelGIRTBuildBudgetMs, 0.1f);
    rtBuildSettings.maxBlasTrianglesPerFrame = static_cast<size_t>(std::max(ctx.surfelGIRTMaxBLASTrianglesPerFrame, 1));
    rtBuildSettings.maxResidentBytes = static_cast<size_t>(std::max(ctx.surfelGIRTMaxResidentMB, 1)) * 1024ull * 1024ull;
    rtBuildSettings.includeSkinnedMeshes = ctx.surfelGIRTIncludeSkinnedMeshes;
    ctx.rtSceneResources->EnsureIncrementalBLAS(sceneGraph, rtBuildSettings);

    const GLuint transformBuffer = renderSystem->GetTransformBufferID();
    const uint32_t maxTransformID = static_cast<uint32_t>(std::min<size_t>(
        renderSystem->GetTransformRecordCount(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    const glm::mat4 invViewProj = glm::inverse(ctx.proj * ctx.view);
    const glm::mat4 invView = glm::inverse(ctx.view);
    const glm::vec3 cameraPos = camera->GetCameraPosition();
    float cameraMotion = 0.0f;
    const glm::vec3 cameraFront = glm::normalize(camera->GetCameraFrontVector());
    if (m_hasPrevCameraPos) {
        const float positionalMotion = glm::length(cameraPos - m_prevCameraPos);
        const float rotationalMotion = glm::length(cameraFront - m_prevCameraFront) * 0.75f;
        cameraMotion = std::max(positionalMotion, rotationalMotion);
    }
    cameraMotion = std::max(cameraMotion, 0.35f);
    m_prevCameraPos = cameraPos;
    m_prevCameraFront = cameraFront;
    m_hasPrevCameraPos = true;

    UpdateDynamicBudgetScale(ctx);
    RuntimeBudget budget = ComputeRuntimeBudget(ctx);
    const float motionPressure = std::clamp((cameraMotion - 0.015f) / 0.22f, 0.0f, 1.0f);
    if (motionPressure > 0.0f) {
        const uint32_t totalTiles = std::max(m_allocatedTileCountX * m_allocatedTileCountY, 1u);
        const auto motionBoost = [motionPressure](uint32_t current, uint32_t target) {
            const float boosted = LerpFloat(static_cast<float>(current), static_cast<float>(target), motionPressure);
            return std::max(current, static_cast<uint32_t>(std::round(boosted)));
        };
        const uint32_t tileTarget = std::min(totalTiles, std::max(budget.maxTilesToScan, 2048u));
        const uint32_t candidateTarget = std::min(totalTiles, std::max(budget.maxSpawnCandidates, 768u));
        budget.maxTilesToScan = motionBoost(budget.maxTilesToScan, tileTarget);
        budget.maxSpawnCandidates = motionBoost(budget.maxSpawnCandidates, candidateTarget);
        budget.maxSurfelsToSpawn = motionBoost(budget.maxSurfelsToSpawn, std::max(budget.maxSurfelsToSpawn, 192u));
        budget.maxRecycleDecisions = motionBoost(budget.maxRecycleDecisions, std::max(budget.maxRecycleDecisions, 4096u));
        budget.maxProjectedSurfels = motionBoost(budget.maxProjectedSurfels, std::max(budget.maxProjectedSurfels, 24576u));
        budget.maxCoarseCoverageSurfels = motionBoost(budget.maxCoarseCoverageSurfels, std::max(budget.maxCoarseCoverageSurfels, 24576u));
        budget.maxLifecycleUpdates = motionBoost(budget.maxLifecycleUpdates, std::max(budget.maxLifecycleUpdates, 32768u));
        budget.gridRebuildInterval = 1u;
    }
    const float userScale = std::clamp(ctx.surfelGIBudgetScale, 0.25f, 2.0f);
    m_lastStats.budgetScale = std::clamp(m_dynamicBudgetScale * userScale, 0.20f, 2.25f);
    m_lastStats.gridRebuildInterval = std::max(budget.gridRebuildInterval, 1u);
    m_projectedCoverageMaxTransformID = maxTransformID;

    using Clock = std::chrono::high_resolution_clock;
    const auto totalBegin = Clock::now();

    BindCommonBuffers(transformBuffer);
    ResetFrameStats(m_frameIndex);
    m_lastStats.tilesScannedThisFrame = 0;
    m_lastStats.tilesSkippedByConfidence = 0;
    m_lastStats.undercoveredTilesQueued = 0;
    m_lastStats.spawnCandidatesEvaluated = 0;
    m_lastStats.queueOverflowCount = 0;
    m_lastStats.projectedSurfelsProcessed = 0;
    m_lastStats.lifecycleSurfelsProcessed = 0;
    m_lastStats.recycleSurfelsProcessed = 0;
    m_lastStats.integratedSurfelsProcessed = 0;
    m_lastStats.requestedRaysThisFrame = 0;
    m_lastStats.allocatedRaysThisFrame = 0;
    m_lastStats.rayEligibleSurfels = 0;
    m_lastStats.rayActiveSurfels = 0;
    m_lastStats.rayEvaluatedSurfels = 0;
    m_lastStats.sharedSurfels = 0;
    m_lastStats.radialDepthUpdates = 0;
    m_lastStats.bleedRejectedContributions = 0;
    m_lastStats.guidingUpdates = 0;
    m_lastStats.rayBudgetUtilizationPercent = 0.0f;
    m_lastStats.tileSelectTimeMs = 0.0f;
    m_lastStats.rayRequestTimeMs = 0.0f;
    m_lastStats.rayAllocationTimeMs = 0.0f;
    m_lastStats.rayTraceTimeMs = 0.0f;
    m_lastStats.temporalAccumulationTimeMs = 0.0f;
    m_lastStats.guidingTimeMs = 0.0f;
    m_lastStats.sharingTimeMs = 0.0f;
    m_lastStats.radialDepthTimeMs = 0.0f;

    auto stageBegin = Clock::now();
    RunPersistentStateUpdate(ctx, *renderSystem, camera, budget);
    m_lastStats.lifecycleTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    if (m_gridRebuildCountdown == 0u) {
        stageBegin = Clock::now();
        RebuildSpatialGrid(cameraPos);
        m_lastStats.gridBuildTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();
        m_gridRebuildCountdown = m_lastStats.gridRebuildInterval > 0u ? (m_lastStats.gridRebuildInterval - 1u) : 0u;
    } else {
        m_lastStats.gridBuildTimeMs = 0.0f;
        --m_gridRebuildCountdown;
    }
    m_lastStats.gridRebuildCountdown = m_gridRebuildCountdown;

    stageBegin = Clock::now();
    RunRecycleDecision(ctx, budget);
    m_lastStats.recycleTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    RunPersistentTileCoverage(ctx, budget, cameraMotion);

    stageBegin = Clock::now();
    RunCoverageGapFill(ctx, *renderSystem, camera, invViewProj, invView, budget);
    m_lastStats.spawnTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunIrradianceIntegration(ctx, dirLight, budget);
    m_lastStats.integrationTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    ResetIrradianceFrameState(budget);

    stageBegin = Clock::now();
    RunAdaptiveRayRequest(ctx, budget);
    m_lastStats.rayRequestTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunGlobalRayAllocation(ctx, budget);
    m_lastStats.rayAllocationTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunBoundedRayTrace(ctx, dirLight, budget);
    m_lastStats.rayTraceTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunTemporalAccumulation(ctx, budget);
    m_lastStats.temporalAccumulationTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunGuidingUpdate(ctx, budget);
    m_lastStats.guidingTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunNeighbourSharing(ctx, budget);
    m_lastStats.sharingTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunRadialDepthValidityUpdate(ctx, budget);
    m_lastStats.radialDepthTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    m_lastStats.totalTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - totalBegin).count();
    m_lastStats.tileCursor = m_tileScanCursor;
    m_lastStats.lifecycleCursor = m_lifecycleCursor;
    m_lastStats.recycleCursor = m_recycleCursor;
    m_lastStats.projectedCursor = m_projectCursor;
    PublishResources(ctx);

    glUseProgram(0);
    ++m_frameIndex;
    if (ShouldReadBackStats(m_frameIndex)) {
        ReadBackStats();
    }
}

void SurfelGIPass::ReadBackStats()
{
    if (!m_headerSSBO) {
        return;
    }

    GpuPoolHeader header{};
    glBindBuffer(GL_COPY_READ_BUFFER, m_headerSSBO);
    glGetBufferSubData(GL_COPY_READ_BUFFER, 0, sizeof(GpuPoolHeader), &header);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);

    m_lastStats.liveCount = header.counts.y;
    m_lastStats.freeCount = header.counts.z;
    m_lastStats.spawnedThisFrame = header.frameStats.x;
    m_lastStats.recycledThisFrame = header.frameStats.y;
    m_lastStats.dormantCount = header.frameStats.z;
    m_lastStats.spawnAttemptsThisFrame = header.frameStats.w;
    m_lastStats.coveragePreventedSpawns = header.coverageStats.x;
    m_lastStats.duplicateRejectedSpawns = header.coverageStats.y;
    m_lastStats.freeStackExhaustedSpawns = header.coverageStats.z;
    m_lastStats.probabilityRejectedSpawns = header.coverageStats.w;
    m_lastStats.oldCoverageContributions = header.contributionStats.x;
    m_lastStats.newCoverageContributions = header.contributionStats.y;
    m_lastStats.oldIntegratedContributions = header.contributionStats.z;
    m_lastStats.newIntegratedContributions = header.contributionStats.w;
    m_lastStats.recycleCandidateCount = header.recycleStats.x;
    m_lastStats.budgetRecycledCount = header.recycleStats.y;
    m_lastStats.invalidTransformRecycledCount = header.recycleStats.z;
    m_lastStats.lifecycleViolationCount = header.recycleStats.w;
    m_lastStats.visibleSurfacePixelCount = header.coverageMetricStats.x;
    m_lastStats.validCoveragePixelCount = header.coverageMetricStats.y;
    m_lastStats.validCoveragePercent = header.coverageMetricStats.x > 0u
        ? (100.0f * static_cast<float>(header.coverageMetricStats.y) / static_cast<float>(header.coverageMetricStats.x))
        : 0.0f;
    m_lastStats.tilesScannedThisFrame = header.tileWorkStats.x;
    m_lastStats.tilesSkippedByConfidence = header.tileWorkStats.y;
    m_lastStats.undercoveredTilesQueued = header.tileWorkStats.z;
    m_lastStats.spawnCandidatesEvaluated = header.tileWorkStats.w;
    m_lastStats.projectedSurfelsProcessed = header.budgetStats.x;
    m_lastStats.lifecycleSurfelsProcessed = header.budgetStats.y;
    m_lastStats.recycleSurfelsProcessed = header.budgetStats.z;
    m_lastStats.integratedSurfelsProcessed = header.budgetStats.w;
    m_lastStats.tileCursor = m_tileScanCursor;
    m_lastStats.lifecycleCursor = m_lifecycleCursor;
    m_lastStats.recycleCursor = m_recycleCursor;
    m_lastStats.projectedCursor = m_projectCursor;
    m_lastStats.queueOverflowCount = header.queueStats.x;
    m_lastStats.gridRebuildInterval = std::max(header.queueStats.z, 1u);
    m_lastStats.gridRebuildCountdown = header.queueStats.w;
    m_lastStats.tileCountX = header.tiling.x;
    m_lastStats.tileCountY = header.tiling.y;
    m_lastStats.gridCellCount = header.tiling.w;

    if (m_lastStats.liveCount > kMaxSurfels || m_lastStats.freeCount > kMaxSurfels) {
        std::cerr << "[SurfelGIPass] Invalid pool counters detected; reinitializing surfel pool.\n";
        m_needsPoolInit = true;
        m_lastStats.liveCount = 0;
        m_lastStats.freeCount = kMaxSurfels;
    }

    if (m_irradianceHeaderSSBO) {
        GpuIrradianceHeader irradianceHeader{};
        glBindBuffer(GL_COPY_READ_BUFFER, m_irradianceHeaderSSBO);
        glGetBufferSubData(GL_COPY_READ_BUFFER, 0, sizeof(GpuIrradianceHeader), &irradianceHeader);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);

        m_lastStats.requestedRaysThisFrame = irradianceHeader.rayStats.x;
        m_lastStats.allocatedRaysThisFrame = irradianceHeader.rayStats.y;
        m_lastStats.rayEligibleSurfels = irradianceHeader.rayStats.z;
        m_lastStats.rayActiveSurfels = irradianceHeader.rayStats.w;
        m_lastStats.rayEvaluatedSurfels = irradianceHeader.passStats.x;
        m_lastStats.sharedSurfels = irradianceHeader.passStats.y;
        m_lastStats.radialDepthUpdates = irradianceHeader.passStats.z;
        m_lastStats.bleedRejectedContributions = irradianceHeader.passStats.w;
        m_lastStats.guidingUpdates = irradianceHeader.debugStats.z;
        const uint32_t rayBudgetCap = std::max(irradianceHeader.config.x, 1u);
        m_lastStats.rayBudgetUtilizationPercent = irradianceHeader.config.x > 0u
            ? (100.0f * static_cast<float>(m_lastStats.allocatedRaysThisFrame) / static_cast<float>(rayBudgetCap))
            : 0.0f;
    }

    PushStatsHistory();
}

void SurfelGIPass::PushStatsHistory()
{
    const uint32_t index = m_statsHistoryHead % kStatsHistoryLength;
    m_liveHistory[index] = static_cast<float>(m_lastStats.liveCount);
    m_freeHistory[index] = static_cast<float>(m_lastStats.freeCount);
    m_spawnHistory[index] = static_cast<float>(m_lastStats.spawnedThisFrame);
    m_recycleHistory[index] = static_cast<float>(m_lastStats.recycledThisFrame);
    m_preventedSpawnHistory[index] = static_cast<float>(m_lastStats.coveragePreventedSpawns);
    m_oldContributionHistory[index] = static_cast<float>(
        m_lastStats.oldCoverageContributions + m_lastStats.oldIntegratedContributions);
    m_newContributionHistory[index] = static_cast<float>(
        m_lastStats.newCoverageContributions + m_lastStats.newIntegratedContributions);
    m_statsHistoryHead = (m_statsHistoryHead + 1u) % kStatsHistoryLength;
}

void SurfelGIPass::RenderDebug(RenderContext& ctx) const
{
    const bool overlayMode = ctx.surfelGIDebugMode >= 18 && ctx.surfelGIDebugMode <= 24;
    const bool tlasOverlayMode = ctx.surfelGIDebugMode == 24;
    if (!ctx.enableSurfelGI || ctx.surfelGIDebugMode <= 0 || !ctx.gbufferFBO) {
        return;
    }
    if (tlasOverlayMode && (!ctx.rtSceneResources || !ctx.rtSceneResources->IsIncrementalReady())) {
        return;
    }
    if (!overlayMode && (!m_debugProgram || !m_surfelSSBO || !m_headerSSBO || !m_gridHeaderSSBO)) {
        return;
    }
    if (overlayMode && (!m_debugOverlayProgram || !ctx.screenQuad ||
        !m_rawProjectedSupportTex || !m_projectedCoverageTex || !m_deficitTex ||
        !m_depthRejectTex || !m_normalRejectTex || !m_winnerIDTex)) {
        return;
    }

    const GLboolean wasDepthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean wasBlendEnabled = glIsEnabled(GL_BLEND);
    const GLboolean wasCullFaceEnabled = glIsEnabled(GL_CULL_FACE);
    GLboolean wasDepthMaskEnabled = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &wasDepthMaskEnabled);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ctx.width, ctx.height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    if (overlayMode) {
        glDisable(GL_BLEND);
        glUseProgram(m_debugOverlayProgram);

        GLuint overlayCoverageTex = m_projectedCoverageTex;
        int overlayModeIndex = 1;
        if (ctx.surfelGIDebugMode == 18) {
            overlayCoverageTex = m_rawProjectedSupportTex;
            overlayModeIndex = 0;
        } else if (ctx.surfelGIDebugMode == 19) {
            overlayCoverageTex = m_projectedCoverageTex;
            overlayModeIndex = 1;
        } else if (ctx.surfelGIDebugMode == 20) {
            overlayCoverageTex = m_projectedCoverageTex;
            overlayModeIndex = 2;
        } else if (ctx.surfelGIDebugMode == 21) {
            overlayCoverageTex = m_depthRejectTex;
            overlayModeIndex = 3;
        } else if (ctx.surfelGIDebugMode == 22) {
            overlayCoverageTex = m_normalRejectTex;
            overlayModeIndex = 4;
        } else if (ctx.surfelGIDebugMode == 23) {
            overlayCoverageTex = m_winnerIDTex;
            overlayModeIndex = 5;
        } else if (ctx.surfelGIDebugMode == 24) {
            overlayModeIndex = 6;
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, overlayCoverageTex);
        if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uCoverageTex"); loc >= 0) {
            glUniform1i(loc, 0);
        }

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_deficitTex);
        if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uDeficitTex"); loc >= 0) {
            glUniform1i(loc, 1);
        }

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
        if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uDepthTex"); loc >= 0) {
            glUniform1i(loc, 2);
        }

        if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uMode"); loc >= 0) {
            glUniform1i(loc, overlayModeIndex);
        }
        if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uCoverageThreshold"); loc >= 0) {
            glUniform1f(loc, std::max(ctx.surfelGICoverageThreshold, 0.05f));
        }

        if (tlasOverlayMode && ctx.rtSceneResources) {
            ctx.rtSceneResources->BindIncrementalForTracing(0, 1, 31, 32);

            const glm::mat4 invViewProj = glm::inverse(ctx.proj * ctx.view);
            if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uInvViewProj"); loc >= 0) {
                glUniformMatrix4fv(loc, 1, GL_FALSE, &(invViewProj[0][0]));
            }
            if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "u_triangleCount"); loc >= 0) {
                glUniform1i(loc, static_cast<GLint>(ctx.rtSceneResources->GetIncrementalTriangleCount()));
            }
            if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "u_bvhNodeCount"); loc >= 0) {
                glUniform1i(loc, static_cast<GLint>(ctx.rtSceneResources->GetIncrementalNodeCount()));
            }
            if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "u_rtInstanceCount"); loc >= 0) {
                glUniform1i(loc, static_cast<GLint>(ctx.rtSceneResources->GetIncrementalInstanceCount()));
            }
            if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "u_rtInstanceNodeCount"); loc >= 0) {
                glUniform1i(loc, static_cast<GLint>(ctx.rtSceneResources->GetIncrementalInstanceNodeCount()));
            }
            if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uHeatmapColorLimit"); loc >= 0) {
                glUniform1i(loc, std::max(ctx.surfelGITLASHeatmapColorLimit, 1));
            }
            if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uDisplayMultipleBVHLayers"); loc >= 0) {
                glUniform1i(loc, ctx.surfelGITLASDisplayMultipleBVHLayers ? 1 : 0);
            }
            if (GLint loc = glGetUniformLocation(m_debugOverlayProgram, "uBVHLayerToDisplay"); loc >= 0) {
                glUniform1i(loc, std::max(ctx.surfelGITLASBVHLayerToDisplay, 0));
            }
        }

        ctx.screenQuad->Render();
        glUseProgram(0);

        if (wasBlendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glDepthMask(wasDepthMaskEnabled);
        if (wasDepthTestEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (wasCullFaceEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        return;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_debugProgram);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSurfels, m_surfelSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingHeader, m_headerSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingGridHeaders, m_gridHeaderSSBO);

    const glm::mat4 viewProj = ctx.proj * ctx.view;
    if (GLint loc = glGetUniformLocation(m_debugProgram, "uViewProj"); loc >= 0) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, &(viewProj[0][0]));
    }
    if (GLint loc = glGetUniformLocation(m_debugProgram, "uDebugMode"); loc >= 0) {
        glUniform1i(loc, ctx.surfelGIDebugMode);
    }
    if (GLint loc = glGetUniformLocation(m_debugProgram, "uFrameIndex"); loc >= 0) {
        glUniform1ui(loc, m_frameIndex);
    }
    if (GLint loc = glGetUniformLocation(m_debugProgram, "uScreenSize"); loc >= 0) {
        glUniform2f(loc, float(ctx.width), float(ctx.height));
    }
    const glm::mat4 invView = glm::inverse(ctx.view);
    const glm::vec3 cameraPos(invView[3]);
    if (GLint loc = glGetUniformLocation(m_debugProgram, "uCameraPos"); loc >= 0) {
        glUniform3f(loc, cameraPos.x, cameraPos.y, cameraPos.z);
    }
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    if (GLint loc = glGetUniformLocation(m_debugProgram, "uDepthTex"); loc >= 0) {
        glUniform1i(loc, 3);
    }

    const uint32_t debugSourceCount = std::clamp(m_lastStats.liveCount + 4096u, 0u, kMaxSurfels);
    const uint32_t debugInstanceCount = std::min(debugSourceCount, kMaxDebugSurfelInstances);
    const uint32_t debugInstanceStride = std::max(1u, (debugSourceCount + debugInstanceCount - 1u) / std::max(debugInstanceCount, 1u));
    if (GLint loc = glGetUniformLocation(m_debugProgram, "uDebugInstanceStride"); loc >= 0) {
        glUniform1ui(loc, debugInstanceStride);
    }
    glBindVertexArray(m_debugVAO);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(debugInstanceCount));
    glBindVertexArray(0);
    glUseProgram(0);

    if (wasBlendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glDepthMask(wasDepthMaskEnabled);
    if (wasDepthTestEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasCullFaceEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}
