#include "ShadowPass.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../LightManager.h"
#include "../RenderContext.h"
#include <iostream>
#include <algorithm>

ShadowPass::ShadowPass() {}

ShadowPass::~ShadowPass() {
    if (m_shadowShader) {
        glDeleteProgram(m_shadowShader);
        m_shadowShader = 0;
    }
}

bool ShadowPass::Initialize(RenderContext& context) {
    m_shadowShader = CreateShaderProgram("shaders/shadow_vert.glsl", 
                                         "shaders/shadow_frag.glsl");
    if (!m_shadowShader) {
        std::cerr << "[ShadowPass] Failed to create shadow shader.\n";
        return false;
    }
    std::cout << "[ShadowPass] Initialized successfully.\n";
    return true;
}

void ShadowPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    // Shadow maps are fixed resolution, no resize needed
}

void ShadowPass::Execute(RenderContext& ctx,
                         const std::shared_ptr<SceneGraph>& sceneGraph,
                         const std::shared_ptr<Camera>& camera,
                         const std::shared_ptr<DirectionalLight>& dirLight,
                         const std::shared_ptr<Skybox>& skybox) {
    if (!ctx.enableShadows || !sceneGraph || !camera) {
        return;
    }

    auto lightManager = ctx.lightManager;
    if (!lightManager) {
        std::cerr << "[ShadowPass] No LightManager available!\n";
        return;
    }

    auto& shadowConfig = lightManager->GetShadowConfig();
    shadowConfig.enablePCSS = ctx.enablePCSS;
    shadowConfig.directionalConstantBias = ctx.shadowBias;
    shadowConfig.directionalSlopeBias = std::max(ctx.shadowBias * 2.0f, ctx.shadowBias);
    shadowConfig.directionalNormalOffset = std::max(0.001f, ctx.shadowBias * 4.0f);
    shadowConfig.useRotatedPoissonPCF = true;
    shadowConfig.stableTexelSnapping = true;

    // Ensure shadow shader is set on LightManager
    if (m_shadowShader > 0 && lightManager->GetShadowShader() == 0) {
        lightManager->SetShadowShader(m_shadowShader);
        std::cout << "[ShadowPass] Set shadow shader on LightManager.\n";
    }

    // Validate shadow array exists
    GLuint shadowArray = lightManager->GetShadowArrayTexture();
    if (shadowArray == 0) {
        std::cerr << "[ShadowPass] Shadow array not initialized - reinitializing...\n";
        lightManager->InitializeShadowSystem(12, 1024);
    }

    // Render all shadow maps into unified array
    float aspect = static_cast<float>(ctx.width) / static_cast<float>(ctx.height);
    lightManager->RenderShadowMaps(sceneGraph, camera, ctx.view, ctx.proj, 
                                   m_shadowNear, m_shadowFar, aspect, 
                                   camera->GetCameraFov());
}
