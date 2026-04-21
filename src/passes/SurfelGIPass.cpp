#include "SurfelGIPass.h"

#include "../Camera.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../RenderSystem.h"
#include "../SceneGraph.h"
#include "../ShaderLoader.h"
#include "../TextureUnits.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>

namespace {
    constexpr GLuint kBindingSurfels = 20;
    constexpr GLuint kBindingHeader = 21;
    constexpr GLuint kBindingFreeStack = 22;
    constexpr GLuint kBindingRecycleStack = 23;
    constexpr GLuint kBindingTileCoverage = 24;
    constexpr GLuint kBindingGridHeaders = 25;
    constexpr GLuint kBindingGridEntries = 26;
    constexpr GLuint kBindingTransforms = 6;

    constexpr uint32_t kGridDimX = 32;
    constexpr uint32_t kGridDimY = 18;
    constexpr uint32_t kGridDimZ = 32;
    constexpr uint32_t kMaxEntriesPerCell = 64;
    constexpr float kCentralGridExtent = 24.0f;
    constexpr float kNearScale = 1.0f;
    constexpr float kFarScale = 120.0f;

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
}

bool SurfelGIPass::Initialize(RenderContext&)
{
    m_initShader = std::make_unique<ComputeShader>();
    m_lifecycleShader = std::make_unique<ComputeShader>();
    m_gridClearShader = std::make_unique<ComputeShader>();
    m_gridBuildShader = std::make_unique<ComputeShader>();
    m_spawnShader = std::make_unique<ComputeShader>();

    bool ok = true;
    ok &= m_initShader->CreateFromFile("shaders/surfel_gi_init_comp.glsl");
    ok &= m_lifecycleShader->CreateFromFile("shaders/surfel_gi_lifecycle_comp.glsl");
    ok &= m_gridClearShader->CreateFromFile("shaders/surfel_gi_grid_clear_comp.glsl");
    ok &= m_gridBuildShader->CreateFromFile("shaders/surfel_gi_grid_build_comp.glsl");
    ok &= m_spawnShader->CreateFromFile("shaders/surfel_gi_spawn_comp.glsl");

    m_debugProgram = CreateShaderProgram("shaders/surfel_gi_debug_vert.glsl", "shaders/surfel_gi_debug_frag.glsl");
    ok &= (m_debugProgram != 0);
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
    m_needsPoolInit = true;
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
}

void SurfelGIPass::EnsureResources(RenderContext& ctx)
{
    const uint32_t tileSize = static_cast<uint32_t>(std::clamp(ctx.surfelGITileSize, 4, 16));
    const uint32_t tileCountX = (static_cast<uint32_t>(std::max(ctx.width, 1)) + tileSize - 1u) / tileSize;
    const uint32_t tileCountY = (static_cast<uint32_t>(std::max(ctx.height, 1)) + tileSize - 1u) / tileSize;
    const uint32_t gridCellCount = kGridDimX * kGridDimY * kGridDimZ;

    const bool sizeChanged =
        tileCountX != m_allocatedTileCountX ||
        tileCountY != m_allocatedTileCountY ||
        gridCellCount != m_allocatedGridCellCount;

    if (!sizeChanged && m_headerSSBO) {
        return;
    }

    m_allocatedTileCountX = tileCountX;
    m_allocatedTileCountY = tileCountY;
    m_allocatedGridCellCount = gridCellCount;

    const GpuPoolHeader header{
        glm::uvec4(kMaxSurfels, 0u, kMaxSurfels, m_frameIndex),
        glm::uvec4(0u),
        glm::uvec4(tileCountX, tileCountY, tileSize, gridCellCount),
        glm::uvec4(kGridDimX, kGridDimY, kGridDimZ, kMaxEntriesPerCell),
        glm::vec4(kCentralGridExtent, kNearScale, kFarScale, (kCentralGridExtent * 2.0f) / float(kGridDimX))
    };

    ResizeBuffer(m_headerSSBO, sizeof(GpuPoolHeader), &header);
    ResizeBuffer(m_surfelSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(GpuSurfelRecord)));
    ResizeBuffer(m_freeStackSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(uint32_t)));
    ResizeBuffer(m_recycleStackSSBO, static_cast<GLsizeiptr>(kMaxSurfels * sizeof(uint32_t)));
    ResizeBuffer(m_tileCoverageSSBO, static_cast<GLsizeiptr>(tileCountX * tileCountY * sizeof(glm::uvec4)));
    ResizeBuffer(m_gridHeaderSSBO, static_cast<GLsizeiptr>(gridCellCount * sizeof(glm::uvec4)));
    ResizeBuffer(m_gridEntrySSBO, static_cast<GLsizeiptr>(gridCellCount * kMaxEntriesPerCell * sizeof(uint32_t)));
    ResizeBuffer(m_statsReadbackPBO, sizeof(GpuPoolHeader));

    m_needsPoolInit = true;
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

