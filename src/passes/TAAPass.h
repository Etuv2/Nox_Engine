#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include <glm/glm.hpp>

class FrameBuffer;

/**
 * TAAPass performs temporal anti-aliasing:
 * - Renders velocity buffer using prev matrices
 * - Resolves current frame with history
 * - Updates history buffer
 * - Maintains jitter state
 */
class TAAPass : public RenderPass {
public:
    TAAPass();
    ~TAAPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    void PrepareJitter(int pattern);
    void ExecuteVelocity(RenderContext& ctx,
                         const std::shared_ptr<SceneGraph>& sceneGraph,
                         const std::shared_ptr<Camera>& camera);
    void ExecuteResolve(RenderContext& ctx);

    void ResetHistory() { m_historyValid = false; m_frameIndex = 0; }
    int GetFrameIndex() const { return m_frameIndex; }
    glm::vec2 GetCurrentJitter() const { return m_jitter; }

    // Expose current FBO for SSGI to use as history source
    FrameBuffer* GetCurrentFBO() const { return m_currentFBO.get(); }

private:
    void RenderVelocity(RenderContext& ctx, const std::shared_ptr<SceneGraph>& sceneGraph,
                       const std::shared_ptr<Camera>& camera);
    void ResolveTemporalAntiAliasing(RenderContext& ctx);
    glm::vec2 GetJitter(int frameIndex, int pattern);
    glm::mat4 RemoveProjectionJitter(const glm::mat4& projection, const glm::vec2& jitterPixels, const RenderContext& ctx) const;

    rhi::ProgramHandle m_velocityShader;
    rhi::ProgramHandle m_resolveShader;


    struct VelocityUniforms {
        rhi::UniformLocation view = -1;
        rhi::UniformLocation projection = -1;
        rhi::UniformLocation prevView = -1;
        rhi::UniformLocation prevProjection = -1;
    };

    struct ResolveUniforms {
        rhi::UniformLocation currentFrame = -1;
        rhi::UniformLocation historyFrame = -1;
        rhi::UniformLocation velocityBuffer = -1;
        rhi::UniformLocation depthBuffer = -1;
        rhi::UniformLocation normalBuffer = -1;
        rhi::UniformLocation historyDepthBuffer = -1;
        rhi::UniformLocation historyNormalBuffer = -1;
        rhi::UniformLocation blendFactor = -1;
        rhi::UniformLocation varianceThreshold = -1;
        rhi::UniformLocation lumaWeight = -1;
        rhi::UniformLocation useYCoCg = -1;
        rhi::UniformLocation historyValid = -1;
        rhi::UniformLocation screenSize = -1;
        rhi::UniformLocation depthThreshold = -1;
        rhi::UniformLocation normalThreshold = -1;
        rhi::UniformLocation edgeThreshold = -1;
        rhi::UniformLocation reactiveMaskStrength = -1;
    };

    VelocityUniforms m_velocityUniforms;
    ResolveUniforms m_resolveUniforms;

    static constexpr bool VerboseLogging = false;
    bool m_runtimeVerboseLogging = false;

    std::unique_ptr<FrameBuffer> m_velocityFBO;
    std::unique_ptr<FrameBuffer> m_currentFBO;
    std::unique_ptr<FrameBuffer> m_historyFBO;
    rhi::TextureHandle m_historyDepthTex;
    rhi::TextureHandle m_historyNormalTex;

    glm::vec2 m_jitter{0.0f};
    glm::vec2 m_prevJitter{0.0f};
    int m_frameIndex = 0;
    bool m_historyValid = false;

    void RecreateHistoryValidationTextures(int width, int height);
    void DestroyHistoryValidationTextures();
};
