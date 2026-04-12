#pragma once

#include "../RenderPass.h"
#include "../Texture.h"
#include <memory>

class ComputeShader;
struct RenderContext;
class SceneGraph;
class Camera;
class DirectionalLight;
class Skybox;

class SSGIPass : public RenderPass {
public:
    struct Config {
        float workingScale = 0.5f;
        float raymarchScale = 0.25f;
        int maxRaySteps = 32;
    };

    SSGIPass() = default;
    ~SSGIPass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
        const std::shared_ptr<SceneGraph>& sceneGraph,
        const std::shared_ptr<Camera>& camera,
        const std::shared_ptr<DirectionalLight>& dirLight,
        const std::shared_ptr<Skybox>& skybox) override;

    void CaptureHistory(RenderContext& ctx);
    GLuint GetSSGITexture() const { return m_ssgiTex ? m_ssgiTex->ID() : 0; }
    void SetConfig(const Config& config) { m_config = config; }
    const Config& GetConfig() const { return m_config; }

private:
    std::unique_ptr<ComputeShader> m_csDirections;
    std::unique_ptr<ComputeShader> m_csRaymarch;
    std::unique_ptr<ComputeShader> m_csBilateral;
    std::unique_ptr<ComputeShader> m_csUpsample;
    std::unique_ptr<ComputeShader> m_csTemporal;
    std::unique_ptr<ComputeShader> m_csFinalUpsample;

    TexturePtr m_dirTex;
    TexturePtr m_ssgiRaw;
    TexturePtr m_ssgiQuarterBlur;
    TexturePtr m_ssgiBlur;
    TexturePtr m_ssgiWork;
    TexturePtr m_ssgiTex;
    TexturePtr m_historyColor;
    TexturePtr m_historySSGI;

    int m_w = 0;
    int m_h = 0;
    int m_hw = 0;
    int m_hh = 0;
    int m_qw = 0;
    int m_qh = 0;
    int m_frameIndex = 0;

    Config m_config{};

    void runDirections(RenderContext& ctx);
    void runRaymarch(RenderContext& ctx, const std::shared_ptr<Camera>& camera, const std::shared_ptr<Skybox>& skybox);
    void runBilateral(RenderContext& ctx);
    void runUpsample(RenderContext& ctx);
    void runTemporal(RenderContext& ctx);
    void runFinalUpsample(RenderContext& ctx);
};
