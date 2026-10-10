#include "SSAOPass.h"
#include "../PresentTarget.h"
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
    if (!rhi::HasGpu()) return;
    rhi::Device& gpu = rhi::Gpu();
    if (m_ssaoShader) gpu.DestroyProgram(m_ssaoShader);
    if (m_blurShader) gpu.DestroyProgram(m_blurShader);
    if (m_noiseTex) gpu.DestroyTexture(m_noiseTex);
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

    rhi::Device& gpu = rhi::Gpu();
    rhi::TextureDesc desc;
    desc.type = rhi::TextureType::Tex2D;
    desc.format = rhi::Format::RGB16F;
    desc.width = 4;
    desc.height = 4;
    m_noiseTex = gpu.CreateTexture(desc);
    gpu.UpdateTexture(m_noiseTex, {}, rhi::PixelFormat::RGB, rhi::PixelType::Float, noise.data());
    gpu.SetTextureFilter(m_noiseTex, rhi::Filter::Nearest, rhi::Filter::Nearest);
    gpu.SetTextureWrapAxis(m_noiseTex, 0, rhi::Wrap::Repeat);
    gpu.SetTextureWrapAxis(m_noiseTex, 1, rhi::Wrap::Repeat);
}

void SSAOPass::Resize(RenderContext&, int newWidth, int newHeight) {
    m_renderWidth = std::max(1, static_cast<int>(newWidth * m_config.resolutionScale));
    m_renderHeight = std::max(1, static_cast<int>(newHeight * m_config.resolutionScale));

    m_ssaoFBO = std::make_unique<FrameBuffer>(
        m_renderWidth, m_renderHeight,
        std::vector<rhi::Format>{ rhi::Format::R8 },
        false, false, 1, false, rhi::Format::D24S8
    );

    m_historyFBO = std::make_unique<FrameBuffer>(
        m_renderWidth, m_renderHeight,
        std::vector<rhi::Format>{ rhi::Format::R8 },
        false, false
    );

    m_resolveFBO = std::make_unique<FrameBuffer>(
        newWidth, newHeight,
        std::vector<rhi::Format>{ rhi::Format::R8 },
        false, false
    );

    m_historyValid = false;
}

rhi::TextureHandle SSAOPass::GetSSAOTexture() const {
    return m_resolveFBO ? m_resolveFBO->GetColorAttachment(0) : rhi::TextureHandle{};
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
        rhi::TextureRegion srcRegion;
        srcRegion.extent = glm::ivec3(m_renderWidth, m_renderHeight, 1);
        rhi::Gpu().CopyTexture(m_ssaoFBO->GetColorAttachment(0), srcRegion,
                               m_historyFBO->GetColorAttachment(0), 0, glm::ivec3(0));
        m_historyValid = true;
    }
}

void SSAOPass::RenderSSAO(RenderContext& ctx) {
    rhi::Device& gpu = rhi::Gpu();

    m_ssaoFBO->Bind();
    gpu.SetViewport({ 0, 0, m_renderWidth, m_renderHeight });
    gpu.SetDepthTest(false);
    gpu.Clear(rhi::ClearColor);

    gpu.UseProgram(m_ssaoShader);

    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "proj"), ctx.proj);

    glm::mat4 invProj = glm::inverse(ctx.proj);
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "invProj"), invProj);
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "view"), ctx.view);

    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "normalsInWorldSpace"), 1);
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "screenSize"),
                   glm::vec2(static_cast<float>(m_renderWidth), static_cast<float>(m_renderHeight)));

    const int kernelSamples = std::clamp(
        m_config.sampleCount,
        1,
        std::min(64, static_cast<int>(m_kernel.size()))
    );
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "sampleCount"), kernelSamples);
    for (int i = 0; i < kernelSamples; ++i) {
        std::string name = "samples[" + std::to_string(i) + "]";
        gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, name.c_str()), m_kernel[i]);
    }

    gpu.BindTexture(0, ctx.gbufferFBO->GetDepthTexture());
    // Depth is sampled without comparison; the compare func is the GL default (LessEqual).
    gpu.SetTextureCompare(ctx.gbufferFBO->GetDepthTexture(), false, rhi::CompareOp::LessEqual);
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "gDepth"), 0);

    gpu.BindTexture(1, ctx.gbufferFBO->GetColorAttachment(0));
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "gPackedNormalRM"), 1);

    gpu.BindTexture(2, m_noiseTex);
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "noiseTex"), 2);

    gpu.BindTexture(3, m_historyFBO ? m_historyFBO->GetColorAttachment(0) : rhi::TextureHandle{});
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "historyAO"), 3);

    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "radius"), ctx.ssaoRadius);
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "bias"), ctx.ssaoBias);
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "temporalBlend"), m_config.temporalBlend);
    gpu.SetUniform(gpu.GetUniformLocation(m_ssaoShader, "historyValid"), m_historyValid ? 1 : 0);

    ctx.screenQuad->Render();
    gpu.BindFramebuffer(PresentTarget::Get());
}

void SSAOPass::ResolveSSAO(RenderContext& ctx) {
    rhi::Device& gpu = rhi::Gpu();

    m_resolveFBO->Bind();
    gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
    gpu.SetDepthTest(false);
    gpu.Clear(rhi::ClearColor);

    gpu.UseProgram(m_blurShader);

    glm::vec2 inputTexelSize(1.0f / static_cast<float>(m_renderWidth),
                             1.0f / static_cast<float>(m_renderHeight));
    glm::vec2 fullResTexelSize(1.0f / static_cast<float>(ctx.width),
                               1.0f / static_cast<float>(ctx.height));
    gpu.SetUniform(gpu.GetUniformLocation(m_blurShader, "inputTexelSize"), inputTexelSize);
    gpu.SetUniform(gpu.GetUniformLocation(m_blurShader, "fullResTexelSize"), fullResTexelSize);
    gpu.SetUniform(gpu.GetUniformLocation(m_blurShader, "depthThreshold"), std::max(0.001f, ctx.ssaoBlurDepthThreshold));
    gpu.SetUniform(gpu.GetUniformLocation(m_blurShader, "normalThreshold"), m_config.normalThreshold);

    gpu.BindTexture(0, m_ssaoFBO->GetColorAttachment(0));
    gpu.SetUniform(gpu.GetUniformLocation(m_blurShader, "ssaoInput"), 0);

    gpu.BindTexture(1, ctx.gbufferFBO->GetDepthTexture());
    gpu.SetUniform(gpu.GetUniformLocation(m_blurShader, "gDepth"), 1);

    gpu.BindTexture(2, ctx.gbufferFBO->GetColorAttachment(0));
    gpu.SetUniform(gpu.GetUniformLocation(m_blurShader, "gPackedNormalRM"), 2);

    ctx.screenQuad->Render();
    gpu.BindFramebuffer(PresentTarget::Get());
}
