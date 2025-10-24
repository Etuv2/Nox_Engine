#include "TAAPass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

TAAPass::TAAPass() {}

TAAPass::~TAAPass() {
    if (m_velocityShader) glDeleteProgram(m_velocityShader);
    if (m_resolveShader) glDeleteProgram(m_resolveShader);
}

bool TAAPass::Initialize(RenderContext& context) {
    m_velocityShader = CreateShaderProgram("shaders/velocity_vert.glsl", 
                                          "shaders/velocity_frag.glsl");
    m_resolveShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", 
                                         "shaders/taa_resolve.glsl");

    if (!m_velocityShader || !m_resolveShader) {
        std::cerr << "[TAAPass] Failed to create shaders.\n";
        return false;
    }

    // Create framebuffers
    m_velocityFBO = std::make_unique<FrameBuffer>(
        context.width, context.height,
        std::vector<GLenum>{ GL_RG16F },
        true, false
    );

    m_currentFBO = std::make_unique<FrameBuffer>(
        context.width, context.height,
        std::vector<GLenum>{ GL_RGB16F },
        false, false
    );

    m_historyFBO = std::make_unique<FrameBuffer>(
        context.width, context.height,
        std::vector<GLenum>{ GL_RGB16F },
        false, false
    );

    if (!m_velocityFBO->IsComplete() || !m_currentFBO->IsComplete() || !m_historyFBO->IsComplete()) {
        std::cerr << "[TAAPass] TAA framebuffers not complete!\n";
        return false;
    }

    // Expose velocity texture in context for other passes (e.g., SSGI)
    context.velocityTex = m_velocityFBO->GetColorAttachment(0);

    std::cout << "[TAAPass] Initialized successfully.\n";
    return true;
}

void TAAPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    if (m_velocityFBO) m_velocityFBO->Resize(newWidth, newHeight);
    if (m_currentFBO) m_currentFBO->Resize(newWidth, newHeight);
    if (m_historyFBO) m_historyFBO->Resize(newWidth, newHeight);
    context.velocityTex = m_velocityFBO ? m_velocityFBO->GetColorAttachment(0) : 0;
    // Invalidate history on resize
    m_historyValid = false;
}

glm::vec2 TAAPass::GetJitter(int frameIndex, int pattern) {
    if (pattern == 0) {
        // Halton sequence
        auto halton = [](int index, int base) -> float {
            float f = 1.0f;
            float r = 0.0f;
            while (index > 0) {
                f = f / base;
                r = r + f * (index % base);
                index = index / base;
            }
            return r;
        };

        float x = halton((frameIndex % 16) + 1, 2) - 0.5f;
        float y = halton((frameIndex % 16) + 1, 3) - 0.5f;
        return glm::vec2(x, y);
    } else {
        // Hammersley 8-sample pattern
        const glm::vec2 samples[8] = {
            glm::vec2(0.000000f, 0.000000f),
            glm::vec2(0.500000f, 0.333333f),
            glm::vec2(0.250000f, 0.666667f),
            glm::vec2(0.750000f, 0.111111f),
            glm::vec2(0.125000f, 0.444444f),
            glm::vec2(0.625000f, 0.777778f),
            glm::vec2(0.375000f, 0.222222f),
            glm::vec2(0.875000f, 0.555556f)
        };
        return samples[frameIndex % 8] - glm::vec2(0.5f);
    }
}

void TAAPass::Execute(RenderContext& ctx,
                      const std::shared_ptr<SceneGraph>& sceneGraph,
                      const std::shared_ptr<Camera>& camera,
                      const std::shared_ptr<DirectionalLight>& dirLight,
                      const std::shared_ptr<Skybox>& skybox) {
    if (!ctx.enableTAA) {
        // TAA disabled - just copy HDR to current
        m_currentFBO->Bind();
        glViewport(0, 0, ctx.width, ctx.height);
        glClear(GL_COLOR_BUFFER_BIT);
        
        glUseProgram(0); // Use a simple blit or copy shader if available
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx.hdrFBO->GetColorAttachment(0));
        
        ctx.screenQuad->Render();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // Update jitter
    m_prevJitter = m_jitter;
    m_jitter = GetJitter(m_frameIndex, ctx.taaJitterPattern);
    m_frameIndex++;

    // Render velocity buffer
    RenderVelocity(ctx, sceneGraph, camera);

    // Resolve TAA
    ResolveTemporalAntiAliasing(ctx);

    // Swap current and history
    m_currentFBO.swap(m_historyFBO);
    m_historyValid = true;
}

