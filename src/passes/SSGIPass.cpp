#include "SSGIPass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../ComputeShader.h"
#include "../Skybox.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <algorithm>
#include <cmath>

SSGIPass::~SSGIPass() = default;

bool SSGIPass::Initialize(RenderContext& ctx) {
    std::cout << "[SSGIPass] Initializing Screen Space Global Illumination..." << std::endl;

    m_csDirections = std::make_unique<ComputeShader>();
    m_csRaymarch = std::make_unique<ComputeShader>();
    m_csBilateral = std::make_unique<ComputeShader>();
    m_csUpsample = std::make_unique<ComputeShader>();
    m_csTemporal = std::make_unique<ComputeShader>();
    m_csFinalUpsample = std::make_unique<ComputeShader>();

    if (!m_csDirections->CreateFromFile("shaders/ssgi_directions_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_directions_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csRaymarch->CreateFromFile("shaders/ssgi_raymarch_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_raymarch_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csBilateral->CreateFromFile("shaders/ssgi_bilateral_blur_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_bilateral_blur_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csUpsample->CreateFromFile("shaders/ssgi_upsample2x_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_upsample2x_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csTemporal->CreateFromFile("shaders/ssgi_temporal_resolve_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_temporal_resolve_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csFinalUpsample->CreateFromFile("shaders/ssgi_final_upsample_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_final_upsample_comp.glsl" << std::endl;
        return false;
    }

    Resize(ctx, ctx.width, ctx.height);
    std::cout << "[SSGIPass] Initialization complete" << std::endl;
    return true;
}

void SSGIPass::Resize(RenderContext& ctx, int w, int h) {
    const float workingScale = ctx.ssgiHalfRes ? ctx.ssgiWorkingResolutionScale : 1.0f;
    const float raymarchScale = ctx.ssgiHalfRes ? ctx.ssgiTraceResolutionScale : workingScale;
    const int desiredWorkingWidth = std::max(1, static_cast<int>(std::round(w * workingScale)));
    const int desiredWorkingHeight = std::max(1, static_cast<int>(std::round(h * workingScale)));
    const int desiredRaymarchWidth = std::max(1, static_cast<int>(std::round(w * raymarchScale)));
    const int desiredRaymarchHeight = std::max(1, static_cast<int>(std::round(h * raymarchScale)));

    if (w == m_w && h == m_h &&
        desiredWorkingWidth == m_hw && desiredWorkingHeight == m_hh &&
        desiredRaymarchWidth == m_qw && desiredRaymarchHeight == m_qh) {
        return;
    }

    m_w = w;
    m_h = h;
    m_hw = desiredWorkingWidth;
    m_hh = desiredWorkingHeight;
    m_qw = desiredRaymarchWidth;
    m_qh = desiredRaymarchHeight;

    std::cout << "[SSGIPass] Resizing to " << w << "x" << h
              << " (raymarch: " << m_qw << "x" << m_qh
              << ", working: " << m_hw << "x" << m_hh << ")" << std::endl;

    m_dirTex = Texture::Builder::Texture2D(m_qw, m_qh, GL_RGBA16F)
        .Format(GL_RGBA).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_NEAREST, GL_NEAREST)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    m_ssgiRaw = TextureFactory::CreateHDR(m_qw, m_qh);
    m_ssgiQuarterBlur = TextureFactory::CreateHDR(m_qw, m_qh);
    m_ssgiBlur = TextureFactory::CreateHDR(m_hw, m_hh);
    m_ssgiWork = TextureFactory::CreateHDR(m_hw, m_hh);
    m_ssgiTex = TextureFactory::CreateHDR(m_w, m_h);
    m_historyColor = TextureFactory::CreateHDR(m_hw, m_hh);
    m_historySSGI = TextureFactory::CreateHDR(m_hw, m_hh);

    std::vector<float> blackPixels(static_cast<size_t>(m_hw) * static_cast<size_t>(m_hh) * 4, 0.0f);
    m_historyColor->Upload2D(0, 0, 0, m_hw, m_hh, GL_RGBA, GL_FLOAT, blackPixels.data());
    m_historySSGI->Upload2D(0, 0, 0, m_hw, m_hh, GL_RGBA, GL_FLOAT, blackPixels.data());
}

void SSGIPass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>&,
    const std::shared_ptr<Camera>& camera,
    const std::shared_ptr<DirectionalLight>&,
    const std::shared_ptr<Skybox>& skybox) {
    if (!ctx.enableSSGI || !ctx.gbufferFBO || !camera) return;
    Resize(ctx, ctx.width, ctx.height);

    runDirections(ctx);
    runRaymarch(ctx, camera, skybox);
    if (ctx.ssgiEnableSpatialDenoise) {
        runBilateral(ctx);
    } else if (m_ssgiQuarterBlur && m_ssgiRaw) {
        glCopyImageSubData(m_ssgiRaw->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_ssgiQuarterBlur->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
    }
    runUpsample(ctx);
    runTemporal(ctx);
    runFinalUpsample(ctx);
}

void SSGIPass::runDirections(RenderContext&) {
    glUseProgram(m_csDirections->GetProgramID());
    glBindImageTexture(0, m_dirTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform2f(glGetUniformLocation(m_csDirections->GetProgramID(), "invScreen"),
        1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform1i(glGetUniformLocation(m_csDirections->GetProgramID(), "frameIndex"), m_frameIndex++);

    GLuint gx = (m_qw + 7) / 8;
    GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUseProgram(0);
}

void SSGIPass::runRaymarch(RenderContext& ctx, const std::shared_ptr<Camera>& camera, const std::shared_ptr<Skybox>& skybox) {
    glUseProgram(m_csRaymarch->GetProgramID());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "gDepth"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "gPackedNormalRM"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(1));
    glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "gAlbedoAO"), 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_dirTex->ID());
    glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "randTex"), 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_historyColor->ID());
    glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "prevColor"), 4);

    GLuint ibl = 0;
    if (skybox) {
        ibl = skybox->GetIrradianceMap();
        if (ibl == 0) ibl = skybox->GetEnvironmentMap();
    }
    glActiveTexture(GL_TEXTURE5);
    if (ibl != 0) {
        glBindTexture(GL_TEXTURE_CUBE_MAP, ibl);
    }
    glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "iblIrradiance"), 5);

    glBindImageTexture(0, m_ssgiRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glm::mat4 invProj = glm::inverse(ctx.proj);
    glm::mat4 invView = glm::inverse(ctx.view);
    glUniformMatrix4fv(glGetUniformLocation(m_csRaymarch->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(invProj));
    glUniformMatrix4fv(glGetUniformLocation(m_csRaymarch->GetProgramID(), "invView"), 1, GL_FALSE, glm::value_ptr(invView));
    glUniformMatrix4fv(glGetUniformLocation(m_csRaymarch->GetProgramID(), "view"), 1, GL_FALSE, glm::value_ptr(ctx.view));
    glUniformMatrix4fv(glGetUniformLocation(m_csRaymarch->GetProgramID(), "proj"), 1, GL_FALSE, glm::value_ptr(ctx.proj));

    glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "maxRayLenVS"), ctx.ssgiRadius);
    glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "numSteps"), std::clamp(ctx.ssgiSampleCount, 8, m_config.maxRaySteps));
    glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "thickness"), ctx.ssgiThickness);
    glUniform2f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "screenSize"), float(m_w), float(m_h));
    glUniform2f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "workSize"), float(m_qw), float(m_qh));
    glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "cameraNear"), camera->GetCameraNearPlane());
    glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "cameraFar"), camera->GetCameraFarPlane());
    glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "iblFallbackStrength"), 1.0f);
    glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "hasIBL"), ibl != 0 ? 1 : 0);

    GLuint gx = (m_qw + 7) / 8;
    GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(0);
}

