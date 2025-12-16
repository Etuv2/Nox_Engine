#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <vector>

// Forward declarations
class FrameBuffer;

/**
 * SSAOPass generates screen-space ambient occlusion:
 * - 192-sample kernel with hemisphere distribution
 * - 4x4 noise texture for rotation
 * - GL_R8 occlusion buffer
 * - Bilateral blur in 2 passes
 */
class SSAOPass : public RenderPass {
public:
    SSAOPass();
    ~SSAOPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    // Provide SSAO result texture for LightingPass
    GLuint GetSSAOTexture() const;

private:
    void GenerateKernel();
    void GenerateNoise();
    void RenderSSAO(RenderContext& ctx);
    void BilateralBlur(RenderContext& ctx);

    GLuint m_ssaoShader = 0;
    GLuint m_blurShader = 0;
    GLuint m_noiseTex = 0;
    std::vector<glm::vec3> m_kernel;
    
    std::unique_ptr<FrameBuffer> m_ssaoFBO;
    std::unique_ptr<FrameBuffer> m_blurFBO[2];
};
