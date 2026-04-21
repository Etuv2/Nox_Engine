#include "IndirectDiffusePass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../ComputeShader.h"
#include "../Texture.h"
#include "../stb_image_write.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <vector>

namespace {
float Luma(const glm::vec3& c) {
    return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
}

glm::vec3 DebugTonemap(const glm::vec3& hdr, float exposure) {
    const glm::vec3 scaled = glm::max(hdr, glm::vec3(0.0f)) * std::max(exposure, 0.0f);
    return scaled / (glm::vec3(1.0f) + scaled);
}
}

const char* IndirectDiffusePass::GetDebugStageLabel(int stage) {
    switch (stage) {
    case DepthQuarter: return "DepthQuarter";
    case NormalsQuarter: return "NormalsQuarter";
    case BounceableRadiance: return "BounceableRadiance";
    case SliceIntervals: return "SliceIntervals";
    case SectorCoverage: return "SectorCoverage";
    case NewSectorCount: return "NewSectorCount";
    case RawIndirect: return "RawIndirect";
    case RawAO: return "RawAO";
    case HistoryReprojected: return "HistoryReprojected";
    case HistoryConfidence: return "HistoryConfidence";
    case HistoryRejected: return "HistoryRejected";
    case BounceReinjection: return "BounceReinjection";
    case Denoise1: return "Denoise1";
    case Denoise2: return "Denoise2";
    case UpscaledIndirect: return "UpscaledIndirect";
    case FinalIndirectOnly: return "FinalIndirectOnly";
    case RadianceSourceCurrent: return "RadianceSourceCurrent";
    case RadianceSourceReinjection: return "RadianceSourceReinjection";
    case RadianceSourceFinal: return "RadianceSourceFinal";
    case TemporalHistoryRaw: return "TemporalHistoryRaw";
    case TemporalHistoryClamped: return "TemporalHistoryClamped";
    case TemporalResolved: return "TemporalResolved";
    case DenoiseWeights: return "DenoiseWeights";
    case ContributionProvenance: return "ContributionProvenance";
    default: return "Disabled";
    }
}

const char* IndirectDiffusePass::GetDebugStageMeaning(int stage) {
    switch (stage) {
    case DepthQuarter: return "Quarter-res linear view-space depth";
    case NormalsQuarter: return "Quarter-res canonical view-space normals";
    case BounceableRadiance: return "Quarter-res final radiance source used by gather";
    case SliceIntervals: return "First-slice angular interval debug";
    case SectorCoverage: return "Gather sector coverage summary";
    case NewSectorCount: return "Newly covered sector ratio";
    case RawIndirect: return "Quarter-res transported RGB before temporal";
    case RawAO: return "Quarter-res scalar visibility/AO from gather";
    case HistoryReprojected: return "Quarter-res temporal resolved RGB";
    case HistoryConfidence: return "Quarter-res temporal confidence scalar";
    case HistoryRejected: return "Quarter-res temporal rejection/disocclusion scalar";
    case BounceReinjection: return "Quarter-res bounded reinjection contribution";
    case Denoise1: return "Quarter-res first denoise stage";
    case Denoise2: return "Quarter-res second denoise stage";
    case UpscaledIndirect: return "Full-res upscaled indirect RGB";
    case FinalIndirectOnly: return "Full-res final indirect contribution used in lighting";
    case RadianceSourceCurrent: return "Quarter-res current-frame source before reinjection";
    case RadianceSourceReinjection: return "Quarter-res prior indirect reinjection term";
    case RadianceSourceFinal: return "Quarter-res final source after reinjection";
    case TemporalHistoryRaw: return "Quarter-res reprojected history before clamp";
    case TemporalHistoryClamped: return "Quarter-res history after neighborhood clamp";
    case TemporalResolved: return "Quarter-res temporally resolved indirect output";
    case DenoiseWeights: return "Quarter-res denoise weight / acceptance debug";
    case ContributionProvenance: return "Quarter-res source construction agreement/provenance";
    default: return "GI debug disabled";
    }
}

bool IndirectDiffusePass::IsQuarterResolutionStage(int stage) {
    return !(stage == Disabled || stage == UpscaledIndirect || stage == FinalIndirectOnly);
}