void SSGIPass::runBilateral(RenderContext& ctx) {
    glUseProgram(m_csBilateral->GetProgramID());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssgiRaw->ID());
    glUniform1i(glGetUniformLocation(m_csBilateral->GetProgramID(), "inTex"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glUniform1i(glGetUniformLocation(m_csBilateral->GetProgramID(), "gDepth"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_csBilateral->GetProgramID(), "gPackedNormalRM"), 2);

    glBindImageTexture(3, m_ssgiQuarterBlur->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "depthSigma"), std::max(0.01f, ctx.ssgiDepthReject));
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "normalThresh"), std::max(0.05f, ctx.ssgiNormalReject));
    glUniform2f(glGetUniformLocation(m_csBilateral->GetProgramID(), "invWork"), 1.0f / float(m_qw), 1.0f / float(m_qh));

    GLuint gx = (m_qw + 7) / 8;
    GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(0);
}

void SSGIPass::runUpsample(RenderContext& ctx) {
    glUseProgram(m_csUpsample->GetProgramID());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssgiQuarterBlur->ID());
    glUniform1i(glGetUniformLocation(m_csUpsample->GetProgramID(), "inLow"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glUniform1i(glGetUniformLocation(m_csUpsample->GetProgramID(), "depthTex"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_csUpsample->GetProgramID(), "normalTex"), 2);

    glBindImageTexture(3, m_ssgiBlur->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glUniform2f(glGetUniformLocation(m_csUpsample->GetProgramID(), "invDst"), 1.0f / float(m_hw), 1.0f / float(m_hh));
    glUniform1f(glGetUniformLocation(m_csUpsample->GetProgramID(), "depthSigma"), std::max(0.01f, ctx.ssgiDepthReject));
    glUniform1f(glGetUniformLocation(m_csUpsample->GetProgramID(), "normalThresh"), std::max(0.05f, ctx.ssgiNormalReject));

    GLuint gx = (m_hw + 7) / 8;
    GLuint gy = (m_hh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(0);
}

void SSGIPass::runTemporal(RenderContext& ctx) {
    glUseProgram(m_csTemporal->GetProgramID());

    glBindTextureUnit(0, m_ssgiBlur->ID());
    glBindTextureUnit(1, m_historySSGI->ID());
    glBindTextureUnit(2, ctx.velocityTex);
    glBindTextureUnit(3, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(4, ctx.gbufferFBO->GetColorAttachment(0));
    glBindImageTexture(5, m_ssgiWork->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "alpha"), std::clamp(ctx.ssgiTemporalAlpha, 0.05f, 0.3f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "depthThreshold"), std::max(0.01f, ctx.ssgiDepthReject));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "normalThreshold"), std::max(0.05f, ctx.ssgiNormalReject));
    glUniform1i(glGetUniformLocation(m_csTemporal->GetProgramID(), "useYCoCg"), ctx.taaUseYCoCg ? 1 : 0);

    GLuint gx = (m_hw + 7) / 8;
    GLuint gy = (m_hh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glCopyImageSubData(m_ssgiWork->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historySSGI->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_hw, m_hh, 1);

    glUseProgram(0);
}

void SSGIPass::runFinalUpsample(RenderContext& ctx) {
    glUseProgram(m_csFinalUpsample->GetProgramID());

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssgiWork->ID());
    glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "inSSGI"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "depthTex"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "normalTex"), 2);

    glBindImageTexture(3, m_ssgiTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glUniform2f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invDst"),
        1.0f / float(m_w), 1.0f / float(m_h));
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "depthSigma"), std::max(0.01f, ctx.ssgiDepthReject));
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "normalThresh"), std::max(0.05f, ctx.ssgiNormalReject));

    GLuint gx = (m_w + 7) / 8;
    GLuint gy = (m_h + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(0);
}

void SSGIPass::CaptureHistory(RenderContext& ctx) {
    FrameBuffer* sourceFBO = (ctx.taaFBO && ctx.enableTAA) ? ctx.taaFBO : ctx.hdrFBO.get();
    if (!sourceFBO || !m_historyColor) return;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFBO->GetFBO());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    GLuint tempFBO = 0;
    glGenFramebuffers(1, &tempFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tempFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_historyColor->ID(), 0);

    glBlitFramebuffer(0, 0, ctx.width, ctx.height,
        0, 0, m_hw, m_hh,
        GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &tempFBO);
}
