#include "SurfelGIPass.h"

#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../RenderSystem.h"
#include "../ScreenQuad.h"
#include "../SceneGraph.h"
#include "../ShaderLoader.h"
#include "../TextureUnits.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
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
    constexpr GLuint kBindingTransforms = 6;
    constexpr GLuint kImageProjectedCoverage = 0;
    constexpr GLuint kImageDeficit = 1;
    constexpr GLuint kImageRawProjectedSupport = 2;
    constexpr GLuint kImageDepthReject = 3;
    constexpr GLuint kImageNormalReject = 4;
    constexpr GLuint kImageWinnerID = 5;
    constexpr GLuint kImageCoverageHistory = 6;

    constexpr uint32_t kSurfelTileSize = 16;
    constexpr uint32_t kSpawnIterations = 2;
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
    constexpr uint32_t kExactCoverageFrameModulo = 2;
    constexpr uint32_t kIntegrationFrameInterval = 2;

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
    m_projectCoverageShader = std::make_unique<ComputeShader>();
    m_deficitShader = std::make_unique<ComputeShader>();
    m_gridClearShader = std::make_unique<ComputeShader>();
    m_gridBuildShader = std::make_unique<ComputeShader>();
    m_spawnShader = std::make_unique<ComputeShader>();
    m_integrateShader = std::make_unique<ComputeShader>();

    bool ok = true;
    ok &= m_initShader->CreateFromFile("shaders/surfel_gi_init_comp.glsl");
    ok &= m_lifecycleShader->CreateFromFile("shaders/surfel_gi_lifecycle_comp.glsl");
    ok &= m_recycleShader->CreateFromFile("shaders/surfel_gi_recycle_comp.glsl");
    ok &= m_tileClearShader->CreateFromFile("shaders/surfel_gi_tile_clear_comp.glsl");
    ok &= m_tileCoverageShader->CreateFromFile("shaders/surfel_gi_tile_coverage_comp.glsl");
    ok &= m_projectCoverageShader->CreateFromFile("shaders/surfel_gi_coverage_project_comp.glsl");
    ok &= m_deficitShader->CreateFromFile("shaders/surfel_gi_deficit_comp.glsl");
    ok &= m_gridClearShader->CreateFromFile("shaders/surfel_gi_grid_clear_comp.glsl");
    ok &= m_gridBuildShader->CreateFromFile("shaders/surfel_gi_grid_build_comp.glsl");
    ok &= m_spawnShader->CreateFromFile("shaders/surfel_gi_spawn_comp.glsl");
    ok &= m_integrateShader->CreateFromFile("shaders/surfel_gi_integrate_comp.glsl");

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
    const std::array<GLuint, 8> buffers = {
        m_surfelSSBO,
        m_headerSSBO,
        m_freeStackSSBO,
        m_recycleStackSSBO,
        m_tileCoverageSSBO,
        m_gridHeaderSSBO,
        m_gridEntrySSBO,
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
    m_gridHeaderSSBO = 0;
    m_gridEntrySSBO = 0;
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
    const bool firstAllocation = m_headerSSBO == 0 || m_surfelSSBO == 0 || m_freeStackSSBO == 0;

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
        glm::uvec4(0u)
    };

    if (firstAllocation) {
        ResizeBuffer(m_headerSSBO, sizeof(GpuPoolHeader), &header);
        ResizeBuffer(m_surfelSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(GpuSurfelRecord)));
        ResizeBuffer(m_freeStackSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(uint32_t)));
        ResizeBuffer(m_recycleStackSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(uint32_t)));
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
        std::vector<glm::uvec4> zeroTiles(static_cast<size_t>(tileCountX) * tileCountY, glm::uvec4(0u));
        ResizeBuffer(m_tileCoverageSSBO,
            static_cast<GLsizeiptr>(zeroTiles.size() * sizeof(glm::uvec4)),
            zeroTiles.empty() ? nullptr : zeroTiles.data());
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
    m_needsPoolInit = false;
}