bool IndirectDiffusePass::IsHdrColorStage(int stage) {
    switch (stage) {
    case BounceableRadiance:
    case RawIndirect:
    case HistoryReprojected:
    case Denoise1:
    case Denoise2:
    case UpscaledIndirect:
    case FinalIndirectOnly:
    case RadianceSourceCurrent:
    case RadianceSourceReinjection:
    case RadianceSourceFinal:
    case TemporalHistoryRaw:
    case TemporalHistoryClamped:
    case TemporalResolved:
        return true;
    default:
        return false;
    }
}

IndirectDiffusePass::~IndirectDiffusePass() = default;

bool IndirectDiffusePass::Initialize(RenderContext& ctx) {
    std::cout << "[IndirectDiffusePass] Initializing visibility-bitmask indirect diffuse pipeline..." << std::endl;

    m_csDepthPrefilter = std::make_unique<ComputeShader>();
    m_csDepthPyramid = std::make_unique<ComputeShader>();
    m_csNormalBuild = std::make_unique<ComputeShader>();
    m_csRadiance = std::make_unique<ComputeShader>();
    m_csHorizonGather = std::make_unique<ComputeShader>();
    m_csTemporal = std::make_unique<ComputeShader>();
    m_csBilateral = std::make_unique<ComputeShader>();
    m_csFinalUpsample = std::make_unique<ComputeShader>();

    if (!m_csDepthPrefilter->CreateFromFile("shaders/indirect_diffuse_depth_prepare_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_depth_prepare_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csDepthPyramid->CreateFromFile("shaders/indirect_diffuse_depth_pyramid_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_depth_pyramid_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csNormalBuild->CreateFromFile("shaders/indirect_diffuse_full_normals_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_full_normals_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csRadiance->CreateFromFile("shaders/indirect_diffuse_radiance_downsample_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_radiance_downsample_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csHorizonGather->CreateFromFile("shaders/indirect_diffuse_gather_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_gather_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csTemporal->CreateFromFile("shaders/indirect_diffuse_temporal_resolve_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_temporal_resolve_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csBilateral->CreateFromFile("shaders/indirect_diffuse_denoise_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_denoise_comp.glsl" << std::endl;
        return false;
    }
    if (!m_csFinalUpsample->CreateFromFile("shaders/indirect_diffuse_upsample_comp.glsl")) {
        std::cerr << "[IndirectDiffusePass] Failed to compile indirect_diffuse_upsample_comp.glsl" << std::endl;
        return false;
    }

    Resize(ctx, ctx.width, ctx.height);
    std::cout << "[IndirectDiffusePass] Visibility-bitmask indirect diffuse initialization complete" << std::endl;
    return true;
}

void IndirectDiffusePass::Resize(RenderContext& ctx, int w, int h) {
    const float quarterScale = std::clamp(m_config.workingScale, 0.25f, 0.25f);

    const int desiredQuarterWidth = std::max(1, static_cast<int>(std::round(w * quarterScale)));
    const int desiredQuarterHeight = std::max(1, static_cast<int>(std::round(h * quarterScale)));

    if (w == m_w && h == m_h && desiredQuarterWidth == m_qw && desiredQuarterHeight == m_qh) {
        return;
    }

    m_w = w;
    m_h = h;
    m_qw = desiredQuarterWidth;
    m_qh = desiredQuarterHeight;
    m_depthMipCount = std::max(1, static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(m_qw, m_qh))))) + 1);

    std::cout << "[IndirectDiffusePass] Resize " << m_w << "x" << m_h
              << " (quarter: " << m_qw << "x" << m_qh
              << ", depth mips: " << m_depthMipCount << ")" << std::endl;

    m_depthLinearQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_R16F)
        .Format(GL_RED).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .GenerateMipmaps(true)
        .Build();

    m_normalQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_RG16F)
        .Format(GL_RG).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    m_bounceableRadianceQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_RGBA16F)
        .Format(GL_RGBA).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .GenerateMipmaps(true)
        .Build();
    m_radianceCurrentDebug = TextureFactory::CreateHDR(m_qw, m_qh);
    m_radianceReinjectionDebug = TextureFactory::CreateHDR(m_qw, m_qh);
    m_radianceProvenanceDebug = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectRaw = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalRaw = TextureFactory::CreateHDR(m_qw, m_qh);
    m_horizonDebug = TextureFactory::CreateHDR(m_qw, m_qh);
    m_sectorDebug = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectTemporal = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalTemporal = TextureFactory::CreateHDR(m_qw, m_qh);
    m_temporalDebug = TextureFactory::CreateHDR(m_qw, m_qh);
    m_temporalHistoryRawDebug = TextureFactory::CreateHDR(m_qw, m_qh);
    m_temporalHistoryClampedDebug = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectDenoiseStage1 = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalDenoiseStage1 = TextureFactory::CreateHDR(m_qw, m_qh);
    m_denoiseWeightDebug = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectDenoised = TextureFactory::CreateHDR(m_qw, m_qh);
    m_directionalDenoised = TextureFactory::CreateHDR(m_qw, m_qh);

    m_indirectDiffuseTex = TextureFactory::CreateHDR(m_w, m_h);
    m_debugOutput = TextureFactory::CreateHDR(m_w, m_h);
    m_upscaledIndirectDebug = TextureFactory::CreateHDR(m_w, m_h);
    m_finalIndirectDebug = TextureFactory::CreateHDR(m_w, m_h);

    m_historyResolvedGI = TextureFactory::CreateHDR(m_w, m_h);
    m_historyIndirect = TextureFactory::CreateHDR(m_qw, m_qh);
    m_historyDirectional = TextureFactory::CreateHDR(m_qw, m_qh);
    m_historyDepthQuarter = Texture::Builder::Texture2D(m_qw, m_qh, GL_R16F)
        .Format(GL_RED).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .GenerateMipmaps(true)
        .Build();
    m_historyNormalFull = Texture::Builder::Texture2D(m_qw, m_qh, GL_RG16F)
        .Format(GL_RG).DataType(GL_HALF_FLOAT)
        .FilterMode(GL_LINEAR, GL_LINEAR)
        .WrapMode(GL_CLAMP_TO_EDGE)
        .Build();

    std::vector<float> clearQuarter(static_cast<size_t>(m_qw) * static_cast<size_t>(m_qh) * 4, 0.0f);
    m_historyIndirect->Upload2D(0, 0, 0, m_qw, m_qh, GL_RGBA, GL_FLOAT, clearQuarter.data());
    m_historyDirectional->Upload2D(0, 0, 0, m_qw, m_qh, GL_RGBA, GL_FLOAT, clearQuarter.data());
    std::vector<float> clearFull(static_cast<size_t>(m_w) * static_cast<size_t>(m_h) * 4, 0.0f);
    m_historyResolvedGI->Upload2D(0, 0, 0, m_w, m_h, GL_RGBA, GL_FLOAT, clearFull.data());

    const float invalidLinearDepth = 65504.0f;
    glClearTexImage(m_historyDepthQuarter->ID(), 0, GL_RED, GL_FLOAT, &invalidLinearDepth);

    const float normalClear[4] = { 0.5f, 0.5f, 0.0f, 0.0f };
    glClearTexImage(m_historyNormalFull->ID(), 0, GL_RG, GL_FLOAT, normalClear);
    m_historyDepthQuarter->GenerateMipmaps();
}

