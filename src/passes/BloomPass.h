#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"

// Forward declarations
class FrameBuffer;

/**
 * BloomPass performs bloom effect:
 * - Extract bright pixels above threshold
 * - Downsample with Kawase blur (4 levels)
 * - Upsample and blend (4 levels)
 * Result stored in upsample[0] for post-process to composite
 */
class BloomPass : public RenderPass {
public:
    BloomPass();
    ~BloomPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    rhi::TextureHandle GetBloomResult() const;

private:
    void ExtractBrightPixels(RenderContext& ctx, rhi::TextureHandle sourceTex);
    void Downsample(RenderContext& ctx);
    void Upsample(RenderContext& ctx);

    rhi::ProgramHandle m_extractShader;
    rhi::ProgramHandle m_kawaseShader;
    rhi::ProgramHandle m_upsampleShader;

    std::unique_ptr<FrameBuffer> m_extractFBO;
    std::unique_ptr<FrameBuffer> m_downsampleFBO[4];
    std::unique_ptr<FrameBuffer> m_upsampleFBO[4];
    bool m_runtimeVerboseLogging = false;
};
