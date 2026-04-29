#include "SurfelIndirectDiffusePass.h"
#include "../ComputeShader.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../Camera.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <iostream>

namespace {
constexpr GLuint kSurfelBindingSurfels = 20;
constexpr GLuint kSurfelBindingHeader = 21;
constexpr GLuint kSurfelBindingGridHeaders = 25;
constexpr GLuint kSurfelBindingGridEntries = 26;
constexpr GLuint kSurfelBindingIrradianceHeader = 28;
constexpr GLuint kSurfelBindingRadialDepth = 30;
constexpr GLuint kSurfelBindingGridAverages = 33;
constexpr int kGatherResolutionDivisor = 2;
constexpr GLuint kWinnerSurfelTextureUnit = 7;
constexpr GLuint kHistoryTextureUnit = 8;
constexpr GLuint kVelocityTextureUnit = 9;
}

SurfelIndirectDiffusePass::~SurfelIndirectDiffusePass() = default;

bool SurfelIndirectDiffusePass::Initialize(RenderContext& context)
{
    m_gatherShader = std::make_unique<ComputeShader>();
    if (!m_gatherShader->CreateFromFile("shaders/surfel_indirect_diffuse_gather_comp.glsl")) {
        std::cerr << "[SurfelIndirectDiffusePass] Failed to compile gather shader" << std::endl;
        return false;
    }

    Resize(context, context.width, context.height);
    std::cout << "[SurfelIndirectDiffusePass] Initialized at half resolution" << std::endl;
    return true;
}

void SurfelIndirectDiffusePass::Resize(RenderContext&, int newWidth, int newHeight)
{
    const int internalWidth = std::max(1, (newWidth + kGatherResolutionDivisor - 1) / kGatherResolutionDivisor);
    const int internalHeight = std::max(1, (newHeight + kGatherResolutionDivisor - 1) / kGatherResolutionDivisor);
    if (internalWidth == m_width && internalHeight == m_height && m_irradiance && m_history && m_debug) {
        return;
    }

    AllocateTextures(internalWidth, internalHeight);
}

void SurfelIndirectDiffusePass::AllocateTextures(int width, int height)
{
    m_width = width;
    m_height = height;

    m_irradiance = Texture::Builder::Texture2D(m_width, m_height, GL_RGBA16F)
        .Format(GL_RGBA)
        .DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    m_history = Texture::Builder::Texture2D(m_width, m_height, GL_RGBA16F)
        .Format(GL_RGBA)
        .DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    m_debug = Texture::Builder::Texture2D(m_width, m_height, GL_RGBA16F)
        .Format(GL_RGBA)
        .DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    m_hasHistory = false;
}