void IndirectDiffusePass::Execute(RenderContext& ctx,
    const std::shared_ptr<SceneGraph>&,
    const std::shared_ptr<Camera>& camera,
    const std::shared_ptr<DirectionalLight>&,
    const std::shared_ptr<Skybox>&) {
    if (!ctx.enableIndirectDiffuse || !ctx.gbufferFBO || !camera || m_bounceableRadianceFull == 0) {
        return;
    }

    Resize(ctx, ctx.width, ctx.height);

    runDepthPrefilter(ctx);
    runRadiance(ctx);
    runHorizonGather(ctx, camera);
    runTemporal(ctx);

    runBilateral(ctx);

    runFinalUpsample(ctx);

    glCopyImageSubData(m_indirectDiffuseTex->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyResolvedGI->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_w, m_h, 1);
    ctx.indirectDiffuseHistoryReset = false;
}

void IndirectDiffusePass::runDepthPrefilter(RenderContext& ctx) {
    glUseProgram(m_csDepthPrefilter->GetProgramID());

    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture());
    glBindImageTexture(2, m_depthLinearQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
    glUniformMatrix4fv(glGetUniformLocation(m_csDepthPrefilter->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    for (int mip = 0; mip < m_depthMipCount - 1; ++mip) {
        const int dstWidth = std::max(1, m_qw >> (mip + 1));
        const int dstHeight = std::max(1, m_qh >> (mip + 1));

        glUseProgram(m_csDepthPyramid->GetProgramID());
        glBindTextureUnit(0, m_depthLinearQuarter->ID());
        glBindImageTexture(1, m_depthLinearQuarter->ID(), mip + 1, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
        glUniform1i(glGetUniformLocation(m_csDepthPyramid->GetProgramID(), "srcMip"), mip);

        const GLuint mipGX = (dstWidth + 7) / 8;
        const GLuint mipGY = (dstHeight + 7) / 8;
        glDispatchCompute(mipGX, mipGY, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    glUseProgram(m_csNormalBuild->GetProgramID());
    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture());
    glBindImageTexture(1, m_normalQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
    glUniformMatrix4fv(glGetUniformLocation(m_csNormalBuild->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void IndirectDiffusePass::runRadiance(RenderContext& ctx) {
    glUseProgram(m_csRadiance->GetProgramID());

    glBindTextureUnit(0, m_bounceableRadianceFull);
    glBindTextureUnit(1, ctx.gbufferFBO->GetColorAttachment(4));
    glBindTextureUnit(2, m_historyResolvedGI->ID());
    glBindTextureUnit(3, m_depthLinearQuarter->ID());
    glBindTextureUnit(4, ctx.velocityTex);
    glBindTextureUnit(5, m_historyDepthQuarter->ID());
    glBindTextureUnit(6, m_historyNormalFull->ID());
    glBindTextureUnit(7, m_normalQuarter->ID());
    glBindTextureUnit(8, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(9, ctx.gbufferFBO->GetColorAttachment(0));
    glBindTextureUnit(10, ctx.gbufferFBO->GetColorAttachment(1));
    glBindImageTexture(0, m_bounceableRadianceQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_radianceCurrentDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(2, m_radianceReinjectionDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(3, m_radianceProvenanceDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform1i(glGetUniformLocation(m_csRadiance->GetProgramID(), "usePreviousIndirect"),
        (ctx.velocityTex != 0 && m_historyDepthQuarter && m_historyNormalFull && !ctx.indirectDiffuseHistoryReset) ? 1 : 0);
    const float reinjectionFeedback = ctx.indirectDiffuseValidationDisableReinjection
        ? 0.0f
        : std::clamp(ctx.indirectDiffuseBounceFeedback, 0.0f, 2.0f);
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "previousIndirectFeedback"), reinjectionFeedback);
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "depthReject"), std::clamp(ctx.indirectDiffuseDepthReject, 0.01f, 0.35f));
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "normalRejectCos"), std::clamp(1.0f - ctx.indirectDiffuseNormalReject, 0.55f, 0.99f));
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "disocclusionReject"), std::clamp(ctx.indirectDiffuseDepthReject * 1.5f, 0.02f, 0.25f));
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "sourceNormalRejectCos"), 0.68f);
    glUniform1f(glGetUniformLocation(m_csRadiance->GetProgramID(), "sourceAlbedoReject"), 1.15f);
    glUniformMatrix4fv(glGetUniformLocation(m_csRadiance->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    m_bounceableRadianceQuarter->GenerateMipmaps();
}

void IndirectDiffusePass::runHorizonGather(RenderContext& ctx, const std::shared_ptr<Camera>& camera) {
    (void)camera;
    glUseProgram(m_csHorizonGather->GetProgramID());

    glBindTextureUnit(0, m_depthLinearQuarter->ID());
    glBindTextureUnit(1, m_normalQuarter->ID());
    glBindTextureUnit(2, m_bounceableRadianceQuarter->ID());

    glBindImageTexture(3, m_indirectRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(4, m_directionalRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(5, m_horizonDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(6, m_sectorDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    const int sliceCount = std::clamp(ctx.indirectDiffuseSliceCount, 1, 8);
    const int stepCount = std::clamp(ctx.indirectDiffuseSamplesPerSlice, 1, 16);

    glUniform2f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "invQuarterSize"),
        1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform2f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "fullResolution"), float(m_w), float(m_h));
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "projScaleX"), ctx.proj[0][0]);
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "projScaleY"), ctx.proj[1][1]);
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "rayLength"), std::max(0.05f, ctx.indirectDiffuseRadiusVS));
    glUniform1f(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "thicknessVS"), std::max(0.001f, ctx.indirectDiffuseThicknessVS));
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "rayCount"), sliceCount);
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "stepCount"), stepCount);
    glUniform1i(glGetUniformLocation(m_csHorizonGather->GetProgramID(), "frameIndex"), m_frameIndex++);

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void IndirectDiffusePass::runTemporal(RenderContext& ctx) {
    if (ctx.indirectDiffuseValidationDisableTemporal || ctx.velocityTex == 0 || !m_historyNormalFull || !m_historyDepthQuarter || ctx.indirectDiffuseHistoryReset) {
        glCopyImageSubData(m_indirectRaw->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        glCopyImageSubData(m_directionalRaw->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);

        glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyIndirect->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyDirectional->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);

        const GLfloat temporalClear[4] = { 1.0f, 0.0f, 1.0f, 0.0f };
        glClearTexImage(m_temporalDebug->ID(), 0, GL_RGBA, GL_FLOAT, temporalClear);
        glClearTexImage(m_temporalHistoryRawDebug->ID(), 0, GL_RGBA, GL_FLOAT, temporalClear);
        glClearTexImage(m_temporalHistoryClampedDebug->ID(), 0, GL_RGBA, GL_FLOAT, temporalClear);

        glCopyImageSubData(m_depthLinearQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyDepthQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        m_historyDepthQuarter->GenerateMipmaps();

        glCopyImageSubData(m_normalQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_historyNormalFull->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        return;
    }

    glUseProgram(m_csTemporal->GetProgramID());

    glBindTextureUnit(0, m_indirectRaw->ID());
    glBindTextureUnit(1, m_directionalRaw->ID());
    glBindTextureUnit(2, m_historyIndirect->ID());
    glBindTextureUnit(3, m_historyDirectional->ID());
    glBindTextureUnit(4, ctx.velocityTex);
    glBindTextureUnit(5, m_depthLinearQuarter->ID());
    glBindTextureUnit(6, m_normalQuarter->ID());
    glBindTextureUnit(7, m_historyDepthQuarter->ID());
    glBindTextureUnit(8, m_historyNormalFull->ID());

    glBindImageTexture(0, m_indirectTemporal->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_directionalTemporal->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(2, m_temporalDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(3, m_temporalHistoryRawDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(4, m_temporalHistoryClampedDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform1i(glGetUniformLocation(m_csTemporal->GetProgramID(), "useHistory"), 1);
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "historyBlend"), std::clamp(1.0f - ctx.indirectDiffuseTemporalAlpha, 0.0f, 0.95f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "depthReject"), std::clamp(ctx.indirectDiffuseDepthReject, 0.01f, 0.35f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "normalRejectCos"), std::clamp(1.0f - ctx.indirectDiffuseNormalReject, 0.55f, 0.99f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "disocclusionReject"), std::clamp(ctx.indirectDiffuseDepthReject * 1.5f, 0.02f, 0.25f));
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "historyClampStrength"), 0.35f);
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "motionRejectPixels"), 32.0f);
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "minHistoryConfidence"), 0.01f);

    const GLuint gx = (m_qw + 7) / 8;
    const GLuint gy = (m_qh + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyIndirect->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
    glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyDirectional->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
    glCopyImageSubData(m_depthLinearQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyDepthQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
    m_historyDepthQuarter->GenerateMipmaps();

    glCopyImageSubData(m_normalQuarter->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_historyNormalFull->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
        m_qw, m_qh, 1);
}

void IndirectDiffusePass::runBilateral(RenderContext& ctx) {
    if (ctx.indirectDiffuseValidationDisableDenoise) {
        glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_indirectDenoiseStage1->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_directionalDenoiseStage1->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        glCopyImageSubData(m_indirectTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_indirectDenoised->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        glCopyImageSubData(m_directionalTemporal->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_directionalDenoised->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
            m_qw, m_qh, 1);
        const GLfloat denoiseClear[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
        glClearTexImage(m_denoiseWeightDebug->ID(), 0, GL_RGBA, GL_FLOAT, denoiseClear);
        return;
    }

    glUseProgram(m_csBilateral->GetProgramID());
    glBindTextureUnit(0, m_indirectTemporal->ID());
    glBindTextureUnit(1, m_directionalTemporal->ID());
    glBindTextureUnit(2, m_depthLinearQuarter->ID());
    glBindTextureUnit(3, m_normalQuarter->ID());
    glBindTextureUnit(4, m_temporalDebug->ID());
    glBindImageTexture(5, m_indirectDenoiseStage1->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(6, m_directionalDenoiseStage1->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(7, m_denoiseWeightDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glUniform2f(glGetUniformLocation(m_csBilateral->GetProgramID(), "invQuarterSize"), 1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform2f(glGetUniformLocation(m_csBilateral->GetProgramID(), "fullResolution"), float(m_w), float(m_h));
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "depthSigma"), std::clamp(ctx.indirectDiffuseDepthReject, 0.01f, 0.20f));
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "normalReject"), std::clamp(ctx.indirectDiffuseNormalReject, 0.04f, 0.24f));
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "denoiseStrength"), std::clamp(ctx.indirectDiffuseDenoiseStrength, 0.5f, 2.5f));
    glUniform1i(glGetUniformLocation(m_csBilateral->GetProgramID(), "kernelRadius"), 2);
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "confidencePower"), 1.6f);
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "lumaPhi"), 0.16f);
    glDispatchCompute((m_qw + 7) / 8, (m_qh + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glBindTextureUnit(0, m_indirectDenoiseStage1->ID());
    glBindTextureUnit(1, m_directionalDenoiseStage1->ID());
    glBindImageTexture(5, m_indirectDenoised->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(6, m_directionalDenoised->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glUniform1i(glGetUniformLocation(m_csBilateral->GetProgramID(), "kernelRadius"), 1);
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "confidencePower"), 1.2f);
    glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "lumaPhi"), 0.10f);
    glDispatchCompute((m_qw + 7) / 8, (m_qh + 7) / 8, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void IndirectDiffusePass::runFinalUpsample(RenderContext& ctx) {
    glUseProgram(m_csFinalUpsample->GetProgramID());

    glBindTextureUnit(0, m_indirectDenoiseStage1->ID());
    glBindTextureUnit(1, m_directionalDenoiseStage1->ID());
    glBindTextureUnit(2, m_indirectDenoised->ID());
    glBindTextureUnit(3, m_directionalDenoised->ID());
    glBindTextureUnit(4, m_indirectRaw->ID());
    glBindTextureUnit(5, m_directionalRaw->ID());
    glBindTextureUnit(6, m_depthLinearQuarter->ID());
    glBindTextureUnit(7, m_normalQuarter->ID());
    glBindTextureUnit(8, m_bounceableRadianceQuarter->ID());
    glBindTextureUnit(9, m_horizonDebug->ID());
    glBindTextureUnit(10, m_temporalDebug->ID());
    glBindTextureUnit(11, ctx.gbufferFBO->GetDepthTexture());
    glBindTextureUnit(12, ctx.gbufferFBO->GetColorAttachment(0));
    glBindTextureUnit(13, ctx.gbufferFBO->GetColorAttachment(1));
    glBindTextureUnit(14, m_sectorDebug->ID());
    glBindTextureUnit(15, m_indirectTemporal->ID());
    glBindTextureUnit(16, m_radianceCurrentDebug->ID());
    glBindTextureUnit(17, m_radianceReinjectionDebug->ID());
    glBindTextureUnit(18, m_radianceProvenanceDebug->ID());
    glBindTextureUnit(19, m_temporalHistoryRawDebug->ID());
    glBindTextureUnit(20, m_temporalHistoryClampedDebug->ID());
    glBindTextureUnit(21, m_denoiseWeightDebug->ID());
    glBindTextureUnit(22, ctx.gbufferFBO->GetColorAttachment(2));
    glBindTextureUnit(23, ctx.gbufferFBO->GetColorAttachment(6));

    glBindImageTexture(0, m_indirectDiffuseTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_debugOutput->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(2, m_upscaledIndirectDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(3, m_finalIndirectDebug->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    glUniform2f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invFullSize"), 1.0f / float(m_w), 1.0f / float(m_h));
    glUniform2f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invQuarterSize"), 1.0f / float(m_qw), 1.0f / float(m_qh));
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "projScaleX"), ctx.proj[0][0]);
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "projScaleY"), ctx.proj[1][1]);
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "upscaleSharpness"), std::clamp(ctx.indirectDiffuseUpscaleSharpness, 0.5f, 4.0f));
    glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "debugMode"), std::clamp(ctx.indirectDiffuseDebugStage, 0, 24));
    glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "indirectStrength"), std::max(ctx.indirectDiffuseStrength, 0.0f));
    glUniformMatrix4fv(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "view"), 1, GL_FALSE, glm::value_ptr(ctx.view));
    glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "normalsInWorldSpace"), 1);
    glUniformMatrix4fv(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invProj"), 1, GL_FALSE, glm::value_ptr(glm::inverse(ctx.proj)));

    const GLuint gx = (m_w + 7) / 8;
    const GLuint gy = (m_h + 7) / 8;
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

GLuint IndirectDiffusePass::getTextureForDebugStage(int stage, bool& outFullRes) const {
    outFullRes = !IsQuarterResolutionStage(stage);
    switch (stage) {
    case DepthQuarter: return m_depthLinearQuarter ? m_depthLinearQuarter->ID() : 0;
    case NormalsQuarter: return m_normalQuarter ? m_normalQuarter->ID() : 0;
    case BounceableRadiance:
    case RadianceSourceFinal:
        return m_bounceableRadianceQuarter ? m_bounceableRadianceQuarter->ID() : 0;
    case SliceIntervals: return m_sectorDebug ? m_sectorDebug->ID() : 0;
    case SectorCoverage:
    case NewSectorCount: return m_horizonDebug ? m_horizonDebug->ID() : 0;
    case RawIndirect:
    case RawAO: return m_indirectRaw ? m_indirectRaw->ID() : 0;
    case HistoryReprojected:
    case TemporalResolved: return m_indirectTemporal ? m_indirectTemporal->ID() : 0;
    case HistoryConfidence:
    case HistoryRejected: return m_temporalDebug ? m_temporalDebug->ID() : 0;
    case BounceReinjection:
    case RadianceSourceReinjection: return m_radianceReinjectionDebug ? m_radianceReinjectionDebug->ID() : 0;
    case Denoise1: return m_indirectDenoiseStage1 ? m_indirectDenoiseStage1->ID() : 0;
    case Denoise2: return m_indirectDenoised ? m_indirectDenoised->ID() : 0;
    case UpscaledIndirect:
        outFullRes = true;
        return m_upscaledIndirectDebug ? m_upscaledIndirectDebug->ID() : 0;
    case FinalIndirectOnly:
        outFullRes = true;
        return m_finalIndirectDebug ? m_finalIndirectDebug->ID() : 0;
    case RadianceSourceCurrent: return m_radianceCurrentDebug ? m_radianceCurrentDebug->ID() : 0;
    case TemporalHistoryRaw: return m_temporalHistoryRawDebug ? m_temporalHistoryRawDebug->ID() : 0;
    case TemporalHistoryClamped: return m_temporalHistoryClampedDebug ? m_temporalHistoryClampedDebug->ID() : 0;
    case DenoiseWeights: return m_denoiseWeightDebug ? m_denoiseWeightDebug->ID() : 0;
    case ContributionProvenance: return m_radianceProvenanceDebug ? m_radianceProvenanceDebug->ID() : 0;
    default:
        outFullRes = true;
        return m_indirectDiffuseTex ? m_indirectDiffuseTex->ID() : 0;
    }
}

bool IndirectDiffusePass::readTextureProbe(GLuint texture, bool fullRes, ProbeSample& outSample, int pixelX, int pixelY) const {
    if (texture == 0) {
        return false;
    }

    const int width = fullRes ? m_w : m_qw;
    const int height = fullRes ? m_h : m_qh;
    if (width <= 0 || height <= 0) {
        return false;
    }

    const int x = std::clamp(pixelX, 0, width - 1);
    const int y = std::clamp(pixelY, 0, height - 1);
    GLfloat sample[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glGetTextureSubImage(texture, 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof(sample), sample);

    outSample.value = glm::vec4(sample[0], sample[1], sample[2], sample[3]);
    outSample.luma = Luma(glm::vec3(outSample.value));
    outSample.valid = true;
    return true;
}

bool IndirectDiffusePass::ReadValidationProbe(int stage, int pixelX, int pixelY, ProbeSample& outSample) const {
    bool fullRes = false;
    const GLuint texture = getTextureForDebugStage(stage, fullRes);
    if (!readTextureProbe(texture, fullRes, outSample, pixelX, pixelY)) {
        return false;
    }

    // Populate aux with the most relevant secondary data for selected stages.
    if (stage == HistoryConfidence || stage == HistoryRejected || stage == HistoryReprojected || stage == TemporalResolved) {
        ProbeSample auxSample;
        if (readTextureProbe(m_temporalDebug ? m_temporalDebug->ID() : 0, false, auxSample, pixelX / 4, pixelY / 4)) {
            outSample.aux = auxSample.value;
        }
    } else if (stage == BounceableRadiance || stage == RadianceSourceCurrent || stage == RadianceSourceFinal || stage == BounceReinjection) {
        ProbeSample auxSample;
        if (readTextureProbe(m_radianceProvenanceDebug ? m_radianceProvenanceDebug->ID() : 0, false, auxSample, pixelX / 4, pixelY / 4)) {
            outSample.aux = auxSample.value;
        }
    } else if (stage == Denoise1 || stage == Denoise2 || stage == DenoiseWeights) {
        ProbeSample auxSample;
        if (readTextureProbe(m_denoiseWeightDebug ? m_denoiseWeightDebug->ID() : 0, false, auxSample, pixelX / 4, pixelY / 4)) {
            outSample.aux = auxSample.value;
        }
    }

    return true;
}

bool IndirectDiffusePass::exportTexture(GLuint texture, bool fullRes, bool hdrColor, const std::string& filepath) const {
    if (texture == 0) {
        return false;
    }

    const int width = fullRes ? m_w : m_qw;
    const int height = fullRes ? m_h : m_qh;
    if (width <= 0 || height <= 0) {
        return false;
    }

    std::vector<float> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0.0f);
    glGetTextureImage(texture, 0, GL_RGBA, GL_FLOAT, static_cast<GLsizei>(pixels.size() * sizeof(float)), pixels.data());

    std::vector<unsigned char> png(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t src = (static_cast<size_t>(y) * width + x) * 4u;
            const size_t dst = (static_cast<size_t>(height - 1 - y) * width + x) * 4u;
            glm::vec3 rgb(pixels[src + 0], pixels[src + 1], pixels[src + 2]);
            if (hdrColor) {
                rgb = DebugTonemap(rgb, 8.0f);
            } else {
                rgb = glm::clamp(rgb, glm::vec3(0.0f), glm::vec3(1.0f));
            }
            png[dst + 0] = static_cast<unsigned char>(glm::clamp(rgb.r, 0.0f, 1.0f) * 255.0f);
            png[dst + 1] = static_cast<unsigned char>(glm::clamp(rgb.g, 0.0f, 1.0f) * 255.0f);
            png[dst + 2] = static_cast<unsigned char>(glm::clamp(rgb.b, 0.0f, 1.0f) * 255.0f);
            png[dst + 3] = static_cast<unsigned char>(glm::clamp(pixels[src + 3], 0.0f, 1.0f) * 255.0f);
        }
    }

    return stbi_write_png(filepath.c_str(), width, height, 4, png.data(), width * 4) != 0;
}

bool IndirectDiffusePass::ExportValidationStages(const std::string& directory) const {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    const int stages[] = {
        BounceableRadiance,
        RawIndirect,
        HistoryReprojected,
        HistoryConfidence,
        Denoise1,
        Denoise2,
        UpscaledIndirect,
        FinalIndirectOnly,
        RadianceSourceCurrent,
        RadianceSourceReinjection,
        RadianceSourceFinal,
        TemporalHistoryRaw,
        TemporalHistoryClamped,
        TemporalResolved,
        DenoiseWeights,
        ContributionProvenance
    };

    bool allOk = true;
    for (const int stage : stages) {
        bool fullRes = false;
        const GLuint tex = getTextureForDebugStage(stage, fullRes);
        if (tex == 0) {
            allOk = false;
            continue;
        }
        const std::string path = (std::filesystem::path(directory) / (std::string(GetDebugStageLabel(stage)) + ".png")).string();
        allOk &= exportTexture(tex, fullRes, IsHdrColorStage(stage), path);
    }
    return allOk;
}
