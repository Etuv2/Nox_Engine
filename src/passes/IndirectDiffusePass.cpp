#include "IndirectDiffusePass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../ComputeShader.h"
#include "../Texture.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <vector>

IndirectDiffusePass::~IndirectDiffusePass() = default;

bool IndirectDiffusePass::Initialize(RenderContext& ctx) {
    std::cout << "[IndirectDiffusePass] Initializing visibility-bitmask indirect diffuse pipeline..." << std::endl;

    m_csDepthPrefilter = std::make_unique<ComputeShader>();
    m_csDepthPyramid = std::make_unique<ComputeShader>();
    m_csNormalBuild = std::make_unique<ComputeShader>();
    m_csRadiance = std::make_unique<ComputeShader>();
    m_csHorizonGather = std::make_unique<ComputeShader>();
    m_csTemporal = std::make_unique<ComputeShader>();
    m_csBilateral = std::make_unique<ComputeShader>();
    m_csFinalUpsample = std::make_unique<ComputeShader>();

    if (!m_csDepthPrefilter->CreateFromFile("shaders/indirect_diffuse_depth_prepare_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_depth_prepare_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csDepthPyramid->CreateFromFile("shaders/indirect_diffuse_depth_pyramid_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_depth_pyramid_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csNormalBuild->CreateFromFile("shaders/indirect_diffuse_full_normals_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_full_normals_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csRadiance->CreateFromFile("shaders/indirect_diffuse_radiance_downsample_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_radiance_downsample_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csHorizonGather->CreateFromFile("shaders/indirect_diffuse_gather_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_gather_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csTemporal->CreateFromFile("shaders/indirect_diffuse_temporal_resolve_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_temporal_resolve_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csBilateral->CreateFromFile("shaders/indirect_diffuse_denoise_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_denoise_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csFinalUpsample->CreateFromFile("shaders/indirect_diffuse_upsample_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_upsample_comp.glsl" << std::endl;
        return false;
    }

    Resize(ctx, ctx.width, ctx.height);
    std::cout << "[IndirectDiffusePass] Visibility-bitmask indirect diffuse initialization complete" << std::endl;
    return true;
}

void IndirectDiffusePass::Resize(RenderContext& ctx, int w, int h) {
    const float quarterScale = std::clamp(m_config.workingScale, 0.25f, 0.25f);

    const int desiredQuarterWidth = std::max(1, static_cast<int>(std::round(w * quarterScale)));
    const int desiredQuarterHeight = std::max(1, static_cast<int>(std::round(h * quarterScale)));

    if (w == m_w && h == m_h && desiredQuarterWidth == m_qw && desiredQuarterHeight == m_qh) {
        return;
    }

    m_w = w;
    m_h = h;
    m_qw = desiredQuarterWidth;
    m_qh = desiredQuarterHeight;
    m_depthMipCount = std::max(1, static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(m_qw, m_qh))))) + 1);

    std::cout << "[IndirectDiffusePass] Resize " << m_w << "x" << m_h
              << " (quarter: " << m_qw << "x" << m_qh
              << ", depth mips: " << m_depthMipCount << ")" << std::endl;

    m_depthLinearQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_R16F)
        .Format(GL_RED).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .GenerateMipmaps(true)
        .Build();

    m_normalQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_RG16F)
        .Format(GL_RG).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    m_bounceableRadianceQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_RGBA16F)
        .Format(GL_RGBA).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .GenerateMipmaps(true)
        .Build();

    m_indirectRaw = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalRaw = TextureFactory::CreateHDR(m_qw, m_qh);
    m_horizonDebug = TextureFactory::CreateHDR(m_qw, m_qh);
    m_sectorDebug = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectTemporal = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalTemporal = TextureFactory::CreateHDR(m_qw, m_qh);
    m_temporalDebug = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectDenoiseStage1 = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalDenoiseStage1 = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectDenoised = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalDenoised = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectDiffuseTex = TextureFactory::CreateHDR(m_w, m_h);
    m_debugOutput = TextureFactory::CreateHDR(m_w, m_h);

    m_historyResolvedGI = TextureFactory::CreateHDR(m_w, m_h);
    m_historyIndirect = TextureFactory::CreateHDR(m_qw, m_qh);
    m_historyDirectional = TextureFactory::CreateHDR(m_qw, m_qh);
    m_historyDepthQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_R16F)
        .Format(GL_RED).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .GenerateMipmaps(true)
        .Build();
    m_historyNormalFull = Texture::Builder::Texture2D(m_qw, m_qh, GL_RG16F)
        .Format(GL_RG).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    std::vector<float> clearQuarter(static_cast<size_t>(m_qw) * static_cast<size_t>(m_qh) * 4, 0.0f);
    m_historyIndirect->Upload2D(0, 0, 0, m_qw, m_qh, GL_RGBA, GL_FLOAT, clearQuarter.data());
    m_historyDirectional->Upload2D(0, 0, 0, m_qw, m_qh, GL_RGBA, GL_FLOAT, clearQuarter.data());
    std::vector<float> clearFull(static_cast<size_t>(m_w) * static_cast<size_t>(m_h) * 4, 0.0f);
    m_historyResolvedGI->Upload2D(0, 0, 0, m_w, m_h, GL_RGBA, GL_FLOAT, clearFull.data());

    const float invalidLinearDepth = 65504.0f;
    glClearTexImage(m_historyDepthQuarter->ID(), 0, GL_RED, GL_FLOAT, &invalidLinearDepth);

    const float normalClear[4] = { 0.5f, 0.5f, 0.0f, 0.0f };
    glClearTexImage(m_historyNormalFull->ID(), 0, GL_RG, GL_FLOAT, normalClear);
    m_historyDepthQuarter->GenerateMipmaps();
}

