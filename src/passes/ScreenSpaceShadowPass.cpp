#include "ScreenSpaceShadowPass.h"
#include "../PresentTarget.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../ShaderLoader.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <vector>
#include <algorithm>

namespace {
    struct SSSParamsStd140 {
        glm::vec2 invScreen;
        glm::vec2 screenSize;

        glm::vec3 lightDirVS;
        float maxRayLength;
        int numSteps;
        float thickness;
        float edgeThreshold;
        float jitterStrength;
        int enableJitter;
        int debugMode;
        int _pad0;
        int _pad1;

        glm::mat4 invProj;
        glm::mat4 proj;

        uint32_t frameIndex;
        uint32_t historyValid;
        uint32_t enableTemporalAccumulation;
        float temporalBlend;

        int enableBilateralBlur;
        int blurRadius;
        float depthSensitivity;
        float _pad5;
    };
}

ScreenSpaceShadowPass::ScreenSpaceShadowPass() {}

ScreenSpaceShadowPass::~ScreenSpaceShadowPass() {
    if (rhi::HasGpu() && m_resolveShader) {
        rhi::Gpu().DestroyProgram(m_resolveShader);
    }

    m_traceTex.reset();
    m_historyTex.reset();

    if (rhi::HasGpu() && m_paramsUBO) {
        rhi::Gpu().DestroyBuffer(m_paramsUBO);
    }
}

bool ScreenSpaceShadowPass::Initialize(RenderContext& ctx) {
    m_cs = std::make_unique<ComputeShader>();
    if (!m_cs->CreateFromFile("shaders/screen_space_shadows_comp.glsl")) {
        std::cerr << "[SSS] Failed to compile compute shader.\n";
        return false;
    }

    m_resolveShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", "shaders/screen_space_bilateral_resolve.glsl");
    if (!m_resolveShader) {
        std::cerr << "[SSS] Failed to create scalar resolve shader.\n";
        return false;
    }

    createOutput(ctx.width, ctx.height);
    createUBO();
    return m_resolveFBO && m_resolveFBO->IsComplete();
}

void ScreenSpaceShadowPass::Resize(RenderContext&, int w, int h) {
    if (w == m_width && h == m_height) return;
    createOutput(w, h);
}

rhi::TextureHandle ScreenSpaceShadowPass::GetShadowTexture() const {
    return m_resolveFBO ? m_resolveFBO->GetColorAttachment(0) : rhi::TextureHandle{};
}

