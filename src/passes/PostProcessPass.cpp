#include "PostProcessPass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <iostream>

PostProcessPass::PostProcessPass() {}

PostProcessPass::~PostProcessPass() {
    if (m_shader) glDeleteProgram(m_shader);
}

bool PostProcessPass::Initialize(RenderContext& context) {
    m_shader = CreateShaderProgram("shaders/fullscreen_vert.glsl", 
                                   "shaders/postprocess_frag.glsl");
    if (!m_shader) {
        std::cerr << "[PostProcessPass] Failed to create shader.\n";
        return false;
    }

    std::cout << "[PostProcessPass] Initialized successfully.\n";
    return true;
}

void PostProcessPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    // Viewport handled in Execute
}

void PostProcessPass::Execute(RenderContext& ctx,
                              const std::shared_ptr<SceneGraph>& sceneGraph,
                              const std::shared_ptr<Camera>& camera,
                              const std::shared_ptr<DirectionalLight>& dirLight,
                              const std::shared_ptr<Skybox>& skybox) {
    std::cout << "[PostProcessPass] Starting execution..." << std::endl;
    
    // CRITICAL: Unbind to backbuffer (FBO 0)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ctx.width, ctx.height);
    
    // CRITICAL: Match legacy renderer - disable depth test and blending for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    
    // Clear with environment color
    glClearColor(ctx.envColor.r, ctx.envColor.g, ctx.envColor.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    std::cout << "[PostProcessPass] Using shader: " << m_shader << std::endl;
    glUseProgram(m_shader);

    // Set exposure and gamma
    glUniform1f(glGetUniformLocation(m_shader, "exposure"), ctx.exposure);
    glUniform1f(glGetUniformLocation(m_shader, "gamma"), ctx.gamma);

    std::cout << "[PostProcessPass] HDR FBO: " << (ctx.hdrFBO ? ctx.hdrFBO->GetFBO() : 0) << std::endl;
    std::cout << "[PostProcessPass] HDR texture: " << (ctx.hdrFBO ? ctx.hdrFBO->GetColorAttachment(0) : 0) << std::endl;
    
    // Bind HDR scene (or TAA-resolved output if TAA is enabled)
    glActiveTexture(GL_TEXTURE0);
    if (ctx.hdrFBO) {
        GLuint hdrTex = ctx.hdrFBO->GetColorAttachment(0);
        if (glIsTexture(hdrTex)) {
            glBindTexture(GL_TEXTURE_2D, hdrTex);
            std::cout << "[PostProcessPass] Bound HDR texture successfully" << std::endl;
        } else {
            std::cerr << "[PostProcessPass] ERROR: Invalid HDR texture!" << std::endl;
        }
    } else {
        std::cerr << "[PostProcessPass] ERROR: HDR FBO is null!" << std::endl;
    }
    glUniform1i(glGetUniformLocation(m_shader, "hdrBuffer"), 0);

    std::cout << "[PostProcessPass] Bloom texture: " << m_bloomTexture << std::endl;
    // Bind bloom result
    glActiveTexture(GL_TEXTURE1);
    if (m_bloomTexture > 0 && glIsTexture(m_bloomTexture)) {
        glBindTexture(GL_TEXTURE_2D, m_bloomTexture);
        std::cout << "[PostProcessPass] Bound bloom texture successfully" << std::endl;
    } else {
        std::cout << "[PostProcessPass] WARNING: No valid bloom texture, using black" << std::endl;
        // Bind a dummy black texture or just leave unbound
    }
    glUniform1i(glGetUniformLocation(m_shader, "bloomBlur"), 1);

    std::cout << "[PostProcessPass] Rendering fullscreen quad..." << std::endl;
    // Render fullscreen quad
    if (ctx.screenQuad) {
        ctx.screenQuad->Render();
    } else {
        std::cerr << "[PostProcessPass] ERROR: ScreenQuad is null!" << std::endl;
    }
    
    std::cout << "[PostProcessPass] Execution complete" << std::endl;
}