void IndirectDiffusePass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>&,
    const std::shared_ptr<Camera>& camera,
    const std::shared_ptr<DirectionalLight>&,
    const std::shared_ptr<Skybox>&) {
    if (!ctx.enableIndirectDiffuse || !ctx.gbufferFBO || !camera || m_bounceableRadianceFull == 0) {
        return;
    }

    Resize(ctx, ctx.width, ctx.height);

    runDepthPrefilter(ctx);
    runRadiance(ctx);
    runHorizonGather(ctx, camera);
    runTemporal(ctx);

    runBilateral(ctx);

    runFinalUpsample(ctx);

    glCopyImageSubData(m_indirectDiffuseTex->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyResolvedGI->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_w, m_h, 1);
    ctx.indirectDiffuseHistoryReset = false;
}

void IndirectDiffusePass::runDepthPrefilter(RenderContext& ctx) {
    glUseProgram(m_csDepthPrefilter->GetProgramID());

    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture());
    glBindImageTexture(2, m_depthLinearQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glUniformMatrix4fv(glGetUniformLocation(m_csDepthPrefilter->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    for (int mip = 0; mip < m_depthMipCount - 1; ++mip) {
        const int dstWidth = std::max(1, m_qw >> (mip + 1));
        const int dstHeight = std::max(1, m_qh >> (mip + 1));

        glUseProgram(m_csDepthPyramid->GetProgramID());
        glBindTextureUnit(0, m_depthLinearQuarter->ID());
        glBindImageTexture(1, m_depthLinearQuarter->ID(), mip + 1, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glUniform1i(glGetUniformLocation(m_csDepthPyramid->GetProgramID(), "srcMip"), mip);

        const GLuint mipGX = (dstWidth + 7) / 8;
        const GLuint mipGY = (dstHeight + 7) / 8;
        glDispatchCompute(mipGX, mipGY, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    glUseProgram(m_csNormalBuild->GetProgramID());
    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture());
    glBindImageTexture(1, m_normalQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glUniformMatrix4fv(glGetUniformLocation(m_csNormalBuild->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void IndirectDiffusePass::runRadiance(RenderContext& ctx) {
    glUseProgram(m_csRadiance->GetProgramID());

    glBindTextureUnit(0, m_bounceableRadianceFull);
    glBindTextureUnit(1, ctx.gbufferFBO->GetColorAttachment(4));
    glBindTextureUnit(2, m_historyResolvedGI->ID());
    glBindTextureUnit(3, m_depthLinearQuarter->ID());
    glBindTextureUnit(4, ctx.velocityTex);
    glBindTextureUnit(5, m_historyDepthQuarter->ID());
    glBindTextureUnit(6, m_historyNormalFull->ID());
    glBindTextureUnit(7, m_normalQuarter->ID());
    glBindTextureUnit(8, ctx.gbufferFBO->GetDepthTexture());
    glBindImageTexture(0, m_bounceableRadianceQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform1i(glGetUniformLocation(m_csRadiance->GetProgramID(), "usePreviousIndirect"),
        (ctx.velocityTex != 0 && m_historyDepthQuarter && m_historyNormalFull && !ctx.indirectDiffuseHistoryReset) ? 1 : 0);
    // Keep the optional multi-bounce reinjection disabled while validating the
    // core visibility-bitmask pipeline. Feeding previous indirect back into the
    // source buffer is useful later, but right now it contaminates stage color
    // debugging and makes it much harder to judge whether gather/temporal/denoise
    // are correct on their own.
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "previousIndirectFeedback"), 0.0f);
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "depthReject"), std::clamp(ctx.indirectDiffuseDepthReject, 0.01f, 0.35f));
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "normalRejectCos"), std::clamp(1.0f - ctx.indirectDiffuseNormalReject, 0.55f, 0.99f));
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "disocclusionReject"), std::clamp(ctx.indirectDiffuseDepthReject * 1.5f, 0.02f, 0.25f));
    glUniformMatrix4fv(glGetUniformLocation(m_csRadiance->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    m_bounceableRadianceQuarter->GenerateMipmaps();
}

void IndirectDiffusePass::runHorizonGather(RenderContext& ctx, const std::shared_ptr<Camera>& camera) {
    (void)camera;
    glUseProgram(m_csHorizonGather->GetProgramID());

    glBindTextureUnit(0, m_depthLinearQuarter->ID());
    glBindTextureUnit(1, m_normalQuarter->ID());
    glBindTextureUnit(2, m_bounceableRadianceQuarter->ID());

    glBindImageTexture(3, m_indirectRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(4, m_directionalRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(5, m_horizonDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(6, m_sectorDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    const int sliceCount = std::clamp(ctx.indirectDiffuseSliceCount, 1, 8);
    const int stepCount = std::clamp(ctx.indirectDiffuseSamplesPerSlice, 1, 16);

    glUniform2f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "invQuarterSize"),
        1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform2f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "fullResolution"), float(m_w), float(m_h));
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "projScaleX"), ctx.proj[0][0]);
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "projScaleY"), ctx.proj[1][1]);
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "rayLength"), std::max(0.05f, ctx.indirectDiffuseRadiusVS));
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "thicknessVS"), std::max(0.001f, ctx.indirectDiffuseThicknessVS));
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "rayCount"), sliceCount);
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "stepCount"), stepCount);
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "frameIndex"), m_frameIndex++);

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void IndirectDiffusePass::runTemporal(RenderContext& ctx) {
    if (ctx.velocityTex == 0 || !m_historyNormalFull || !m_historyDepthQuarter || ctx.indirectDiffuseHistoryReset) {
        glCopyImageSubData(m_indirectRaw->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        glCopyImageSubData(m_directionalRaw->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);

        glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyIndirect->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyDirectional->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);

        const GLfloat temporalClear[4] = { 1.0f, 0.0f, 1.0f, 0.0f };
        glClearTexImage(m_temporalDebug->ID(), 0, GL_RGBA, GL_FLOAT, temporalClear);

        glCopyImageSubData(m_depthLinearQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyDepthQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        m_historyDepthQuarter->GenerateMipmaps();

        glCopyImageSubData(m_normalQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyNormalFull->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        return;
    }

    glUseProgram(m_csTemporal->GetProgramID());

    glBindTextureUnit(0, m_indirectRaw->ID());
    glBindTextureUnit(1, m_historyIndirect->ID());
    glBindTextureUnit(2, m_directionalRaw->ID());
    glBindTextureUnit(3, m_historyDirectional->ID());
    glBindTextureUnit(4, ctx.velocityTex);
    glBindTextureUnit(5, m_depthLinearQuarter->ID());
    glBindTextureUnit(6, m_normalQuarter->ID());
    glBindTextureUnit(7, m_historyDepthQuarter->ID());
    glBindTextureUnit(8, m_historyNormalFull->ID());

    glBindImageTexture(0, m_indirectTemporal->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_directionalTemporal->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(2, m_temporalDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform1i(glGetUniformLocation(m_csTemporal->GetProgramID(), "useHistory"), 1);
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "historyBlend"), std::clamp(1.0f - ctx.indirectDiffuseTemporalAlpha, 0.0f, 0.95f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "depthReject"), std::clamp(ctx.indirectDiffuseDepthReject, 0.01f, 0.35f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "normalRejectCos"), std::clamp(1.0f - ctx.indirectDiffuseNormalReject, 0.55f, 0.99f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "disocclusionReject"), std::clamp(ctx.indirectDiffuseDepthReject * 1.5f, 0.02f, 0.25f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "historyClampStrength"), 0.35f);
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "motionRejectPixels"), 32.0f);
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "minHistoryConfidence"), 0.01f);

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyIndirect->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
    glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyDirectional->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
    glCopyImageSubData(m_depthLinearQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyDepthQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
    m_historyDepthQuarter->GenerateMipmaps();

    glCopyImageSubData(m_normalQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyNormalFull->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
}

void IndirectDiffusePass::runBilateral(RenderContext& ctx) {
    // Temporarily bypass the denoiser to keep the pipeline in a trustworthy
    // validation state. Recent color regressions made Denoise1/Denoise2 an
    // unreliable debugging stage, so keep stage ownership intact while copying
    // temporal outputs straight through.
    glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_indirectDenoiseStage1->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
    glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_directionalDenoiseStage1->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);

    glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_indirectDenoised->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
    glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_directionalDenoised->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
}