void SurfelGIPass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>& sceneGraph,
    const std::shared_ptr<Camera>& camera,
    const std::shared_ptr<DirectionalLight>&,
    const std::shared_ptr<Skybox>&)
{
    if (!ctx.enableSurfelGI || !ctx.gbufferFBO || !sceneGraph || !camera) {
        return;
    }

    RenderSystem* renderSystem = sceneGraph->GetRenderSystem();
    if (!renderSystem || !renderSystem->GetTransformBufferID()) {
        return;
    }

    EnsureResources(ctx);
    if (m_needsPoolInit) {
        InitializePool();
    }

    const GLuint transformBuffer = renderSystem->GetTransformBufferID();
    const glm::mat4 invViewProj = glm::inverse(ctx.proj * ctx.view);
    const glm::mat4 invView = glm::inverse(ctx.view);
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

    BindCommonBuffers(transformBuffer);

    const uint32_t frame = m_frameIndex;
    const glm::uvec4 zeroFrameStats(0u);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_headerSSBO);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, counts) + sizeof(uint32_t) * 3u),
        sizeof(uint32_t),
        &frame);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLintptr>(offsetof(GpuPoolHeader, frameStats)),
        sizeof(glm::uvec4),
        &zeroFrameStats);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glUseProgram(m_lifecycleShader->GetProgramID());
    m_lifecycleShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_lifecycleShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
    m_lifecycleShader->SetUniform("uView", ctx.view);
    m_lifecycleShader->SetUniform("uProjection", ctx.proj);
    m_lifecycleShader->SetUniform("uCameraPos", cameraPos);
    m_lifecycleShader->SetUniform("uTargetRadiusPixels", ctx.surfelGITargetRadiusPixels);
    m_lifecycleShader->SetUniform("uRecyclePressure", ctx.surfelGIRecyclePressure);
    m_lifecycleShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_lifecycleShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(m_gridClearShader->GetProgramID());
    m_gridClearShader->SetUniform("uGridCellCount", static_cast<int>(m_allocatedGridCellCount));
    m_gridClearShader->Dispatch(ComputeShader::CalculateWorkGroups(m_allocatedGridCellCount, 256u), 1u, 1u);
    m_gridClearShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(m_gridBuildShader->GetProgramID());
    m_gridBuildShader->SetUniform("uView", ctx.view);
    m_gridBuildShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_gridBuildShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);

    bindGBufferTextures(m_spawnShader->GetProgramID());
    m_spawnShader->SetUniform("uFrameIndex", static_cast<int>(m_frameIndex));
    m_spawnShader->SetUniform("uMaxTransformID", static_cast<int>(renderSystem->GetTransformRecordCount()));
    m_spawnShader->SetUniform("uResolution", glm::vec2(float(ctx.width), float(ctx.height)));
    m_spawnShader->SetUniform("uView", ctx.view);
    m_spawnShader->SetUniform("uProjection", ctx.proj);
    m_spawnShader->SetUniform("uInvViewProj", invViewProj);
    m_spawnShader->SetUniform("uInvView", invView);
    m_spawnShader->SetUniform("uTargetRadiusPixels", ctx.surfelGITargetRadiusPixels);
    m_spawnShader->SetUniform("uCoverageThreshold", ctx.surfelGICoverageThreshold);
    m_spawnShader->SetUniform("uNormalReject", ctx.surfelGINormalReject);
    m_spawnShader->SetUniform("uCameraPos", cameraPos);
    m_spawnShader->Dispatch(m_allocatedTileCountX, m_allocatedTileCountY, 1u);
    m_spawnShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(m_gridClearShader->GetProgramID());
    m_gridClearShader->SetUniform("uGridCellCount", static_cast<int>(m_allocatedGridCellCount));
    m_gridClearShader->Dispatch(ComputeShader::CalculateWorkGroups(m_allocatedGridCellCount, 256u), 1u, 1u);
    m_gridClearShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(m_gridBuildShader->GetProgramID());
    m_gridBuildShader->SetUniform("uView", ctx.view);
    m_gridBuildShader->Dispatch(ComputeShader::CalculateWorkGroups(kMaxSurfels, 256u), 1u, 1u);
    m_gridBuildShader->WaitForCompletion(GL_SHADER_STORAGE_BARRIER_BIT);

    glUseProgram(0);
    ++m_frameIndex;
    ReadBackStats();
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
    m_lastStats.tileCountX = header.tiling.x;
    m_lastStats.tileCountY = header.tiling.y;
    m_lastStats.gridCellCount = header.tiling.w;
}

void SurfelGIPass::RenderDebug(RenderContext& ctx) const
{
    if (!ctx.enableSurfelGI || ctx.surfelGIDebugMode <= 0 || !m_debugProgram || !m_surfelSSBO || !ctx.gbufferFBO) {
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

    glBindVertexArray(m_debugVAO);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(kMaxSurfels));
    glBindVertexArray(0);
    glUseProgram(0);

    if (wasBlendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glDepthMask(wasDepthMaskEnabled);
    if (wasDepthTestEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (wasCullFaceEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}
