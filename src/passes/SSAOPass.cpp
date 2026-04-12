#include "SSAOPass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <random>
#include <iostream>
#include <algorithm>

SSAOPass::SSAOPass() {}

SSAOPass::~SSAOPass() {
    if (m_ssaoShader) glDeleteProgram(m_ssaoShader);
    if (m_blurShader) glDeleteProgram(m_blurShader);
    if (m_noiseTex) glDeleteTextures(1, &m_noiseTex);
}

bool SSAOPass::Initialize(RenderContext& context) {
    m_ssaoShader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
                                       "shaders/ssao.glsl");
    m_blurShader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
                                       "shaders/ssao_blur.glsl");

    if (!m_ssaoShader || !m_blurShader) {
        std::cerr << "[SSAOPass] Failed to create shaders.\n";
        return false;
    }

    GenerateKernel();
    GenerateNoise();
    Resize(context, context.width, context.height);

    if (!m_ssaoFBO || !m_historyFBO || !m_resolveFBO ||
        !m_ssaoFBO->IsComplete() || !m_historyFBO->IsComplete() || !m_resolveFBO->IsComplete()) {
        std::cerr << "[SSAOPass] SSAO framebuffers not complete!\n";
        return false;
    }

    std::cout << "[SSAOPass] Initialized successfully at "
              << m_renderWidth << "x" << m_renderHeight << " internal resolution.\n";
    return true;
}

void SSAOPass::GenerateKernel() {
    const int sampleCount = 64;
    m_kernel.clear();
    m_kernel.reserve(sampleCount);

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_real_distribution<float> rnd01(0.0f, 1.0f);

    for (int i = 0; i < sampleCount; ++i) {
        glm::vec3 sample(
            rnd01(rng) * 2.0f - 1.0f,
            rnd01(rng) * 2.0f - 1.0f,
            rnd01(rng)
        );
        sample = glm::normalize(sample) * rnd01(rng);

        float scale = static_cast<float>(i) / static_cast<float>(sampleCount);
        scale = glm::mix(0.1f, 1.0f, scale * scale);
        sample *= scale;

        m_kernel.push_back(sample);
    }
}