void ScreenSpaceShadowPass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>&,
    const std::shared_ptr<Camera>&,
    const std::shared_ptr<DirectionalLight>& dirLight,
    const std::shared_ptr<Skybox>&)
{
    if (!m_cs || !m_cs->IsValid() || !ctx.gbufferFBO || !dirLight || !m_traceTex || !m_historyTex || !m_resolveFBO) {
        return;
    }

    m_cfg.resolutionScale = glm::clamp(ctx.sssResolutionScale, 0.25f, 1.0f);
    m_cfg.temporalBlend = glm::clamp(ctx.sssTemporalAlpha, 0.02f, 0.8f);
    const int desiredTraceWidth = std::max(1, static_cast<int>(ctx.width * m_cfg.resolutionScale));
    const int desiredTraceHeight = std::max(1, static_cast<int>(ctx.height * m_cfg.resolutionScale));
    if (desiredTraceWidth != m_traceWidth || desiredTraceHeight != m_traceHeight ||
        ctx.width != m_width || ctx.height != m_height) {
        createOutput(ctx.width, ctx.height);
        if (!m_traceTex || !m_historyTex || !m_resolveFBO || !m_resolveFBO->IsComplete()) {
            return;
        }
    }

    m_cfg.steps = std::clamp(ctx.sssSampleCount, 4, 24);
    m_cfg.rayLength = ctx.sssMaxRayLength;
    m_cfg.thickness = std::max(0.001f, ctx.sssThickness);
    m_cfg.edgeFade = std::max(0.0005f, ctx.sssEdgeThreshold);
    const bool settingsChanged =
        m_lastSteps != m_cfg.steps ||
        std::abs(m_lastRayLength - m_cfg.rayLength) > 1e-4f ||
        std::abs(m_lastThickness - m_cfg.thickness) > 1e-4f ||
        std::abs(m_lastEdgeFade - m_cfg.edgeFade) > 1e-5f;
    if (settingsChanged) {
        m_historyValid = false;
        m_lastSteps = m_cfg.steps;
        m_lastRayLength = m_cfg.rayLength;
        m_lastThickness = m_cfg.thickness;
        m_lastEdgeFade = m_cfg.edgeFade;
    }

    glm::vec3 surfaceToLightWS = -dirLight->GetDirection();
    if (glm::dot(surfaceToLightWS, surfaceToLightWS) < 1e-8f) {
        surfaceToLightWS = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    glm::vec3 surfaceToLightVS = glm::normalize(glm::mat3(ctx.view) * glm::normalize(surfaceToLightWS));

    updateUBO(surfaceToLightVS, ctx.proj);

    rhi::Device& gpu = rhi::Gpu();

    gpu.UseProgram(m_cs->GetProgramID());
    gpu.BindTexture(0, ctx.gbufferFBO->GetDepthTexture());
    gpu.BindTexture(1, m_historyTex->ID());
    gpu.BindImage(2, m_traceTex->ID(), 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::R8);
    gpu.BindUniformBuffer(3, m_paramsUBO);

    uint32_t gx = (m_traceWidth + 7) / 8;
    uint32_t gy = (m_traceHeight + 7) / 8;
    gpu.Dispatch(gx, gy, 1);

    gpu.PipelineBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);

    gpu.BindUniformBuffer(3, {});
    gpu.BindImage(2, {}, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::R8);
    gpu.UseProgram({});

    resolveToFullResolution(ctx);

    bool copiedHistory = false;
    if (m_traceTex && m_historyTex && m_traceTex->ID().IsValid() && m_historyTex->ID().IsValid() &&
        m_traceTex->Width() == m_historyTex->Width() &&
        m_traceTex->Height() == m_historyTex->Height()) {
        rhi::TextureRegion srcRegion;
        srcRegion.extent = glm::ivec3(m_traceWidth, m_traceHeight, 1);
        gpu.CopyTexture(m_traceTex->ID(), srcRegion, m_historyTex->ID(), 0, glm::ivec3(0));
        copiedHistory = true;
    }

    m_historyValid = copiedHistory;
    ++m_frameIndex;
}

void ScreenSpaceShadowPass::createOutput(int w, int h) {
    m_width = w;
    m_height = h;
    m_traceWidth = std::max(1, static_cast<int>(w * m_cfg.resolutionScale));
    m_traceHeight = std::max(1, static_cast<int>(h * m_cfg.resolutionScale));

    m_traceTex = Texture::Builder::Texture2D(m_traceWidth, m_traceHeight, rhi::Format::R8)
        .Format(rhi::PixelFormat::R)
        .DataType(rhi::PixelType::UByte)
        .FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
        .WrapMode(rhi::Wrap::ClampToEdge)
        .TextureType(TextureType::Custom)
        .Build();

    m_historyTex = Texture::Builder::Texture2D(m_traceWidth, m_traceHeight, rhi::Format::R8)
        .Format(rhi::PixelFormat::R)
        .DataType(rhi::PixelType::UByte)
        .FilterMode(rhi::Filter::Linear, rhi::Filter::Linear)
        .WrapMode(rhi::Wrap::ClampToEdge)
        .TextureType(TextureType::Custom)
        .Build();

    m_resolveFBO = std::make_unique<FrameBuffer>(
        w, h,
        std::vector<rhi::Format>{ rhi::Format::R8 },
        false, false
    );

    std::vector<uint8_t> fullyLit(static_cast<size_t>(m_traceWidth) * static_cast<size_t>(m_traceHeight), 255);
    if (m_traceTex) {
        m_traceTex->Upload2D(0, 0, 0, m_traceWidth, m_traceHeight, rhi::PixelFormat::R, rhi::PixelType::UByte, fullyLit.data());
    }
    if (m_historyTex) {
        m_historyTex->Upload2D(0, 0, 0, m_traceWidth, m_traceHeight, rhi::PixelFormat::R, rhi::PixelType::UByte, fullyLit.data());
    }

    m_historyValid = false;
    if (!m_paramsUBO) createUBO();
}