void SurfelIndirectDiffusePass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>&,
    const std::shared_ptr<Camera>& camera,
    const std::shared_ptr<DirectionalLight>&,
    const std::shared_ptr<Skybox>&)
{
    Resize(ctx, ctx.width, ctx.height);

    const bool ready =
        ctx.enableSurfelGI &&
        ctx.enableSurfelIndirectDiffuse &&
        ctx.surfelGIGridReady &&
        ctx.gbufferFBO &&
        camera &&
        m_gatherShader &&
        m_gatherShader->IsValid() &&
        m_irradiance &&
        m_debug &&
        ctx.surfelGISurfelBuffer != 0 &&
        ctx.surfelGIHeaderBuffer != 0 &&
        ctx.surfelGIGridHeaderBuffer != 0 &&
        ctx.surfelGIGridEntryBuffer != 0 &&
        ctx.surfelGIGridAverageBuffer != 0 &&
        ctx.surfelGIIrradianceHeaderBuffer != 0 &&
        ctx.surfelGIRadialDepthBinsBuffer != 0 &&
        ctx.surfelGIWinnerIDTexture != 0;

    if (!ready) {
        const GLfloat clearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (m_irradiance) {
            glClearTexImage(m_irradiance->ID(), 0, GL_RGBA, GL_FLOAT, clearValue);
        }
        if (m_debug) {
            glClearTexImage(m_debug->ID(), 0, GL_RGBA, GL_FLOAT, clearValue);
        }
        if (m_history) {
            glClearTexImage(m_history->ID(), 0, GL_RGBA, GL_FLOAT, clearValue);
        }
        m_hasHistory = false;
        return;
    }

    m_config.neighborRadius = std::clamp(ctx.surfelIndirectDiffuseNeighborRadius, 0, 2);
    const bool validationMode =
        ctx.surfelGIDebug.forceRayBootstrap ||
        ctx.surfelGIDebug.validationMode != RenderContext::SurfelGIDebugSettings::Production;
    const int productionCandidateCap = validationMode ? 256 : 32;
    const int productionAcceptedCap = validationMode ? 64 : 10;
    m_config.maxCandidates = std::clamp(ctx.surfelIndirectDiffuseMaxCandidates, 8, productionCandidateCap);
    m_config.maxAccepted = std::clamp(ctx.surfelIndirectDiffuseMaxAccepted, 1, productionAcceptedCap);
    m_config.fallbackStrength = std::clamp(ctx.surfelIndirectDiffuseFallbackStrength, 0.0f, 1.0f);
    m_config.disableRadialDepthReject = ctx.surfelGIDebug.disableRadialDepthReject;
    m_config.disableNormalReject = ctx.surfelGIDebug.disableNormalReject;
    m_config.disableConfidenceReject = ctx.surfelGIDebug.disableConfidenceReject;
    m_config.disableFallback = ctx.surfelGIDebug.disableGatherFallback;

    glUseProgram(m_gatherShader->GetProgramID());

    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(1, ctx.gbufferFBO->GetColorAttachment(0));
    glBindTextureUnit(2, ctx.gbufferFBO->GetColorAttachment(1));
    glBindTextureUnit(3, ctx.gbufferFBO->GetColorAttachment(2));
    glBindTextureUnit(4, ctx.gbufferFBO->GetColorAttachment(4));
    glBindTextureUnit(5, ctx.gbufferFBO->GetColorAttachment(5));
    glBindTextureUnit(6, ctx.gbufferFBO->GetColorAttachment(7));
    glBindTextureUnit(kWinnerSurfelTextureUnit, ctx.surfelGIWinnerIDTexture);
    glBindTextureUnit(kHistoryTextureUnit, m_history ? m_history->ID() : 0);
    glBindTextureUnit(kVelocityTextureUnit, ctx.velocityTex);

    glBindImageTexture(0, m_irradiance->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_debug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingSurfels, ctx.surfelGISurfelBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingHeader, ctx.surfelGIHeaderBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingGridHeaders, ctx.surfelGIGridHeaderBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingGridEntries, ctx.surfelGIGridEntryBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingIrradianceHeader, ctx.surfelGIIrradianceHeaderBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingRadialDepth, ctx.surfelGIRadialDepthBinsBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingGridAverages, ctx.surfelGIGridAverageBuffer);

    const glm::mat4 invProjection = glm::inverse(ctx.proj);
    const glm::mat4 invView = glm::inverse(ctx.view);
    m_gatherShader->SetUniform("uInvProjection", invProjection);
    m_gatherShader->SetUniform("uInvView", invView);
    m_gatherShader->SetUniform("uView", ctx.view);
    m_gatherShader->SetUniform("uViewPos", camera->GetCameraPosition());
    m_gatherShader->SetUniform("uResolution", glm::vec2(static_cast<float>(m_width), static_cast<float>(m_height)));
    m_gatherShader->SetUniform("uNeighborRadius", m_config.neighborRadius);
    m_gatherShader->SetUniform("uMaxCandidates", m_config.maxCandidates);
    m_gatherShader->SetUniform("uMaxAccepted", m_config.maxAccepted);
    m_gatherShader->SetUniform("uFallbackStrength", m_config.fallbackStrength);
    m_gatherShader->SetUniform("uDebugMode", std::clamp(ctx.surfelIndirectDiffuseDebugMode, 0, 11));
    m_gatherShader->SetUniform("uDisableRadialDepthReject", m_config.disableRadialDepthReject ? 1 : 0);
    m_gatherShader->SetUniform("uDisableNormalReject", m_config.disableNormalReject ? 1 : 0);
    m_gatherShader->SetUniform("uDisableConfidenceReject", m_config.disableConfidenceReject ? 1 : 0);
    m_gatherShader->SetUniform("uDisableFallback", m_config.disableFallback ? 1 : 0);
    m_gatherShader->SetUniform("uUseTemporal", ctx.surfelIndirectDiffuseUseTemporal ? 1 : 0);
    m_gatherShader->SetUniform("uTemporalAlpha", 0.24f);
    m_gatherShader->SetUniform("uTemporalReset", m_hasHistory ? 0 : 1);
    m_gatherShader->SetUniform("uHasVelocityHistory", (ctx.velocityTex != 0 && m_hasHistory) ? 1 : 0);

    m_gatherShader->Dispatch(ComputeShader::CalculateWorkGroups(static_cast<GLuint>(m_width), 8u),
        ComputeShader::CalculateWorkGroups(static_cast<GLuint>(m_height), 8u), 1u);
    m_gatherShader->WaitForCompletion(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    if (m_history && m_irradiance) {
        glCopyImageSubData(m_irradiance->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_history->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_width, m_height, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        m_hasHistory = true;
    }
}
