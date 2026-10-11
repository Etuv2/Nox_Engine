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
    void UploadInteriorAtlas(const InteriorAtlas& atlas, uint64_t revision);

    // Interior atlas GPU state, rebuilt when the scene's render extras change.
    struct InteriorGpu {
        const InteriorAtlas* source = nullptr;
        uint64_t revision = ~0ull;
        size_t layerCount = 0;
        rhi::TextureHandle faces;      // cached by TextureArrayBuilder (not owned)
        rhi::TextureHandle roomData;   // owned: RGBA32F per-room tables
        int rooms = 0;
        int styles = 1;
        int facesPerRoom = 0;
        int roleSlot[5] = { -1, -1, -1, -1, -1 };
    } m_interior;

    rhi::ProgramHandle m_shader;
    rhi::UniformLocation m_locView = -1;
    rhi::UniformLocation m_locProjection = -1;
    bool m_runtimeVerboseLogging = false;
};
