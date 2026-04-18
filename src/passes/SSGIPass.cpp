#include "SSGIPass.h"
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

SSGIPass::~SSGIPass() {
    if (m_historyDepthTex != 0) {
        glDeleteTextures(1, &m_historyDepthTex);
        m_historyDepthTex = 0;
    }
    if (m_historyNormalTex != 0) {
        glDeleteTextures(1, &m_historyNormalTex);
        m_historyNormalTex = 0;
    }
}

void SSGIPass::resizeTemporalHistoryBuffers() {
    if (m_historyDepthTex != 0) {
        glDeleteTextures(1, &m_historyDepthTex);
        m_historyDepthTex = 0;
    }
    if (m_historyNormalTex != 0) {
        glDeleteTextures(1, &m_historyNormalTex);
        m_historyNormalTex = 0;
    }

    glGenTextures(1, &m_historyDepthTex);
    glBindTexture(GL_TEXTURE_2D, m_historyDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_w, m_h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    float depthClear = 1.0f;
    glClearTexImage(m_historyDepthTex, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &depthClear);

    glGenTextures(1, &m_historyNormalTex);
    glBindTexture(GL_TEXTURE_2D, m_historyNormalTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_w, m_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLint normalClear[4] = { 0, 0, 0, 0 };
    glClearTexImage(m_historyNormalTex, 0, GL_RGBA, GL_INT, normalClear);

    glBindTexture(GL_TEXTURE_2D, 0);
}

bool SSGIPass::Initialize(RenderContext& ctx) {
    std::cout << "[SSGIPass] Initializing horizon-based SSGI pipeline..." << std::endl;

    m_csDepthPrefilter = std::make_unique<ComputeShader>();
    m_csRadiance = std::make_unique<ComputeShader>();
    m_csHorizonGather = std::make_unique<ComputeShader>();
    m_csTemporal = std::make_unique<ComputeShader>();
    m_csBilateral = std::make_unique<ComputeShader>();
    m_csFinalUpsample = std::make_unique<ComputeShader>();

    if (!m_csDepthPrefilter->CreateFromFile("shaders/ssgi_downsample2x_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_downsample2x_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csRadiance->CreateFromFile("shaders/ssgi_radiance_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_radiance_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csHorizonGather->CreateFromFile("shaders/ssgi_raymarch_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_raymarch_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csTemporal->CreateFromFile("shaders/ssgi_temporal_resolve_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_temporal_resolve_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csBilateral->CreateFromFile("shaders/ssgi_bilateral_blur_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_bilateral_blur_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csFinalUpsample->CreateFromFile("shaders/ssgi_final_upsample_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_final_upsample_comp.glsl" << std::endl;
        return false;
    }

    Resize(ctx, ctx.width, ctx.height);
    std::cout << "[SSGIPass] Horizon-based SSGI initialization complete" << std::endl;
    return true;
}

void SSGIPass::Resize(RenderContext& ctx, int w, int h) {
    float quarterScale = std::clamp(ctx.ssgiTraceResolutionScale, 0.125f, 0.5f);
    if (!ctx.ssgiHalfRes) {
        quarterScale = std::min(0.5f, quarterScale * 2.0f);
    }

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

    std::cout << "[SSGIPass] Resize " << m_w << "x" << m_h
              << " (quarter: " << m_qw << "x" << m_qh
              << ", depth mips: " << m_depthMipCount << ")" << std::endl;

    m_depthLinearQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_R16F)
        .Format(GL_RED).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .GenerateMipmaps(true)
        .Build();

    m_normalQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_RG16F)
        .Format(GL_RG).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    m_radianceTex = Texture::Builder::Texture2D(m_qw, m_qh, GL_RGBA16F)
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

    m_indirectDenoised = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalDenoised = TextureFactory::CreateHDR(m_qw, m_qh);

    m_ssgiTex = TextureFactory::CreateHDR(m_w, m_h);
    m_debugOutput = TextureFactory::CreateHDR(m_w, m_h);

    m_historyColor = TextureFactory::CreateHDR(m_qw, m_qh);
    m_historyIndirect = TextureFactory::CreateHDR(m_qw, m_qh);
    m_historyDirectional = TextureFactory::CreateHDR(m_qw, m_qh);

    resizeTemporalHistoryBuffers();

    std::vector<float> clearQuarter(static_cast<size_t>(m_qw) * static_cast<size_t>(m_qh) * 4, 0.0f);
    m_historyColor->Upload2D(0, 0, 0, m_qw, m_qh, GL_RGBA, GL_FLOAT, clearQuarter.data());
    m_historyIndirect->Upload2D(0, 0, 0, m_qw, m_qh, GL_RGBA, GL_FLOAT, clearQuarter.data());
    m_historyDirectional->Upload2D(0, 0, 0, m_qw, m_qh, GL_RGBA, GL_FLOAT, clearQuarter.data());
}

void SSGIPass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>&,
    const std::shared_ptr<Camera>& camera,
    const std::shared_ptr<DirectionalLight>&,
    const std::shared_ptr<Skybox>&) {
    if (!ctx.enableSSGI || !ctx.gbufferFBO || !camera) {
        return;
    }

    Resize(ctx, ctx.width, ctx.height);

    runDepthPrefilter(ctx);
    runRadiance(ctx);
    runHorizonGather(ctx, camera);
    runTemporal(ctx);

    if (ctx.ssgiEnableSpatialDenoise) {
        runBilateral(ctx);
    }
    else {
        glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_indirectDenoised->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_directionalDenoised->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
    }

    runFinalUpsample(ctx);
}

void SSGIPass::runDepthPrefilter(RenderContext& ctx) {
    glUseProgram(m_csDepthPrefilter->GetProgramID());

    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(1, ctx.gbufferFBO->GetColorAttachment(0));
    glBindImageTexture(2, m_depthLinearQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glBindImageTexture(3, m_normalQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);

    glUniform2f(glGetUniformLocation(m_csDepthPrefilter->GetProgramID(), "invQuarterSize"),
        1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniformMatrix4fv(glGetUniformLocation(m_csDepthPrefilter->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    m_depthLinearQuarter->GenerateMipmaps();
}

void SSGIPass::runRadiance(RenderContext& ctx) {
    glUseProgram(m_csRadiance->GetProgramID());

    glBindTextureUnit(0, ctx.gbufferFBO->GetColorAttachment(1)); // albedo + AO
    glBindTextureUnit(1, ctx.gbufferFBO->GetColorAttachment(2)); // specular F0 + emissive strength
    glBindTextureUnit(2, ctx.gbufferFBO->GetColorAttachment(4)); // emissive color
    glBindTextureUnit(3, m_historyColor->ID());
    glBindTextureUnit(4, m_depthLinearQuarter->ID());
    glBindImageTexture(5, m_radianceTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform2f(glGetUniformLocation(m_csRadiance->GetProgramID(), "invRadianceSize"),
        1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform3f(glGetUniformLocation(m_csRadiance->GetProgramID(), "envColor"),
        ctx.envColor.r, ctx.envColor.g, ctx.envColor.b);

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    m_radianceTex->GenerateMipmaps();
}

void SSGIPass::runHorizonGather(RenderContext& ctx, const std::shared_ptr<Camera>& camera) {
    glUseProgram(m_csHorizonGather->GetProgramID());

    glBindTextureUnit(0, m_depthLinearQuarter->ID());
    glBindTextureUnit(1, m_normalQuarter->ID());
    glBindTextureUnit(2, m_radianceTex->ID());

    glBindImageTexture(3, m_indirectRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(4, m_directionalRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(5, m_horizonDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(6, m_sectorDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    const int sampleBudget = std::clamp(ctx.ssgiSampleCount, 16, 512);
    const int sliceCount = std::clamp(sampleBudget / 10, 4, 20);
    const int stepsPerSlice = std::clamp(sampleBudget / std::max(sliceCount, 1), 4, 40);

    glUniform2f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "invQuarterSize"),
        1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "projScaleX"), ctx.proj[0][0]);
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "projScaleY"), ctx.proj[1][1]);
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "maxRadiusVS"), std::max(0.05f, ctx.ssgiRadius));
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "thicknessVS"), std::max(0.001f, ctx.ssgiThickness));
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "sliceCount"), sliceCount);
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "stepsPerSlice"), stepsPerSlice);
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "sectorCount"), std::clamp(ctx.ssgiSectorCount, 8, 24));
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "frameIndex"), m_frameIndex++);
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "cameraNear"), camera->GetCameraNearPlane());
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "cameraFar"), camera->GetCameraFarPlane());

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void SSGIPass::runTemporal(RenderContext& ctx) {
    if (ctx.velocityTex == 0 || m_historyDepthTex == 0 || m_historyNormalTex == 0) {
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

        glCopyImageSubData(ctx.gbufferFBO->GetDepthTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyDepthTex, GL_TEXTURE_2D, 0, 0, 0, 0,
            m_w, m_h, 1);
        glCopyImageSubData(ctx.gbufferFBO->GetColorAttachment(0), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyNormalTex, GL_TEXTURE_2D, 0, 0, 0, 0,
            m_w, m_h, 1);
        return;
    }

    glUseProgram(m_csTemporal->GetProgramID());

    glBindTextureUnit(0, m_indirectRaw->ID());
    glBindTextureUnit(1, m_historyIndirect->ID());
    glBindTextureUnit(2, m_directionalRaw->ID());
    glBindTextureUnit(3, m_historyDirectional->ID());
    glBindTextureUnit(4, ctx.velocityTex);
    glBindTextureUnit(5, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(6, ctx.gbufferFBO->GetColorAttachment(0));
    glBindTextureUnit(7, m_historyDepthTex);
    glBindTextureUnit(8, m_historyNormalTex);

    glBindImageTexture(9, m_indirectTemporal->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(10, m_directionalTemporal->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(11, m_temporalDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "alpha"), std::clamp(ctx.ssgiTemporalAlpha, 0.02f, 0.35f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "depthThreshold"), std::clamp(ctx.ssgiDepthReject * 0.01f, 0.0002f, 0.02f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "normalThreshold"), std::clamp(1.0f - ctx.ssgiNormalReject, 0.65f, 0.99f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "temporalResponse"), std::clamp(ctx.ssgiTemporalResponse, 0.0f, 1.0f));

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

    glCopyImageSubData(ctx.gbufferFBO->GetDepthTexture(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyDepthTex, GL_TEXTURE_2D, 0, 0, 0, 0,
        m_w, m_h, 1);
    glCopyImageSubData(ctx.gbufferFBO->GetColorAttachment(0), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyNormalTex, GL_TEXTURE_2D, 0, 0, 0, 0,
        m_w, m_h, 1);
}

void SSGIPass::runBilateral(RenderContext& ctx) {
    glUseProgram(m_csBilateral->GetProgramID());

    glBindTextureUnit(0, m_indirectTemporal->ID());
    glBindTextureUnit(1, m_directionalTemporal->ID());
    glBindTextureUnit(2, m_depthLinearQuarter->ID());
    glBindTextureUnit(3, m_normalQuarter->ID());

    glBindImageTexture(4, m_indirectDenoised->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(5, m_directionalDenoised->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform2f(glGetUniformLocation(m_csBilateral->GetProgramID(), "invQuarterSize"), 1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "depthSigma"), std::max(0.01f, ctx.ssgiDepthReject));
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "normalReject"), std::max(0.05f, ctx.ssgiNormalReject));

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void SSGIPass::runFinalUpsample(RenderContext& ctx) {
    glUseProgram(m_csFinalUpsample->GetProgramID());

    glBindTextureUnit(0, m_indirectDenoised->ID());
    glBindTextureUnit(1, m_directionalDenoised->ID());
    glBindTextureUnit(2, m_depthLinearQuarter->ID());
    glBindTextureUnit(3, m_horizonDebug->ID());
    glBindTextureUnit(4, m_sectorDebug->ID());
    glBindTextureUnit(5, m_temporalDebug->ID());
    glBindTextureUnit(6, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(7, ctx.gbufferFBO->GetColorAttachment(0));

    glBindImageTexture(8, m_ssgiTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform2f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invFullSize"), 1.0f / float(m_w), 1.0f / float(m_h));
    glUniform2f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invQuarterSize"), 1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "projScaleX"), ctx.proj[0][0]);
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "projScaleY"), ctx.proj[1][1]);
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "upscaleSharpness"), std::clamp(ctx.ssgiUpscaleSharpness, 0.5f, 4.0f));
    glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "debugMode"), std::clamp(ctx.ssgiDebugMode, 0, 8));

    const GLuint gx = (m_w + 7) / 8;
    const GLuint gy = (m_h + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void SSGIPass::CaptureHistory(RenderContext& ctx) {
    FrameBuffer* sourceFBO = (ctx.taaFBO && ctx.enableTAA) ? ctx.taaFBO : ctx.hdrFBO.get();
    if (!sourceFBO || !m_historyColor) {
        return;
    }

    GLuint tempFBO = 0;
    glGenFramebuffers(1, &tempFBO);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFBO->GetFBO());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tempFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_historyColor->ID(), 0);

    glBlitFramebuffer(0, 0, ctx.width, ctx.height,
        0, 0, m_qw, m_qh,
        GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &tempFBO);
}
