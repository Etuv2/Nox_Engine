#include "SSAOPass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <random>
#include <iostream>

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

    // Create framebuffers
    m_ssaoFBO = std::make_unique<FrameBuffer>(
        context.width, context.height,
        std::vector<GLenum>{ GL_R8 },
        false, false, 1, false, GL_DEPTH24_STENCIL8
    );

    for (int i = 0; i < 2; ++i) {
        m_blurFBO[i] = std::make_unique<FrameBuffer>(
            context.width, context.height,
            std::vector<GLenum>{ GL_R8 },
            false, false
        );
    }

    if (!m_ssaoFBO->IsComplete() || !m_blurFBO[0]->IsComplete() || !m_blurFBO[1]->IsComplete()) {
        std::cerr << "[SSAOPass] SSAO framebuffers not complete!\n";
        return false;
    }

    GenerateKernel();
    GenerateNoise();

    std::cout << "[SSAOPass] Initialized successfully.\n";
    return true;
}

void SSAOPass::GenerateKernel() {
    // 192 samples with hemisphere distribution
    const int sampleCount = 192;
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
        
        // Scale samples so more are closer to origin
        float scale = float(i) / float(sampleCount);
        scale = glm::mix(0.1f, 1.0f, scale * scale);
        sample *= scale;
        
        m_kernel.push_back(sample);
    }
}

void SSAOPass::GenerateNoise() {
    // 4x4 noise texture for rotation
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

void SSAOPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    if (m_ssaoFBO) m_ssaoFBO->Resize(newWidth, newHeight);
    for (int i = 0; i < 2; ++i) {
        if (m_blurFBO[i]) m_blurFBO[i]->Resize(newWidth, newHeight);
    }
}

GLuint SSAOPass::GetSSAOTexture() const {
    return m_blurFBO[1] ? m_blurFBO[1]->GetColorAttachment(0) : 0;
}

void SSAOPass::Execute(RenderContext& ctx,
                       const std::shared_ptr<SceneGraph>& sceneGraph,
                       const std::shared_ptr<Camera>& camera,
                       const std::shared_ptr<DirectionalLight>& dirLight,
                       const std::shared_ptr<Skybox>& skybox) {
    RenderSSAO(ctx);
    BilateralBlur(ctx);
}

void SSAOPass::RenderSSAO(RenderContext& ctx) {
    m_ssaoFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(m_ssaoShader);

    // Upload matrices
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "proj"), 
                       1, GL_FALSE, glm::value_ptr(ctx.proj));
    
    glm::mat4 invProj = glm::inverse(ctx.proj);
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "invProj"), 
                       1, GL_FALSE, glm::value_ptr(invProj));

    // CRITICAL FIX: Upload view matrix for normal transformation
    glUniformMatrix4fv(glGetUniformLocation(m_ssaoShader, "view"), 
                       1, GL_FALSE, glm::value_ptr(ctx.view));
    
    // CRITICAL FIX: Set normals in world space flag
    glUniform1i(glGetUniformLocation(m_ssaoShader, "normalsInWorldSpace"), 1);

    // Screen size
    glUniform2f(glGetUniformLocation(m_ssaoShader, "screenSize"), 
                static_cast<float>(ctx.width), static_cast<float>(ctx.height));

    // Upload kernel samples (now correctly limited to 64)
    int kernelSamples = std::min(64, static_cast<int>(m_kernel.size()));
    for (int i = 0; i < kernelSamples; ++i) {
        std::string name = "samples[" + std::to_string(i) + "]";
        glUniform3fv(glGetUniformLocation(m_ssaoShader, name.c_str()), 
                     1, glm::value_ptr(m_kernel[i]));
    }

    // CRITICAL FIX: Bind G-buffer depth texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "gDepth"), 0);

    // CRITICAL FIX: Bind G-buffer packed normals (RT0: oct normal + roughness + metallic)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glUniform1i(glGetUniformLocation(m_ssaoShader, "gPackedNormalRM"), 1);

    // Bind noise texture
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_noiseTex);
    glUniform1i(glGetUniformLocation(m_ssaoShader, "noiseTex"), 2);

    // SSAO parameters
    glUniform1f(glGetUniformLocation(m_ssaoShader, "radius"), ctx.ssaoRadius);
    glUniform1f(glGetUniformLocation(m_ssaoShader, "bias"), ctx.ssaoBias);

    ctx.screenQuad->Render();
  
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SSAOPass::BilateralBlur(RenderContext& ctx) {
    glUseProgram(m_blurShader);

    GLuint srcTex = m_ssaoFBO->GetColorAttachment(0);

    for (int i = 0; i < 2; ++i) {
        m_blurFBO[i % 2]->Bind();
        glViewport(0, 0, ctx.width, ctx.height);
        glClear(GL_COLOR_BUFFER_BIT);

        glm::vec2 texelSize(1.0f / ctx.width, 1.0f / ctx.height);
        glUniform2fv(glGetUniformLocation(m_blurShader, "texelSize"), 
                     1, glm::value_ptr(texelSize));

        glUniform1f(glGetUniformLocation(m_blurShader, "depthThreshold"), 
                    ctx.ssaoBlurDepthThreshold);

        // CRITICAL FIX: Bind SSAO input texture
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glUniform1i(glGetUniformLocation(m_blurShader, "ssaoInput"), 0);

        // CRITICAL FIX: Bind depth for bilateral filtering
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
        glUniform1i(glGetUniformLocation(m_blurShader, "gDepth"), 1);

        // CRITICAL FIX: Bind packed normals from RT0
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
        glUniform1i(glGetUniformLocation(m_blurShader, "gPackedNormalRM"), 2);

        ctx.screenQuad->Render();

        srcTex = m_blurFBO[i % 2]->GetColorAttachment(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
