#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <vector>

class FrameBuffer;

class SSAOPass : public RenderPass {
public:
    struct Config {
        float resolutionScale = 0.5f;
        float temporalBlend = 0.12f;
        float depthThreshold = 0.02f;
        float normalThreshold = 0.15f;
        int sampleCount = 32;
    };

    SSAOPass();
    ~SSAOPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
                 const std::shared_ptr<SceneGraph>& sceneGraph,
                 const std::shared_ptr<Camera>& camera,
                 const std::shared_ptr<DirectionalLight>& dirLight,
                 const std::shared_ptr<Skybox>& skybox) override;

    GLuint GetSSAOTexture() const;
    void SetConfig(const Config& config) { m_config = config; }
    const Config& GetConfig() const { return m_config; }

private:
    void GenerateKernel();
    void GenerateNoise();
    void RenderSSAO(RenderContext& ctx);
    void ResolveSSAO(RenderContext& ctx);

    GLuint m_ssaoShader = 0;
    GLuint m_blurShader = 0;
    GLuint m_noiseTex = 0;
    std::vector<glm::vec3> m_kernel;

    Config m_config{};
    int m_renderWidth = 0;
    int m_renderHeight = 0;
    bool m_historyValid = false;

    std::unique_ptr<FrameBuffer> m_ssaoFBO;
    std::unique_ptr<FrameBuffer> m_historyFBO;
    std::unique_ptr<FrameBuffer> m_resolveFBO;
};
