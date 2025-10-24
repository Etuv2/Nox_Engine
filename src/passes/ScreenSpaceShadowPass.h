#pragma once
#include "../RenderPass.h"
#include "../ComputeShader.h"
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <memory>

/**
 * Screen-Space Shadow Pass (article approach — view-space ray march).
 * Produces a single-channel visibility texture [0..1] (0=shadow, 1=lit).
 */
class ScreenSpaceShadowPass : public RenderPass {
public:
    ScreenSpaceShadowPass();
    ~ScreenSpaceShadowPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;

    void Execute(RenderContext& ctx,
        const std::shared_ptr<SceneGraph>& sceneGraph,
        const std::shared_ptr<Camera>& camera,
        const std::shared_ptr<DirectionalLight>& dirLight,
        const std::shared_ptr<Skybox>& skybox) override;

    GLuint GetShadowTexture() const { return m_shadowTex; }

    struct Config {
        int   steps = 64;
        float rayLength = 1.2f;
        float thickness = 0.05f;
        float edgeFade = 0.002f;
        bool  jitter = true;      
        float jitterAmount = 0.35f;
        int   debugMode = 0;      
        bool  enableBilateralBlur = true;  
        int   blurRadius = 1;
        float depthSensitivity = 0.08f;
    };

    void SetConfig(const Config& c) { m_cfg = c; }
    const Config& GetConfig() const { return m_cfg; }

private:
    std::unique_ptr<ComputeShader> m_cs;
    GLuint m_shadowTex = 0;    // R8
    GLuint m_paramsUBO = 0;

    int m_width = 0, m_height = 0;
    uint32_t m_frameIndex = 0;

    Config m_cfg{};

    void createOutput(int w, int h);
    void createUBO();
    void updateUBO(const glm::vec3& lightDirVS,
        const glm::mat4& projection);
};