void ScreenSpaceShadowPass::createUBO() {
    rhi::Device& gpu = rhi::Gpu();
    if (m_paramsUBO) gpu.DestroyBuffer(m_paramsUBO);
    rhi::BufferDesc desc;
    desc.size = sizeof(SSSParamsStd140);
    desc.initialData = nullptr;
    desc.usage = rhi::BufferUsage::DynamicDraw;
    m_paramsUBO = gpu.CreateBuffer(desc);
}

void ScreenSpaceShadowPass::updateUBO(const glm::vec3& lightDirVS, const glm::mat4& projection) {
    SSSParamsStd140 p{};
    p.invScreen = glm::vec2(1.0f / float(m_traceWidth), 1.0f / float(m_traceHeight));
    p.screenSize = glm::vec2(float(m_traceWidth), float(m_traceHeight));
    p.lightDirVS = lightDirVS;
    p.maxRayLength = m_cfg.rayLength;
    p.numSteps = std::max(1, m_cfg.steps);
    p.thickness = m_cfg.thickness;
    p.edgeThreshold = m_cfg.edgeFade;
    p.jitterStrength = glm::clamp(m_cfg.jitterAmount, 0.0f, 1.0f);
    p.enableJitter = m_cfg.jitter ? 1 : 0;
    p.debugMode = m_cfg.debugMode;
    p.invProj = glm::inverse(projection);
    p.proj = projection;
    p.frameIndex = m_frameIndex;
    p.historyValid = m_historyValid ? 1u : 0u;
    p.enableTemporalAccumulation = m_cfg.enableTemporalAccumulation ? 1u : 0u;
    p.temporalBlend = glm::clamp(m_cfg.temporalBlend, 0.02f, 0.8f);
    p.enableBilateralBlur = m_cfg.enableBilateralBlur ? 1 : 0;
    p.blurRadius = glm::clamp(m_cfg.blurRadius, 1, 2);
    p.depthSensitivity = std::max(0.001f, m_cfg.depthSensitivity);

    rhi::Gpu().UpdateBuffer(m_paramsUBO, 0, sizeof(SSSParamsStd140), &p);
}

void ScreenSpaceShadowPass::resolveToFullResolution(RenderContext& ctx) {
    rhi::Device& gpu = rhi::Gpu();

    m_resolveFBO->Bind();
    gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
    gpu.SetDepthTest(false);
    gpu.Clear(rhi::ClearColor);

    gpu.UseProgram(m_resolveShader);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "inputTexelSize"),
        glm::vec2(1.0f / static_cast<float>(m_traceWidth),
                  1.0f / static_cast<float>(m_traceHeight)));
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "fullResTexelSize"),
        glm::vec2(1.0f / static_cast<float>(ctx.width),
                  1.0f / static_cast<float>(ctx.height)));
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "depthThreshold"), std::max(0.001f, m_cfg.depthSensitivity));
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "normalThreshold"), 0.15f);

    gpu.BindTexture(0, m_traceTex->ID());
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "ssaoInput"), 0);

    gpu.BindTexture(1, ctx.gbufferFBO->GetDepthTexture());
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "gDepth"), 1);

    gpu.BindTexture(2, ctx.gbufferFBO->GetColorAttachment(0));
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "gPackedNormalRM"), 2);

    if (ctx.screenQuad) {
        ctx.screenQuad->Render();
    }

    gpu.BindFramebuffer(PresentTarget::Get());
}
