#pragma once
#include "../RenderPass.h"
#include "rhi/RHI.h"

struct InteriorAtlas;

/**
 * GBufferPass writes extended G-Buffer attachments:
 * - Normal (RG8)
 * - Roughness/Metallic (RG8)
 * - Albedo (RGB16F)
 * - Emissive (RGB16F)
 * - Specular (RGB16F)
 * - Occlusion (R8)
 * Plus depth buffer
 */
class GBufferPass : public RenderPass {
public:
    GBufferPass();
    ~GBufferPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

private:
    void UploadInteriorAtlas(const InteriorAtlas& atlas);

    rhi::ProgramHandle m_shader;
    rhi::UniformLocation m_locView = -1;
    rhi::UniformLocation m_locProjection = -1;
    bool m_runtimeVerboseLogging = false;
};
