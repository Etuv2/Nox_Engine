#include "GBufferPass.h"
#include "PassLogging.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

#pragma warning(disable: 4996)  // Suppress deprecated function warnings for SceneGraph legacy API

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
    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[GBufferPass] Resized to " << newWidth << "x" << newHeight);
}

void GBufferPass::Execute(RenderContext& ctx,
                          const std::shared_ptr<SceneGraph>& sceneGraph,
                          const std::shared_ptr<Camera>& camera,
                          const std::shared_ptr<DirectionalLight>& dirLight,
                          const std::shared_ptr<Skybox>& skybox) {
    if (!sceneGraph || !camera) {
        std::cerr << "[GBufferPass] ERROR: Missing sceneGraph or camera!" << std::endl;
        return;
    }

    if (!ctx.gbufferFBO) {
        std::cerr << "[GBufferPass] ERROR: G-buffer FBO is null!" << std::endl;
        return;
    }

    // Bind G-buffer FBO
    ctx.gbufferFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Save wireframe state
    GLint oldPolygonMode[2];
    glGetIntegerv(GL_POLYGON_MODE, oldPolygonMode);
    
    // Apply wireframe mode if enabled
    if (ctx.wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(1.0f);
        // Wireframe state is surfaced via UI/profiler context instead of per-frame console logging.
    }

    // Apply force backface culling setting to RenderSystem
    if (auto* renderSystem = sceneGraph->GetRenderSystem()) {
        renderSystem->SetForceBackfaceCulling(ctx.forceBackfaceCulling);
    }

    glUseProgram(m_shader);

    // Upload matrices
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "view"), 
                       1, GL_FALSE, glm::value_ptr(ctx.view));
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "projection"), 
                       1, GL_FALSE, glm::value_ptr(ctx.proj));

    // Render scene geometry to G-buffer
    sceneGraph->RenderGeometry(m_shader);

    // Reset force backface culling after geometry rendering to ensure
    // it doesn't affect other passes (skybox, UI, transparent, etc.)
    if (auto* renderSystem = sceneGraph->GetRenderSystem()) {
        renderSystem->SetForceBackfaceCulling(false);
    }

    // Restore default culling state after geometry pass
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Restore wireframe state
    if (ctx.wireframeMode) {
        glPolygonMode(GL_FRONT_AND_BACK, oldPolygonMode[0]);
    }

    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
