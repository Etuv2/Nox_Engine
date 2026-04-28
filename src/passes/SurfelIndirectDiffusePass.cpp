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
constexpr GLuint kSurfelBindingRadialDepth = 30;
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
    std::cout << "[SurfelIndirectDiffusePass] Initialized at full resolution" << std::endl;
    return true;
}

void SurfelIndirectDiffusePass::Resize(RenderContext&, int newWidth, int newHeight)
{
    if (newWidth == m_width && newHeight == m_height && m_irradiance && m_debug) {
        return;
    }

    AllocateTextures(std::max(1, newWidth), std::max(1, newHeight));
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

    m_debug = Texture::Builder::Texture2D(m_width, m_height, GL_RGBA16F)
        .Format(GL_RGBA)
        .DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();
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
        ctx.surfelGIRadialDepthBinsBuffer != 0;

    if (!ready) {
        const GLfloat clearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (m_irradiance) {
            glClearTexImage(m_irradiance->ID(), 0, GL_RGBA, GL_FLOAT, clearValue);
        }
        if (m_debug) {
            glClearTexImage(m_debug->ID(), 0, GL_RGBA, GL_FLOAT, clearValue);
        }
        return;
    }

    m_config.neighborRadius = std::clamp(ctx.surfelIndirectDiffuseNeighborRadius, 0, 2);
    m_config.maxCandidates = std::clamp(ctx.surfelIndirectDiffuseMaxCandidates, 8, 256);
    m_config.maxAccepted = std::clamp(ctx.surfelIndirectDiffuseMaxAccepted, 1, 64);
    m_config.fallbackStrength = std::clamp(ctx.surfelIndirectDiffuseFallbackStrength, 0.0f, 1.0f);

    glUseProgram(m_gatherShader->GetProgramID());

    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(1, ctx.gbufferFBO->GetColorAttachment(0));
    glBindTextureUnit(2, ctx.gbufferFBO->GetColorAttachment(1));
    glBindTextureUnit(3, ctx.gbufferFBO->GetColorAttachment(2));
    glBindTextureUnit(4, ctx.gbufferFBO->GetColorAttachment(4));
    glBindTextureUnit(5, ctx.gbufferFBO->GetColorAttachment(5));
    glBindTextureUnit(6, ctx.gbufferFBO->GetColorAttachment(7));

    glBindImageTexture(0, m_irradiance->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_debug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingSurfels, ctx.surfelGISurfelBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingHeader, ctx.surfelGIHeaderBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingGridHeaders, ctx.surfelGIGridHeaderBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingGridEntries, ctx.surfelGIGridEntryBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kSurfelBindingRadialDepth, ctx.surfelGIRadialDepthBinsBuffer);

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
    m_gatherShader->SetUniform("uDebugMode", std::clamp(ctx.surfelIndirectDiffuseDebugMode, 0, 10));

    m_gatherShader->Dispatch(ComputeShader::CalculateWorkGroups(static_cast<GLuint>(m_width), 8u),
        ComputeShader::CalculateWorkGroups(static_cast<GLuint>(m_height), 8u), 1u);
    m_gatherShader->WaitForCompletion(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
}
