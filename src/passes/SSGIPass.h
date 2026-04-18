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
        float quarterScale = 0.25f;
        int maxDepthMip = 6;
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
    std::unique_ptr<ComputeShader> m_csDepthPrefilter;
    std::unique_ptr<ComputeShader> m_csRadiance;
    std::unique_ptr<ComputeShader> m_csHorizonGather;
    std::unique_ptr<ComputeShader> m_csTemporal;
    std::unique_ptr<ComputeShader> m_csBilateral;
    std::unique_ptr<ComputeShader> m_csFinalUpsample;

    TexturePtr m_depthLinearQuarter;
    TexturePtr m_normalQuarter;
    TexturePtr m_radianceTex;
    TexturePtr m_indirectRaw;
    TexturePtr m_directionalRaw;
    TexturePtr m_horizonDebug;
    TexturePtr m_sectorDebug;

    TexturePtr m_indirectTemporal;
    TexturePtr m_directionalTemporal;
    TexturePtr m_temporalDebug;

    TexturePtr m_indirectDenoised;
    TexturePtr m_directionalDenoised;

    TexturePtr m_ssgiTex;
    TexturePtr m_debugOutput;

    TexturePtr m_historyColor;
    TexturePtr m_historyIndirect;
    TexturePtr m_historyDirectional;
    GLuint m_historyDepthTex = 0;
    GLuint m_historyNormalTex = 0;

    int m_w = 0;
    int m_h = 0;
    int m_qw = 0;
    int m_qh = 0;
    int m_depthMipCount = 1;
    int m_frameIndex = 0;

    Config m_config{};

    void runDepthPrefilter(RenderContext& ctx);
    void runRadiance(RenderContext& ctx);
    void runHorizonGather(RenderContext& ctx, const std::shared_ptr<Camera>& camera);
    void runTemporal(RenderContext& ctx);
    void runBilateral(RenderContext& ctx);
    void runFinalUpsample(RenderContext& ctx);
    void resizeTemporalHistoryBuffers();
};