void SurfelGIPass::ResetFrameStats(uint32_t frameIndex)
{
    const glm::uvec4 zeroStats(0u);

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
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void SurfelGIPass::RunPersistentStateUpdate(RenderContext& ctx, RenderSystem& renderSystem, const std::shared_ptr<Camera>& camera)
{
    if (!m_lifecycleShader || !m_lifecycleShader->IsValid() || !camera) {
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
    m_lifecycleShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_lifecycleShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunRecycleDecision(RenderContext& ctx)
{
    if (!m_recycleShader || !m_recycleShader->IsValid()) {
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
    m_recycleShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_recycleShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RebuildSpatialGrid(const glm::mat4& view)
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
    m_gridBuildShader->SetUniform("uView", view);
    m_gridBuildShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_gridBuildShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunPersistentTileCoverage(RenderContext& ctx)
{
    if (!m_tileClearShader || !m_tileClearShader->IsValid()) {
        return;
    }

    using Clock = std::chrono::high_resolution_clock;

    const uint32_t tileCount = m_allocatedTileCountX * m_allocatedTileCountY;

    glUseProgram(m_tileClearShader->GetProgramID());
    m_tileClearShader->SetUniform("uTileCount", static_cast<int>(tileCount));
    m_tileClearShader->Dispatch(ComputeShader::CalculateWorkGroups(tileCount, 256u), 1u, 1u);
    m_tileClearShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    if (m_tileCoverageShader && m_tileCoverageShader->IsValid()) {
        const auto coarseBegin = Clock::now();
        glUseProgram(m_tileCoverageShader->GetProgramID());
        m_tileCoverageShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
        m_tileCoverageShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
        m_tileCoverageShader->SetUniform("uView", ctx.view);
        m_tileCoverageShader->SetUniform("uProjection", ctx.proj);
        m_tileCoverageShader->SetUniform("uCoverageThreshold", ctx.surfelGICoverageThreshold);
        m_tileCoverageShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
        m_tileCoverageShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);
        m_lastStats.coarseCoverageTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - coarseBegin).count();
    } else {
        m_lastStats.coarseCoverageTimeMs = 0.0f;
    }

    const glm::mat4 invViewProj = glm::inverse(ctx.proj * ctx.view);
    auto exactBegin = Clock::now();
    RunProjectedCoverage(ctx, invViewProj, m_projectedCoverageMaxTransformID);
    m_lastStats.exactCoverageTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - exactBegin).count();
    auto deficitBegin = Clock::now();
    RunCoverageDeficit(ctx);
    m_lastStats.deficitTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - deficitBegin).count();
}

void SurfelGIPass::RunProjectedCoverage(RenderContext& ctx, const glm::mat4& invViewProj, uint32_t maxTransformID)
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
    m_projectCoverageShader->SetUniform("uProjectionFrameModulo", static_cast<int>(kExactCoverageFrameModulo));
    m_projectCoverageShader->SetUniform("uProjectionFramePhase", static_cast<int>(m_frameIndex % kExactCoverageFrameModulo));

    m_projectCoverageShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 64u), 1u, 1u);
    m_projectCoverageShader->WaitForCompletion(
        GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
        GL_SHADER_STORAGE_BARRIER_BIT |
        GL_TEXTURE_FETCH_BARRIER_BIT);
}

void SurfelGIPass::RunCoverageDeficit(RenderContext& ctx)
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
    m_deficitShader->SetUniform("uCoverageHistoryHysteresis", 0.92f);

    const GLuint groupsX = ComputeShader::CalculateWorkGroups(static_cast<GLuint>(std::max(ctx.width, 1)), 16u);
    const GLuint groupsY = ComputeShader::CalculateWorkGroups(static_cast<GLuint>(std::max(ctx.height, 1)), 16u);
    m_deficitShader->Dispatch(groupsX, groupsY, 1u);
    m_deficitShader->WaitForCompletion(
        GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
        GL_TEXTURE_FETCH_BARRIER_BIT |
        GL_SHADER_STORAGE_BARRIER_BIT);
}

