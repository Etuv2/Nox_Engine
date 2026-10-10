#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"

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

    void SetBloomTexture(rhi::TextureHandle bloomTex) { m_bloomTexture = bloomTex; }

private:
    rhi::ProgramHandle m_shader;
    rhi::TextureHandle m_bloomTexture;
    unsigned m_frameIndex = 0;
    bool m_runtimeVerboseLogging = false;
};
