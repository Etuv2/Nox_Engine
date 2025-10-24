#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>

/**
 * ShadowPass uses LightManager to render all lights' shadows into unified shadow array.
 * Sets shadow shader on LightManager if missing.
 */
class ShadowPass : public RenderPass {
public:
    ShadowPass();
    ~ShadowPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

private:
    GLuint m_shadowShader = 0;
    float m_shadowNear = 0.1f;
    float m_shadowFar = 100.0f;
};
