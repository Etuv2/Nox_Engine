#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>

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

    GLuint GetBloomResult() const;

private:
    void ExtractBrightPixels(RenderContext& ctx, GLuint sourceTex);
    void Downsample(RenderContext& ctx);
    void Upsample(RenderContext& ctx);

    GLuint m_extractShader = 0;
    GLuint m_kawaseShader = 0;
    GLuint m_upsampleShader = 0;

    std::unique_ptr<FrameBuffer> m_extractFBO;
    std::unique_ptr<FrameBuffer> m_downsampleFBO[4];
    std::unique_ptr<FrameBuffer> m_upsampleFBO[4];
    bool m_runtimeVerboseLogging = false;
};
