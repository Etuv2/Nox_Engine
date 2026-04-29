#pragma once

#include "../RenderPass.h"
#include "../Texture.h"
#include <GL/glew.h>
#include <memory>

class ComputeShader;

class SurfelIndirectDiffusePass : public RenderPass {
public:
    struct Config {
        int neighborRadius = 1;
        int maxCandidates = 96;
        int maxAccepted = 24;
        float fallbackStrength = 0.65f;
        bool disableRadialDepthReject = false;
        bool disableNormalReject = false;
        bool disableConfidenceReject = false;
        bool disableFallback = false;
    };

    SurfelIndirectDiffusePass() = default;
    ~SurfelIndirectDiffusePass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
        const std::shared_ptr<SceneGraph>& sceneGraph,
        const std::shared_ptr<Camera>& camera,
        const std::shared_ptr<DirectionalLight>& dirLight,
        const std::shared_ptr<Skybox>& skybox) override;

    GLuint GetIrradianceTexture() const { return m_irradiance ? m_irradiance->ID() : 0; }
    GLuint GetDebugTexture() const { return m_debug ? m_debug->ID() : 0; }
    const Config& GetConfig() const { return m_config; }
    void SetConfig(const Config& config) { m_config = config; }

private:
    void AllocateTextures(int width, int height);

    std::unique_ptr<ComputeShader> m_gatherShader;
    TexturePtr m_irradiance;
    TexturePtr m_history;
    TexturePtr m_debug;
    int m_width = 0;
    int m_height = 0;
    bool m_hasHistory = false;
    Config m_config{};
};
