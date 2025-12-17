#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>

/**
 * @brief TransparentForwardPass renders transparent objects with physically correct glass-like materials.
 * 
 */
class TransparentForwardPass : public RenderPass {
public:
    TransparentForwardPass();
    ~TransparentForwardPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

private:
    GLuint m_shader = 0; // Forward PBR shader for transparent materials
};
