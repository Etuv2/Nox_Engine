#include "BloomPass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

BloomPass::BloomPass() {}

BloomPass::~BloomPass() {
    if (m_extractShader) glDeleteProgram(m_extractShader);
    if (m_kawaseShader) glDeleteProgram(m_kawaseShader);
    if (m_upsampleShader) glDeleteProgram(m_upsampleShader);
}

bool BloomPass::Initialize(RenderContext& context) {
    m_extractShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", 
                                         "shaders/bloom_extract.glsl");
    m_kawaseShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", 
                                        "shaders/kawase_blur.glsl");
    m_upsampleShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", 
                                          "shaders/bloom_upsample.glsl");

    if (!m_extractShader || !m_kawaseShader || !m_upsampleShader) {
        std::cerr << "[BloomPass] Failed to create shaders.\n";
        return false;
    }

    // Create framebuffers
    m_extractFBO = std::make_unique<FrameBuffer>(
        context.width, context.height,
        std::vector<GLenum>{ GL_RGB16F },
        false, false
    );

    for (int i = 0; i < 4; ++i) {
        int scale = 1 << i; // 1, 2, 4, 8
        m_downsampleFBO[i] = std::make_unique<FrameBuffer>(
            context.width / scale, context.height / scale,
            std::vector<GLenum>{ GL_RGB16F },
            false, false
        );
        m_upsampleFBO[i] = std::make_unique<FrameBuffer>(
            context.width / scale, context.height / scale,
            std::vector<GLenum>{ GL_RGB16F },
            false, false
        );
    }

    std::cout << "[BloomPass] Initialized successfully.\n";
    return true;
}

void BloomPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    if (m_extractFBO) m_extractFBO->Resize(newWidth, newHeight);
    
    for (int i = 0; i < 4; ++i) {
        int scale = 1 << i;
        if (m_downsampleFBO[i]) m_downsampleFBO[i]->Resize(newWidth / scale, newHeight / scale);
        if (m_upsampleFBO[i]) m_upsampleFBO[i]->Resize(newWidth / scale, newHeight / scale);
    }
}

GLuint BloomPass::GetBloomResult() const {
    return m_upsampleFBO[0] ? m_upsampleFBO[0]->GetColorAttachment(0) : 0;
}

void BloomPass::Execute(RenderContext& ctx,
                        const std::shared_ptr<SceneGraph>& sceneGraph,
                        const std::shared_ptr<Camera>& camera,
                        const std::shared_ptr<DirectionalLight>& dirLight,
                        const std::shared_ptr<Skybox>& skybox) {
    std::cout << "[BloomPass] Starting execution..." << std::endl;
    
    // Source is HDR FBO color attachment
    GLuint sourceTex = ctx.hdrFBO->GetColorAttachment(0);
    std::cout << "[BloomPass] Source HDR texture: " << sourceTex << std::endl;
    
    ExtractBrightPixels(ctx, sourceTex);
    Downsample(ctx);
    Upsample(ctx);
    
    std::cout << "[BloomPass] Bloom result texture: " << GetBloomResult() << std::endl;
    std::cout << "[BloomPass] Execution complete" << std::endl;
}

void BloomPass::ExtractBrightPixels(RenderContext& ctx, GLuint sourceTex) {
    m_extractFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    
    // CRITICAL: Disable depth test and blending for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_extractShader);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sourceTex);
    glUniform1i(glGetUniformLocation(m_extractShader, "hdrBuffer"), 0);

    glUniform1f(glGetUniformLocation(m_extractShader, "threshold"), ctx.bloomThreshold);
    glUniform1f(glGetUniformLocation(m_extractShader, "knee"), ctx.bloomKnee);
    glUniform1f(glGetUniformLocation(m_extractShader, "bloomStrength"), ctx.bloomStrength);

    ctx.screenQuad->Render();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void BloomPass::Downsample(RenderContext& ctx) {
    // CRITICAL: Disable depth test and blending for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    
    GLuint srcTex = m_extractFBO->GetColorAttachment(0);

    for (int i = 0; i < 4; ++i) {
        m_downsampleFBO[i]->Bind();
        int w = m_downsampleFBO[i]->GetWidth();
        int h = m_downsampleFBO[i]->GetHeight();
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_kawaseShader);
        glUniform1i(glGetUniformLocation(m_kawaseShader, "pass"), i);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glUniform1i(glGetUniformLocation(m_kawaseShader, "image"), 0);

        glm::vec2 texelSize(1.0f / w, 1.0f / h);
        glUniform2fv(glGetUniformLocation(m_kawaseShader, "texelSize"), 
                     1, glm::value_ptr(texelSize));

        ctx.screenQuad->Render();

        srcTex = m_downsampleFBO[i]->GetColorAttachment(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void BloomPass::Upsample(RenderContext& ctx) {
    // CRITICAL: Disable depth test and blending for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    
    GLuint srcTex = m_downsampleFBO[3]->GetColorAttachment(0); // Smallest

    for (int i = 2; i >= 0; --i) {
        m_upsampleFBO[i]->Bind();
        int w = m_upsampleFBO[i]->GetWidth();
        int h = m_upsampleFBO[i]->GetHeight();
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(m_upsampleShader);

        // Low-res input (previous upsample)
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glUniform1i(glGetUniformLocation(m_upsampleShader, "lowResTex"), 0);

        // High-res input (current downsample level)
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_downsampleFBO[i]->GetColorAttachment(0));
        glUniform1i(glGetUniformLocation(m_upsampleShader, "highResTex"), 1);

        ctx.screenQuad->Render();

        srcTex = m_upsampleFBO[i]->GetColorAttachment(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
