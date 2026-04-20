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

class IndirectDiffusePass : public RenderPass {
public:
    struct Config {
        float workingScale = 0.25f;
        int maxDepthMip = 6;
    };

    IndirectDiffusePass() = default;
    ~IndirectDiffusePass() override;

    bool Initialize(RenderContext& context) override;
    void Resize(RenderContext& context, int newWidth, int newHeight) override;
    void Execute(RenderContext& ctx,
        const std::shared_ptr<SceneGraph>& sceneGraph,
        const std::shared_ptr<Camera>& camera,
        const std::shared_ptr<DirectionalLight>& dirLight,
        const std::shared_ptr<Skybox>& skybox) override;

    GLuint GetIndirectDiffuseTexture() const { return m_indirectDiffuseTex ? m_indirectDiffuseTex->ID() : 0; }
    GLuint GetDebugTexture() const { return m_debugOutput ? m_debugOutput->ID() : 0; }
    void SetBounceableRadianceTexture(GLuint texture) { m_bounceableRadianceFull = texture; }
    void SetConfig(const Config& config) { m_config = config; }
    const Config& GetConfig() const { return m_config; }

private:
    std::unique_ptr<ComputeShader> m_csDepthPrefilter;
    std::unique_ptr<ComputeShader> m_csDepthPyramid;
    std::unique_ptr<ComputeShader> m_csNormalBuild;
    std::unique_ptr<ComputeShader> m_csRadiance;
    std::unique_ptr<ComputeShader> m_csHorizonGather;
    std::unique_ptr<ComputeShader> m_csTemporal;
    std::unique_ptr<ComputeShader> m_csBilateral;
    std::unique_ptr<ComputeShader> m_csFinalUpsample;

    TexturePtr m_depthLinearQuarter;
    TexturePtr m_normalQuarter;
    TexturePtr m_bounceableRadianceQuarter;
    TexturePtr m_indirectRaw;
    TexturePtr m_directionalRaw;
    TexturePtr m_horizonDebug;
    TexturePtr m_sectorDebug;

    TexturePtr m_indirectTemporal;
    TexturePtr m_directionalTemporal;
    TexturePtr m_temporalDebug;

    TexturePtr m_indirectDenoiseStage1;
    TexturePtr m_directionalDenoiseStage1;

    TexturePtr m_indirectDenoised;
    TexturePtr m_directionalDenoised;

    TexturePtr m_indirectDiffuseTex;
    TexturePtr m_debugOutput;

    TexturePtr m_historyResolvedGI;
    TexturePtr m_historyIndirect;
    TexturePtr m_historyDirectional;
    TexturePtr m_historyDepthQuarter;
    TexturePtr m_historyNormalFull;
    GLuint m_bounceableRadianceFull = 0;
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
};