void SurfelGIPass::RunCoverageGapFill(RenderContext& ctx,
    RenderSystem& renderSystem,
    const std::shared_ptr<Camera>& camera,
    const glm::mat4& invViewProj,
    const glm::mat4& invView)
{
    if (!m_spawnShader || !m_spawnShader->IsValid() || !camera) {
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

    for (uint32_t iteration = 0; iteration < kSpawnIterations; ++iteration) {
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
        m_spawnShader->SetUniform("uSpawnIteration", static_cast<int>(iteration));
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
        m_spawnShader->Dispatch(m_allocatedTileCountX, m_allocatedTileCountY, 1u);
        m_spawnShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
}

void SurfelGIPass::RunIrradianceIntegration(RenderContext& ctx, const std::shared_ptr<DirectionalLight>& dirLight)
{
    if (!m_integrateShader || !m_integrateShader->IsValid()) {
        return;
    }
    if ((m_frameIndex % kIntegrationFrameInterval) != 0u) {
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
    m_integrateShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_integrateShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void SurfelGIPass::PublishResources(RenderContext& ctx) const
{
    ctx.surfelGISurfelBuffer = m_surfelSSBO;
    ctx.surfelGIHeaderBuffer = m_headerSSBO;
    ctx.surfelGIGridHeaderBuffer = m_gridHeaderSSBO;
    ctx.surfelGIGridEntryBuffer = m_gridEntrySSBO;
    ctx.surfelGIApplyStrength = kSurfelLightingApplyStrength;
    ctx.surfelGIGridReady =
        m_surfelSSBO != 0 &&
        m_headerSSBO != 0 &&
        m_gridHeaderSSBO != 0 &&
        m_gridEntrySSBO != 0;
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

    const GLuint transformBuffer = renderSystem->GetTransformBufferID();
    const uint32_t maxTransformID = static_cast<uint32_t>(std::min<size_t>(
        renderSystem->GetTransformRecordCount(),
        static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
    const glm::mat4 invViewProj = glm::inverse(ctx.proj * ctx.view);
    const glm::mat4 invView = glm::inverse(ctx.view);
    m_projectedCoverageMaxTransformID = maxTransformID;

    using Clock = std::chrono::high_resolution_clock;
    const auto totalBegin = Clock::now();

    BindCommonBuffers(transformBuffer);
    ResetFrameStats(m_frameIndex);

    auto stageBegin = Clock::now();
    RunPersistentStateUpdate(ctx, *renderSystem, camera);
    m_lastStats.lifecycleTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RebuildSpatialGrid(ctx.view);
    m_lastStats.gridBuildTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunRecycleDecision(ctx);
    m_lastStats.recycleTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    RunPersistentTileCoverage(ctx);

    stageBegin = Clock::now();
    RunCoverageGapFill(ctx, *renderSystem, camera, invViewProj, invView);
    m_lastStats.spawnTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    stageBegin = Clock::now();
    RunIrradianceIntegration(ctx, dirLight);
    m_lastStats.integrationTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - stageBegin).count();

    m_lastStats.totalTimeMs = std::chrono::duration<float, std::milli>(Clock::now() - totalBegin).count();
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
    m_lastStats.tileCountX = header.tiling.x;
    m_lastStats.tileCountY = header.tiling.y;
    m_lastStats.gridCellCount = header.tiling.w;

    if (m_lastStats.liveCount > kMaxSurfels || m_lastStats.freeCount > kMaxSurfels) {
        std::cerr << "[SurfelGIPass] Invalid pool counters detected; reinitializing surfel pool.\n";
        m_needsPoolInit = true;
        m_lastStats.liveCount = 0;
        m_lastStats.freeCount = kMaxSurfels;
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
    const bool overlayMode = ctx.surfelGIDebugMode >= 18 && ctx.surfelGIDebugMode <= 23;
    if (!ctx.enableSurfelGI || ctx.surfelGIDebugMode <= 0 || !ctx.gbufferFBO) {
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
