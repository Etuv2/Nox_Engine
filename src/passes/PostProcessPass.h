#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>

/**
 * PostProcessPass performs final tone-mapping and output to backbuffer:
 * - Unbinds to default framebuffer (backbuffer)
 * - Applies exposure and gamma correction
 * - Composites bloom result
 * - Outputs to screen
 */
class PostProcessPass : public RenderPass {
public:
    PostProcessPass();
    ~PostProcessPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    void SetBloomTexture(GLuint bloomTex) { m_bloomTexture = bloomTex; }

private:
    GLuint m_shader = 0;
    GLuint m_bloomTexture = 0;
};