void TAAPass::RenderVelocity(RenderContext& ctx, 
                             const std::shared_ptr<SceneGraph>& sceneGraph,
                             const std::shared_ptr<Camera>& camera) {
    if (!sceneGraph || !camera) return;

    m_velocityFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(m_velocityShader);

    // Upload current and previous matrices
    glUniformMatrix4fv(glGetUniformLocation(m_velocityShader, "view"), 
                       1, GL_FALSE, glm::value_ptr(ctx.view));
    glUniformMatrix4fv(glGetUniformLocation(m_velocityShader, "projection"), 
                       1, GL_FALSE, glm::value_ptr(ctx.proj));
    
    // Previous matrices (stored in context)
    glUniformMatrix4fv(glGetUniformLocation(m_velocityShader, "prevView"), 
                       1, GL_FALSE, glm::value_ptr(ctx.prevView));
    glUniformMatrix4fv(glGetUniformLocation(m_velocityShader, "prevProjection"), 
                       1, GL_FALSE, glm::value_ptr(ctx.prevProj));

    // Upload jitter
    glUniform2fv(glGetUniformLocation(m_velocityShader, "jitter"), 
                 1, glm::value_ptr(m_jitter));
    glUniform2fv(glGetUniformLocation(m_velocityShader, "prevJitter"), 
                 1, glm::value_ptr(m_prevJitter));

    glUniform2f(glGetUniformLocation(m_velocityShader, "screenSize"), 
                static_cast<float>(ctx.width), static_cast<float>(ctx.height));

    // Render scene for motion vectors
    sceneGraph->DrawVelocity(m_velocityShader);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Update context velocity texture each frame
    ctx.velocityTex = m_velocityFBO->GetColorAttachment(0);
}

void TAAPass::ResolveTemporalAntiAliasing(RenderContext& ctx) {
    m_currentFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_resolveShader);

    // Bind current frame (HDR output)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.hdrFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_resolveShader, "currentFrame"), 0);

    // Bind history
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_historyFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_resolveShader, "historyFrame"), 1);

    // Bind velocity
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_velocityFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_resolveShader, "velocityBuffer"), 2);

    // Bind depth for disocclusion detection
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glUniform1i(glGetUniformLocation(m_resolveShader, "depthBuffer"), 3);

    // Bind normal for rejection
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_resolveShader, "gNormal"), 4);

    // Upload TAA parameters
    glUniform1f(glGetUniformLocation(m_resolveShader, "blendFactor"), ctx.taaBlendFactor);
    glUniform1f(glGetUniformLocation(m_resolveShader, "varianceThreshold"), ctx.taaVarianceThreshold);
    glUniform1f(glGetUniformLocation(m_resolveShader, "lumaWeight"), ctx.taaLumaWeight);
    glUniform1i(glGetUniformLocation(m_resolveShader, "useYCoCg"), ctx.taaUseYCoCg ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_resolveShader, "historyValid"), m_historyValid ? 1 : 0);
    glUniform2f(glGetUniformLocation(m_resolveShader, "screenSize"), 
                static_cast<float>(ctx.width), static_cast<float>(ctx.height));
    glUniform2fv(glGetUniformLocation(m_resolveShader, "jitter"), 
                 1, glm::value_ptr(m_jitter));

    // Enhanced quality parameters
    glUniform1f(glGetUniformLocation(m_resolveShader, "depthThreshold"), ctx.taaDepthThreshold);
    glUniform1f(glGetUniformLocation(m_resolveShader, "normalThreshold"), ctx.taaNormalThreshold);
    glUniform1f(glGetUniformLocation(m_resolveShader, "edgeThreshold"), ctx.taaEdgeThreshold);
    glUniform1f(glGetUniformLocation(m_resolveShader, "reactiveMaskStrength"), ctx.taaReactiveMaskStrength);

    ctx.screenQuad->Render();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