void IndirectDiffusePass::runFinalUpsample(RenderContext& ctx) {
    glUseProgram(m_csFinalUpsample->GetProgramID());

    glBindTextureUnit(0, m_indirectDenoiseStage1->ID());
    glBindTextureUnit(1, m_directionalDenoiseStage1->ID());
    glBindTextureUnit(2, m_indirectDenoised->ID());
    glBindTextureUnit(3, m_directionalDenoised->ID());
    glBindTextureUnit(4, m_indirectRaw->ID());
    glBindTextureUnit(5, m_directionalRaw->ID());
    glBindTextureUnit(6, m_depthLinearQuarter->ID());
    glBindTextureUnit(7, m_normalQuarter->ID());
    glBindTextureUnit(8, m_bounceableRadianceQuarter->ID());
    glBindTextureUnit(9, m_horizonDebug->ID());
    glBindTextureUnit(10, m_temporalDebug->ID());
    glBindTextureUnit(11, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(12, ctx.gbufferFBO->GetColorAttachment(0));
    glBindTextureUnit(13, ctx.gbufferFBO->GetColorAttachment(1));
    glBindTextureUnit(14, m_sectorDebug->ID());
    glBindTextureUnit(15, m_indirectTemporal->ID());

    glBindImageTexture(0, m_indirectDiffuseTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_debugOutput->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform2f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invFullSize"), 1.0f / float(m_w), 1.0f / float(m_h));
    glUniform2f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invQuarterSize"), 1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "projScaleX"), ctx.proj[0][0]);
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "projScaleY"), ctx.proj[1][1]);
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "upscaleSharpness"), std::clamp(ctx.indirectDiffuseUpscaleSharpness, 0.5f, 4.0f));
    glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "debugMode"), std::clamp(ctx.indirectDiffuseDebugStage, 0, 16));
    glUniformMatrix4fv(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "view"), 1, GL_FALSE, glm::value_ptr(ctx.view));
    glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "normalsInWorldSpace"), 1);
    glUniformMatrix4fv(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    const GLuint gx = (m_w + 7) / 8;
    const GLuint gy = (m_h + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}
