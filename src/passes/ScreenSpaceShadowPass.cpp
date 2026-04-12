#include "ScreenSpaceShadowPass.h"
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
    if (m_resolveShader) {
        glDeleteProgram(m_resolveShader);
        m_resolveShader = 0;
    }

    m_traceTex.reset();
    m_historyTex.reset();

    if (m_paramsUBO) {
        glDeleteBuffers(1, &m_paramsUBO);
        m_paramsUBO = 0;
    }
}

bool ScreenSpaceShadowPass::Initialize(RenderContext& ctx) {
    m_cs = std::make_unique<ComputeShader>();
    if (!m_cs->CreateFromFile("shaders/screen_space_shadows_comp.glsl")) {
        std::cerr << "[SSS] Failed to compile compute shader.\n";
        return false;
    }

    m_resolveShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", "shaders/ssao_blur.glsl");
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

GLuint ScreenSpaceShadowPass::GetShadowTexture() const {
    return m_resolveFBO ? m_resolveFBO->GetColorAttachment(0) : 0;
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
    }

    glm::vec3 lightDirection = glm::normalize(dirLight->GetDirection());
    glm::vec3 lightToSurfaceVS = glm::normalize(glm::mat3(ctx.view) * lightDirection);
    glm::vec3 surfaceToLightVS = -lightToSurfaceVS;

    m_cfg.steps = std::clamp(ctx.sssSampleCount, 4, 24);
    m_cfg.rayLength = ctx.sssMaxRayLength;
    m_cfg.thickness = std::max(0.001f, ctx.sssThickness);
    m_cfg.edgeFade = std::max(0.0005f, ctx.sssEdgeThreshold);

    updateUBO(surfaceToLightVS, ctx.proj);

    glUseProgram(m_cs->GetProgramID());
    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(1, m_historyTex->ID());
    glBindImageTexture(2, m_traceTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, m_paramsUBO);

    GLuint gx = (m_traceWidth + 7) / 8;
    GLuint gy = (m_traceHeight + 7) / 8;
    glDispatchCompute(gx, gy, 1);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glBindBufferBase(GL_UNIFORM_BUFFER, 3, 0);
    glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
    glUseProgram(0);

    resolveToFullResolution(ctx);

    glCopyImageSubData(m_traceTex->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       m_historyTex->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
                       m_traceWidth, m_traceHeight, 1);

    m_historyValid = true;
    ++m_frameIndex;
}

void ScreenSpaceShadowPass::createOutput(int w, int h) {
    m_width = w;
    m_height = h;
    m_traceWidth = std::max(1, static_cast<int>(w * m_cfg.resolutionScale));
    m_traceHeight = std::max(1, static_cast<int>(h * m_cfg.resolutionScale));

    m_traceTex = Texture::Builder::Texture2D(m_traceWidth, m_traceHeight, GL_R8)
        .Format(GL_RED)
        .DataType(GL_UNSIGNED_BYTE)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .TextureType(TextureType::Custom)
        .Build();

    m_historyTex = Texture::Builder::Texture2D(m_traceWidth, m_traceHeight, GL_R8)
        .Format(GL_RED)
        .DataType(GL_UNSIGNED_BYTE)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .TextureType(TextureType::Custom)
        .Build();

    m_resolveFBO = std::make_unique<FrameBuffer>(
        w, h,
        std::vector<GLenum>{ GL_R8 },
        false, false
    );

    std::vector<uint8_t> fullyLit(static_cast<size_t>(m_traceWidth) * static_cast<size_t>(m_traceHeight), 255);
    if (m_traceTex) {
        m_traceTex->Upload2D(0, 0, 0, m_traceWidth, m_traceHeight, GL_RED, GL_UNSIGNED_BYTE, fullyLit.data());
    }
    if (m_historyTex) {
        m_historyTex->Upload2D(0, 0, 0, m_traceWidth, m_traceHeight, GL_RED, GL_UNSIGNED_BYTE, fullyLit.data());
    }

    m_historyValid = false;
    if (!m_paramsUBO) createUBO();
}

void ScreenSpaceShadowPass::createUBO() {
    if (m_paramsUBO) glDeleteBuffers(1, &m_paramsUBO);
    glGenBuffers(1, &m_paramsUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, m_paramsUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(SSSParamsStd140), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
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

    glBindBuffer(GL_UNIFORM_BUFFER, m_paramsUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(SSSParamsStd140), &p);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void ScreenSpaceShadowPass::resolveToFullResolution(RenderContext& ctx) {
    m_resolveFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_resolveShader);
    glUniform2f(glGetUniformLocation(m_resolveShader, "inputTexelSize"),
                1.0f / static_cast<float>(m_traceWidth),
                1.0f / static_cast<float>(m_traceHeight));
    glUniform2f(glGetUniformLocation(m_resolveShader, "fullResTexelSize"),
                1.0f / static_cast<float>(ctx.width),
                1.0f / static_cast<float>(ctx.height));
    glUniform1f(glGetUniformLocation(m_resolveShader, "depthThreshold"), std::max(0.001f, m_cfg.depthSensitivity));
    glUniform1f(glGetUniformLocation(m_resolveShader, "normalThreshold"), 0.15f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_traceTex->ID());
    glUniform1i(glGetUniformLocation(m_resolveShader, "ssaoInput"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glUniform1i(glGetUniformLocation(m_resolveShader, "gDepth"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_resolveShader, "gPackedNormalRM"), 2);

    if (ctx.screenQuad) {
        ctx.screenQuad->Render();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