void SSAOPass::GenerateNoise() {
    std::vector<glm::vec3> noise(16);

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_real_distribution<float> rnd01(0.0f, 1.0f);

    for (auto& n : noise) {
        n = glm::normalize(glm::vec3(
            rnd01(rng) * 2.0f - 1.0f,
            rnd01(rng) * 2.0f - 1.0f,
            0.0f
        ));
    }

    glGenTextures(1, &m_noiseTex);
    glBindTexture(GL_TEXTURE_2D, m_noiseTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0, GL_RGB, GL_FLOAT, noise.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void SSAOPass::Resize(RenderContext&, int newWidth, int newHeight) {
    m_renderWidth = std::max(1, static_cast<int>(newWidth * m_config.resolutionScale));
    m_renderHeight = std::max(1, static_cast<int>(newHeight * m_config.resolutionScale));

    m_ssaoFBO = std::make_unique<FrameBuffer>(
        m_renderWidth, m_renderHeight,
        std::vector<GLenum>{ GL_R8 },
        false, false, 1, false, GL_DEPTH24_STENCIL8
    );

    m_historyFBO = std::make_unique<FrameBuffer>(
        m_renderWidth, m_renderHeight,
        std::vector<GLenum>{ GL_R8 },
        false, false
    );

    m_resolveFBO = std::make_unique<FrameBuffer>(
        newWidth, newHeight,
        std::vector<GLenum>{ GL_R8 },
        false, false
    );

    m_historyValid = false;
}

GLuint SSAOPass::GetSSAOTexture() const {
    return m_resolveFBO ? m_resolveFBO->GetColorAttachment(0) : 0;
}

void SSAOPass::Execute(RenderContext& ctx,
                       const std::shared_ptr<SceneGraph>&,
                       const std::shared_ptr<Camera>&,
                       const std::shared_ptr<DirectionalLight>&,
                       const std::shared_ptr<Skybox>&) {
    if (!ctx.enableSSAO || !ctx.gbufferFBO || !ctx.screenQuad || !m_ssaoFBO || !m_resolveFBO) {
        return;
    }

    RenderSSAO(ctx);
    ResolveSSAO(ctx);

    if (m_historyFBO) {
        glCopyImageSubData(m_ssaoFBO->GetColorAttachment(0), GL_TEXTURE_2D, 0, 0, 0, 0,
                           m_historyFBO->GetColorAttachment(0), GL_TEXTURE_2D, 0, 0, 0, 0,
                           m_renderWidth, m_renderHeight, 1);
        m_historyValid = true;
    }
}

void SSAOPass::RenderSSAO(RenderContext& ctx) {
    m_ssaoFBO->Bind();
    glViewport(0, 0, m_renderWidth, m_renderHeight);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_ssaoShader);

    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "proj"),
                       1, GL_FALSE, glm::value_ptr(ctx.proj));

    glm::mat4 invProj = glm::inverse(ctx.proj);
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "invProj"),
                       1, GL_FALSE, glm::value_ptr(invProj));
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "view"),
                       1, GL_FALSE, glm::value_ptr(ctx.view));

    glUniform1i(glGetUniformLocation(m_ssaoShader, "normalsInWorldSpace"), 1);
    glUniform2f(glGetUniformLocation(m_ssaoShader, "screenSize"),
                static_cast<float>(m_renderWidth), static_cast<float>(m_renderHeight));

    const int kernelSamples = std::clamp(
        m_config.sampleCount,
        1,
        std::min(64, static_cast<int>(m_kernel.size()))
    );
    glUniform1i(glGetUniformLocation(m_ssaoShader, "sampleCount"), kernelSamples);
    for (int i = 0; i < kernelSamples; ++i) {
        std::string name = "samples[" + std::to_string(i) + "]";
        glUniform3fv(glGetUniformLocation(m_ssaoShader, name.c_str()),
                     1, glm::value_ptr(m_kernel[i]));
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "gDepth"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_ssaoShader, "gPackedNormalRM"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_noiseTex);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "noiseTex"), 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_historyFBO ? m_historyFBO->GetColorAttachment(0) : 0);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "historyAO"), 3);

    glUniform1f(glGetUniformLocation(m_ssaoShader, "radius"), ctx.ssaoRadius);
    glUniform1f(glGetUniformLocation(m_ssaoShader, "bias"), ctx.ssaoBias);
    glUniform1f(glGetUniformLocation(m_ssaoShader, "temporalBlend"), m_config.temporalBlend);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "historyValid"), m_historyValid ? 1 : 0);

    ctx.screenQuad->Render();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SSAOPass::ResolveSSAO(RenderContext& ctx) {
    m_resolveFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_blurShader);

    glm::vec2 inputTexelSize(1.0f / static_cast<float>(m_renderWidth),
                             1.0f / static_cast<float>(m_renderHeight));
    glm::vec2 fullResTexelSize(1.0f / static_cast<float>(ctx.width),
                               1.0f / static_cast<float>(ctx.height));
    glUniform2fv(glGetUniformLocation(m_blurShader, "inputTexelSize"), 1, glm::value_ptr(inputTexelSize));
    glUniform2fv(glGetUniformLocation(m_blurShader, "fullResTexelSize"), 1, glm::value_ptr(fullResTexelSize));
    glUniform1f(glGetUniformLocation(m_blurShader, "depthThreshold"), std::max(0.001f, ctx.ssaoBlurDepthThreshold));
    glUniform1f(glGetUniformLocation(m_blurShader, "normalThreshold"), m_config.normalThreshold);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssaoFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_blurShader, "ssaoInput"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glUniform1i(glGetUniformLocation(m_blurShader, "gDepth"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_blurShader, "gPackedNormalRM"), 2);

    ctx.screenQuad->Render();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
