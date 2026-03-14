#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
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

    GLuint m_velocityShader = 0;
    GLuint m_resolveShader = 0;


    struct VelocityUniforms {
        GLint view = -1;
        GLint projection = -1;
        GLint prevView = -1;
        GLint prevProjection = -1;
        GLint jitter = -1;
        GLint prevJitter = -1;
        GLint screenSize = -1;
    };

    struct ResolveUniforms {
        GLint currentFrame = -1;
        GLint historyFrame = -1;
        GLint velocityBuffer = -1;
        GLint depthBuffer = -1;
        GLint gNormal = -1;
        GLint blendFactor = -1;
        GLint varianceThreshold = -1;
        GLint lumaWeight = -1;
        GLint useYCoCg = -1;
        GLint historyValid = -1;
        GLint screenSize = -1;
        GLint jitter = -1;
        GLint depthThreshold = -1;
        GLint normalThreshold = -1;
        GLint edgeThreshold = -1;
        GLint reactiveMaskStrength = -1;
    };

    VelocityUniforms m_velocityUniforms;
    ResolveUniforms m_resolveUniforms;

    static constexpr bool VerboseLogging = false;
    bool m_runtimeVerboseLogging = false;

    std::unique_ptr<FrameBuffer> m_velocityFBO;
    std::unique_ptr<FrameBuffer> m_currentFBO;
    std::unique_ptr<FrameBuffer> m_historyFBO;

    glm::vec2 m_jitter{0.0f};
    glm::vec2 m_prevJitter{0.0f};
    int m_frameIndex = 0;
    bool m_historyValid = false;
};
