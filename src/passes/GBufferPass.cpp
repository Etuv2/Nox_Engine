#include "GBufferPass.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

GBufferPass::GBufferPass() {}

GBufferPass::~GBufferPass() {
    if (m_shader) {
        glDeleteProgram(m_shader);
        m_shader = 0;
    }
}

bool GBufferPass::Initialize(RenderContext& context) {
    m_shader = CreateShaderProgram("shaders/gbuffer_vert.glsl", 
                                   "shaders/gbuffer_frag.glsl");
    if (!m_shader) {
        std::cerr << "[GBufferPass] Failed to create shader.\n";
        return false;
    }
    std::cout << "[GBufferPass] Initialized successfully.\n";
    return true;
}

void GBufferPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    // G-buffer FBO is resized by Renderer coordinator
    std::cout << "[GBufferPass] Resized to " << newWidth << "x" << newHeight << "\n";
}

void GBufferPass::Execute(RenderContext& ctx,
                          const std::shared_ptr<SceneGraph>& sceneGraph,
                          const std::shared_ptr<Camera>& camera,
                          const std::shared_ptr<DirectionalLight>& dirLight,
                          const std::shared_ptr<Skybox>& skybox) {
    std::cout << "[GBufferPass] Starting execution..." << std::endl;
    
    if (!sceneGraph || !camera) {
        std::cerr << "[GBufferPass] ERROR: Missing sceneGraph or camera!" << std::endl;
        return;
    }

    if (!ctx.gbufferFBO) {
        std::cerr << "[GBufferPass] ERROR: G-buffer FBO is null!" << std::endl;
        return;
    }

    std::cout << "[GBufferPass] Binding G-buffer FBO (ID: " << ctx.gbufferFBO->GetFBO() << ")" << std::endl;
    
    // Bind G-buffer FBO
    ctx.gbufferFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    std::cout << "[GBufferPass] Using shader program: " << m_shader << std::endl;
    glUseProgram(m_shader);

    // Upload matrices
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "view"), 
                       1, GL_FALSE, glm::value_ptr(ctx.view));
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "projection"), 
                       1, GL_FALSE, glm::value_ptr(ctx.proj));

    std::cout << "[GBufferPass] Drawing geometry..." << std::endl;
    // Render scene geometry to G-buffer
    sceneGraph->DrawGeometry(m_shader);

    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::cout << "[GBufferPass] Execution complete" << std::endl;
}
