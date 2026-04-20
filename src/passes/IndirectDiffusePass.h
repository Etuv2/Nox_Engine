#pragma once

#include "../RenderPass.h"
#include "../Texture.h"
#include <glm/vec4.hpp>
#include <memory>
#include <string>

class ComputeShader;
struct RenderContext;
class SceneGraph;
class Camera;
class DirectionalLight;
class Skybox;

class IndirectDiffusePass : public RenderPass {
public:
    enum DebugStage {
        Disabled = 0,
        DepthQuarter = 1,
        NormalsQuarter = 2,
        BounceableRadiance = 3,
        SliceIntervals = 4,
        SectorCoverage = 5,
        NewSectorCount = 6,
        RawIndirect = 7,
        RawAO = 8,
        HistoryReprojected = 9,
        HistoryConfidence = 10,
        HistoryRejected = 11,
        BounceReinjection = 12,
        Denoise1 = 13,
        Denoise2 = 14,
        UpscaledIndirect = 15,
        FinalIndirectOnly = 16,
        RadianceSourceCurrent = 17,
        RadianceSourceReinjection = 18,
        RadianceSourceFinal = 19,
        TemporalHistoryRaw = 20,
        TemporalHistoryClamped = 21,
        TemporalResolved = 22,
        DenoiseWeights = 23,
        ContributionProvenance = 24
    };

    struct ProbeSample {
        glm::vec4 value{ 0.0f };
        glm::vec4 aux{ 0.0f };
        float luma = 0.0f;
        bool valid = false;
    };

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
    static const char* GetDebugStageLabel(int stage);
    static const char* GetDebugStageMeaning(int stage);
    static bool IsQuarterResolutionStage(int stage);
    static bool IsHdrColorStage(int stage);
    bool ExportValidationStages(const std::string& directory) const;
    bool ReadValidationProbe(int stage, int pixelX, int pixelY, ProbeSample& outSample) const;

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
    TexturePtr m_radianceCurrentDebug;
    TexturePtr m_radianceReinjectionDebug;
    TexturePtr m_radianceProvenanceDebug;
    TexturePtr m_indirectRaw;
    TexturePtr m_directionalRaw;
    TexturePtr m_horizonDebug;
    TexturePtr m_sectorDebug;

    TexturePtr m_indirectTemporal;
    TexturePtr m_directionalTemporal;
    TexturePtr m_temporalDebug;
    TexturePtr m_temporalHistoryRawDebug;
    TexturePtr m_temporalHistoryClampedDebug;

    TexturePtr m_indirectDenoiseStage1;
    TexturePtr m_directionalDenoiseStage1;
    TexturePtr m_denoiseWeightDebug;

    TexturePtr m_indirectDenoised;
    TexturePtr m_directionalDenoised;

    TexturePtr m_indirectDiffuseTex;
    TexturePtr m_debugOutput;
    TexturePtr m_upscaledIndirectDebug;
    TexturePtr m_finalIndirectDebug;

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
    GLuint getTextureForDebugStage(int stage, bool& outFullRes) const;
    bool exportTexture(GLuint texture, bool fullRes, bool hdrColor, const std::string& filepath) const;
    bool readTextureProbe(GLuint texture, bool fullRes, ProbeSample& outSample, int pixelX, int pixelY) const;
};
