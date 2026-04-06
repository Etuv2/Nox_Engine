#include "PostProcessPass.h"
#include "PassLogging.h"
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
    // Output to backbuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ctx.width, ctx.height);

    // We handle output gamma in the shader, disable fixed-function SRGB conversion
#ifdef GL_FRAMEBUFFER_SRGB
    glDisable(GL_FRAMEBUFFER_SRGB);
#endif
    
    // Disable depth/blend for fullscreen quad
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    
    // Clear with environment color
    glClearColor(ctx.envColor.r, ctx.envColor.g, ctx.envColor.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Using shader: " << m_shader);
    glUseProgram(m_shader);

    // Helper to set uniform only if present
    auto set1f = [&](const char* name, float v){ GLint loc = glGetUniformLocation(m_shader, name); if (loc >= 0) glUniform1f(loc, v); };
    auto set1i = [&](const char* name, int v){ GLint loc = glGetUniformLocation(m_shader, name); if (loc >= 0) glUniform1i(loc, v); };

    set1f("exposure", ctx.exposure);
    set1f("gamma", ctx.gamma);

    // Tonemapper uniforms
    set1i("uTonemap", static_cast<int>(ctx.tonemapType));
    set1f("uP", ctx.tm_P);
    set1f("ua", ctx.tm_a);
    set1f("um", ctx.tm_m);
    set1f("ul", ctx.tm_l);
    set1f("uc", ctx.tm_c);
    set1f("ub", ctx.tm_b);
    set1i("uOutputSRGB", ctx.outputSRGB ? 1 : 0);

    // GT7 params
    set1f("uTm7PeakNits", ctx.tm7_peakNits);
    set1f("uTm7Blend", ctx.tm7_blend);
    set1f("uTm7FadeStart", ctx.tm7_fadeStart);
    set1f("uTm7FadeEnd", ctx.tm7_fadeEnd);
    set1i("uTm7UseJzazbz", ctx.tm7_useJzazbz ? 1 : 0);

    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] HDR FBO: " << (ctx.hdrFBO ? ctx.hdrFBO->GetFBO() : 0));
    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] HDR texture: " << (ctx.hdrFBO ? ctx.hdrFBO->GetColorAttachment(0) : 0));
    
    // Bind HDR scene (or TAA-resolved output if TAA is enabled)
    glActiveTexture(GL_TEXTURE0);
    if (ctx.hdrFBO) {
        GLuint hdrTex = ctx.hdrFBO->GetColorAttachment(0);
        if (glIsTexture(hdrTex)) {
            glBindTexture(GL_TEXTURE_2D, hdrTex);
            PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Bound HDR texture successfully");
        } else {
            std::cerr << "[PostProcessPass] ERROR: Invalid HDR texture!" << std::endl;
        }
    } else {
        std::cerr << "[PostProcessPass] ERROR: HDR FBO is null!" << std::endl;
    }
    set1i("hdrBuffer", 0);

    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Bloom texture: " << m_bloomTexture);
    // Bind bloom result
    glActiveTexture(GL_TEXTURE1);
    if (m_bloomTexture > 0 && glIsTexture(m_bloomTexture)) {
        glBindTexture(GL_TEXTURE_2D, m_bloomTexture);
        PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Bound bloom texture successfully");
    } else {
        PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] No valid bloom texture, using black");
        // Optionally bind 0
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    set1i("bloomBlur", 1);

    PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[PostProcessPass] Rendering fullscreen quad...");
    // Render fullscreen quad
    if (ctx.screenQuad) {
        ctx.screenQuad->Render();
    } else {
        std::cerr << "[PostProcessPass] ERROR: ScreenQuad is null!" << std::endl;
    }
}
