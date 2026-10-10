#include "LPVPass.h"
#include "../PresentTarget.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../RenderContext.h"
#include "../LightManager.h"
#include "../BaseLight.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#pragma warning(disable: 4996)  // Suppress deprecated function warnings for SceneGraph legacy API

namespace {
// Immutable storage of a single mip level (glTexStorage*).
rhi::TextureDesc MakeImmutableDesc(rhi::TextureType type, rhi::Format format, int width, int height, int depth) {
    rhi::TextureDesc desc;
    desc.type = type;
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.mipLevels = 1;
    desc.immutable = true;
    return desc;
}
}

LPVPass::LPVPass() {}

LPVPass::~LPVPass() {
    CleanupResources();
}

bool LPVPass::Initialize(RenderContext& context) {
    std::cout << "[LPVPass] Initializing Light Propagation Volume GI system...\n";

    if (!CreateShaders()) {
        std::cerr << "[LPVPass] Failed to create shaders\n";
        return false;
    }

    if (!CreateLPVResources()) {
        std::cerr << "[LPVPass] Failed to create LPV resources\n";
        return false;
    }

    if (!CreateRSMResources()) {
        std::cerr << "[LPVPass] Failed to create RSM resources\n";
        return false;
    }

    // Track allocated resolutions so we can recreate when settings change
    m_allocatedGridResolution = config.gridResolution;
    m_allocatedRSMResolution = config.rsmResolution;

    std::cout << "[LPVPass] Initialized successfully:\n";
    std::cout << "  - Grid Resolution: " << config.gridResolution << "^3\n";
    std::cout << "  - Voxel Size: " << config.voxelSize << "m\n";
    std::cout << "  - RSM Resolution: " << config.rsmResolution << "x" << config.rsmResolution << "\n";
    std::cout << "  - VPL Sample Count: " << config.vplSampleCount << "\n";
    std::cout << "  - Propagation Iterations: " << config.propagationIterations << "\n";

    return true;
}

void LPVPass::Resize(RenderContext& context, int /*newWidth*/, int /*newHeight*/) {
    // LPV is world-space; no screen-size dependent allocations here
}

void LPVPass::DestroyLPVResourcesOnly() {
    rhi::Device& gpu = rhi::Gpu();
    for (int i = 0; i < 3; ++i) {
        if (m_lpvTextures[i]) { gpu.DestroyTexture(m_lpvTextures[i]); }
        if (m_lpvSampleTextures[i]) { gpu.DestroyTexture(m_lpvSampleTextures[i]); }
        if (m_lpvSampleTexturesTemp[i]) { gpu.DestroyTexture(m_lpvSampleTexturesTemp[i]); }
        if (m_lpvAccumTextures[i]) { gpu.DestroyTexture(m_lpvAccumTextures[i]); }
    }
    if (m_geometryVolume) { gpu.DestroyTexture(m_geometryVolume); }
}

void LPVPass::DestroyRSMResourcesOnly() {
    rhi::Device& gpu = rhi::Gpu();
    if (m_rsmPosition) { gpu.DestroyTexture(m_rsmPosition); }
    if (m_rsmNormal)   { gpu.DestroyTexture(m_rsmNormal); }
    if (m_rsmFlux)     { gpu.DestroyTexture(m_rsmFlux); }
    if (m_rsmDepth)    { gpu.DestroyTexture(m_rsmDepth); }
    if (m_resolvedIndirectTexture) { gpu.DestroyTexture(m_resolvedIndirectTexture); }
    m_rsmFBO.reset();
}

void LPVPass::Execute(RenderContext& ctx,
                      const std::shared_ptr<SceneGraph>& sceneGraph,
                      const std::shared_ptr<Camera>& camera,
                      const std::shared_ptr<DirectionalLight>& dirLight,
                      const std::shared_ptr<Skybox>& /*skybox*/) {
    if (!config.enableLPV || !sceneGraph || !camera) return;

    rhi::Device& gpu = rhi::Gpu();

    // Recreate resources if grid/RSM res changed
    if (m_allocatedGridResolution != config.gridResolution) {
        std::cout << "[LPVPass] Recreating LPV resources for grid res change: "
                  << m_allocatedGridResolution << " -> " << config.gridResolution << "\n";
        DestroyLPVResourcesOnly();
        if (!CreateLPVResources()) return;
        m_allocatedGridResolution = config.gridResolution;
        m_geometryDirty = true;
    }
    if (m_allocatedRSMResolution != config.rsmResolution) {
        std::cout << "[LPVPass] Recreating RSM resources: "
                  << m_allocatedRSMResolution << " -> " << config.rsmResolution << "\n";
        DestroyRSMResourcesOnly();
        if (!CreateRSMResources()) return;
        m_allocatedRSMResolution = config.rsmResolution;
    }

    // Follow the camera, shifted forward so most cells cover what is in view (the camera sits
    // 15% of the extent from the back face) and snapped to whole cells so the lighting does not
    // swim as it moves.
    const float voxelSize = std::max(config.voxelSize, 1e-3f);
    const float gridExtent = config.gridResolution * voxelSize;
    glm::vec3 forward = camera->GetCameraFrontVector();
    forward = glm::dot(forward, forward) > 1e-8f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 desiredCenter = camera->GetCameraPosition() + forward * (0.35f * gridExtent);
    config.gridCenter = glm::floor(desiredCenter / voxelSize + 0.5f) * voxelSize;
    config.gridOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    // Share parameters with lighting pass
    ctx.lpvGridCenter = config.gridCenter;
    ctx.lpvGridResolution = config.gridResolution;
    ctx.lpvVoxelSize = config.voxelSize;
    ctx.lpvGridOrientation = config.gridOrientation;

    if (glm::distance(m_lastGridCenter, config.gridCenter) > config.voxelSize * 2.0f) {
        m_geometryDirty = true;
        m_lastGridCenter = config.gridCenter;
    }

    // Voxelize occluders every frame so moving objects block light where they are now
    if (config.enableOcclusion) {
        VoxelizeGeometry(sceneGraph);
        m_geometryDirty = false;
    }

    // Clear the injection accumulators
    for (int i = 0; i < 3; ++i) {
        const int zero = 0;
        gpu.ClearTexture(m_lpvTextures[i], 0, rhi::PixelFormat::RInt, rhi::PixelType::Int, &zero);
    }

    glm::vec3 lightDirection(0.0f, -1.0f, 0.0f);
    glm::vec3 lightIrradiance(0.0f);
    const bool hasLight = FindInjectedLight(ctx, dirLight, lightDirection, lightIrradiance);

    if (hasLight) {
        // Render RSM for current light configuration, then inject its texels as VPLs
        RenderRSM(sceneGraph, lightDirection, lightIrradiance);
        InjectVPLs(lightIrradiance);
    }

    // Make sure injection writes are visible
    gpu.PipelineBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);

    // Convert the fixed-point accumulators into the first float wave
    if (m_convertShader) {
        gpu.UseProgram(m_convertShader);
        gpu.SetUniform(gpu.GetUniformLocation(m_convertShader, "u_gridResolution"), config.gridResolution);
        gpu.SetUniform(gpu.GetUniformLocation(m_convertShader, "u_fixedPointScale"), m_fixedPointScale);
        gpu.BindImage(0, m_lpvTextures[0], 0, true, 0, rhi::ImageAccess::ReadOnly, rhi::Format::R32I);
        gpu.BindImage(1, m_lpvTextures[1], 0, true, 0, rhi::ImageAccess::ReadOnly, rhi::Format::R32I);
        gpu.BindImage(2, m_lpvTextures[2], 0, true, 0, rhi::ImageAccess::ReadOnly, rhi::Format::R32I);
        gpu.BindImage(3, m_lpvSampleTextures[0], 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
        gpu.BindImage(4, m_lpvSampleTextures[1], 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
        gpu.BindImage(5, m_lpvSampleTextures[2], 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
        const int groups = (config.gridResolution + 7) / 8;
        gpu.Dispatch(groups, groups, groups);
        gpu.PipelineBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch | rhi::BarrierTextureUpdate);

        // The injected light itself is the first term of the accumulated volume.
        for (int i = 0; i < 3; ++i) {
            gpu.CopyTexture(m_lpvSampleTextures[i],
                            rhi::TextureRegion{ 0, glm::ivec3(0), glm::ivec3(config.gridResolution) },
                            m_lpvAccumTextures[i], 0, glm::ivec3(0));
        }
    }

    PropagateLPV();

    // Resolve LPV's 3D SH representation into a generic full-screen indirect texture.
    ResolveIndirect(ctx);

    // Ensure the final float volumes and resolved texture are visible to downstream passes
    gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);
}

bool LPVPass::FindInjectedLight(const RenderContext& ctx,
                                const std::shared_ptr<DirectionalLight>& fallback,
                                glm::vec3& direction,
                                glm::vec3& irradiance) const {
    // Inject the same directional light the deferred pass shades with, not the engine's default
    // light object (which only exists as a fallback for scenes without lights).
    std::shared_ptr<BaseLight> light;
    if (ctx.lightManager) {
        for (const auto& candidate : ctx.lightManager->GetEnabledLights()) {
            if (candidate && candidate->GetLightType() == BaseLight::LightType::DIRECTIONAL) {
                light = candidate;
                break;
            }
        }
    }
    if (!light) {
        light = fallback;
    }
    if (!light) {
        return false;
    }

    const glm::vec3 dir = light->GetDirection();
    if (glm::dot(dir, dir) < 1e-8f) {
        return false;
    }
    direction = glm::normalize(dir);
    // Same convention as lighting_common.glsl: a directional light delivers color * intensity to
    // a surface facing it.
    irradiance = glm::max(light->GetColor() * light->GetIntensity(), glm::vec3(0.0f));
    return irradiance.x + irradiance.y + irradiance.z > 0.0f;
}

bool LPVPass::CreateShaders() {
    m_rsmShader = CreateShaderProgram("shaders/lpv_rsm_vert.glsl", "shaders/lpv_rsm_frag.glsl");
    if (!m_rsmShader) return false;

    m_injectionShader = CreateComputeShader("shaders/lpv_injection_comp.glsl");
    if (!m_injectionShader) return false;

    m_propagationShader = CreateComputeShader("shaders/lpv_propagation_comp.glsl");
    if (!m_propagationShader) return false;

    m_convertShader = CreateComputeShader("shaders/lpv_convert_comp.glsl");
    if (!m_convertShader) return false;

    m_resolveShader = CreateComputeShader("shaders/lpv_resolve_indirect_comp.glsl");
    if (!m_resolveShader) return false;

    m_voxelizeShader = CreateShaderProgramWithGeometry(
        "shaders/lpv_voxelize_vert.glsl",
        "shaders/lpv_voxelize_geom.glsl",
        "shaders/lpv_voxelize_frag.glsl");
    if (!m_voxelizeShader) return false;

    m_debugShader = CreateShaderProgram("shaders/lpv_debug_vert.glsl", "shaders/lpv_debug_frag.glsl");
    if (!m_debugShader) return false;

    return true;
}

bool LPVPass::CreateLPVResources() {
    rhi::Device& gpu = rhi::Gpu();
    const int res = config.gridResolution;
    const int extendedResX = res * 4; // 4 SH coefficients along X for the integer accumulators

    auto setVolumeParams = [&gpu](rhi::TextureHandle texture, rhi::Filter filter) {
        gpu.SetTextureFilter(texture, filter, filter);
        gpu.SetTextureWrapAxis(texture, 0, rhi::Wrap::ClampToEdge);
        gpu.SetTextureWrapAxis(texture, 1, rhi::Wrap::ClampToEdge);
        gpu.SetTextureWrapAxis(texture, 2, rhi::Wrap::ClampToEdge);
    };

    for (int i = 0; i < 3; ++i) {
        m_lpvTextures[i] = gpu.CreateTexture(MakeImmutableDesc(rhi::TextureType::Tex3D, rhi::Format::R32I, extendedResX, res, res));
        setVolumeParams(m_lpvTextures[i], rhi::Filter::Nearest);

        rhi::TextureHandle* floatVolumes[3] = { &m_lpvSampleTextures[i], &m_lpvSampleTexturesTemp[i], &m_lpvAccumTextures[i] };
        for (rhi::TextureHandle* volume : floatVolumes) {
            *volume = gpu.CreateTexture(MakeImmutableDesc(rhi::TextureType::Tex3D, rhi::Format::RGBA16F, res, res, res));
            setVolumeParams(*volume, rhi::Filter::Linear);
            const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            gpu.ClearTexture(*volume, 0, rhi::PixelFormat::RGBA, rhi::PixelType::Float, zero);
        }
    }

    // Directional geometry volume for occlusion (6 bits per cell, see lpv_voxelize_frag.glsl)
    m_geometryVolume = gpu.CreateTexture(MakeImmutableDesc(rhi::TextureType::Tex3D, rhi::Format::R32UI, res, res, res));
    setVolumeParams(m_geometryVolume, rhi::Filter::Nearest);
    const uint32_t noGeometry = 0u;
    gpu.ClearTexture(m_geometryVolume, 0, rhi::PixelFormat::RInt, rhi::PixelType::UInt, &noGeometry);

    return gpu.PollError() == nullptr;
}

bool LPVPass::CreateRSMResources() {
    rhi::Device& gpu = rhi::Gpu();
    int res = config.rsmResolution;

    auto setRSMParams = [&gpu](rhi::TextureHandle texture) {
        gpu.SetTextureFilter(texture, rhi::Filter::Nearest, rhi::Filter::Nearest);
        gpu.SetTextureWrapAxis(texture, 0, rhi::Wrap::ClampToEdge);
        gpu.SetTextureWrapAxis(texture, 1, rhi::Wrap::ClampToEdge);
    };

    m_rsmPosition = gpu.CreateTexture(MakeImmutableDesc(rhi::TextureType::Tex2D, rhi::Format::RGB16F, res, res, 1));
    setRSMParams(m_rsmPosition);

    m_rsmNormal = gpu.CreateTexture(MakeImmutableDesc(rhi::TextureType::Tex2D, rhi::Format::RGB16F, res, res, 1));
    setRSMParams(m_rsmNormal);

    m_rsmFlux = gpu.CreateTexture(MakeImmutableDesc(rhi::TextureType::Tex2D, rhi::Format::RGB16F, res, res, 1));
    setRSMParams(m_rsmFlux);

    m_rsmDepth = gpu.CreateTexture(MakeImmutableDesc(rhi::TextureType::Tex2D, rhi::Format::D24, res, res, 1));
    setRSMParams(m_rsmDepth);

    // Use existing FrameBuffer abstraction; we attach our textures after creation
    m_rsmFBO = std::make_unique<FrameBuffer>(res, res, std::vector<rhi::Format>{rhi::Format::RGB16F, rhi::Format::RGB16F, rhi::Format::RGB16F}, true);
    if (!m_rsmFBO->IsComplete()) return false;

    rhi::FramebufferHandle fbo = m_rsmFBO->GetFBO();
    gpu.BindFramebuffer(fbo);
    gpu.SetAttachment(fbo, rhi::AttachmentPoint::Color0, rhi::Attachment{ m_rsmPosition });
    gpu.SetAttachment(fbo, rhi::AttachmentPoint::Color1, rhi::Attachment{ m_rsmNormal });
    gpu.SetAttachment(fbo, rhi::AttachmentPoint::Color2, rhi::Attachment{ m_rsmFlux });
    gpu.SetAttachment(fbo, rhi::AttachmentPoint::Depth, rhi::Attachment{ m_rsmDepth });
    const int bufs[3] = { 0, 1, 2 };
    gpu.SetDrawBuffers(fbo, bufs, 3);
    bool complete = gpu.IsFramebufferComplete(fbo);
    gpu.BindFramebuffer(PresentTarget::Get());
    return complete;
}

bool LPVPass::EnsureResolvedIndirectTexture(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (m_resolvedIndirectTexture &&
        m_resolvedIndirectWidth == width &&
        m_resolvedIndirectHeight == height) {
        return true;
    }

    rhi::Device& gpu = rhi::Gpu();
    if (m_resolvedIndirectTexture) {
        gpu.DestroyTexture(m_resolvedIndirectTexture);
    }

    m_resolvedIndirectTexture = gpu.CreateTexture(MakeImmutableDesc(rhi::TextureType::Tex2D, rhi::Format::RGBA16F, width, height, 1));
    gpu.SetTextureFilter(m_resolvedIndirectTexture, rhi::Filter::Linear, rhi::Filter::Linear);
    gpu.SetTextureWrapAxis(m_resolvedIndirectTexture, 0, rhi::Wrap::ClampToEdge);
    gpu.SetTextureWrapAxis(m_resolvedIndirectTexture, 1, rhi::Wrap::ClampToEdge);

    m_resolvedIndirectWidth = width;
    m_resolvedIndirectHeight = height;
    return gpu.PollError() == nullptr;
}

void LPVPass::RenderRSM(const std::shared_ptr<SceneGraph>& sceneGraph,
                        const glm::vec3& lightDirection,
                        const glm::vec3& lightIrradiance) {
    if (!m_rsmFBO) return;

    rhi::Device& gpu = rhi::Gpu();

    m_rsmFBO->Bind();
    gpu.SetViewport(rhi::Rect{ 0, 0, config.rsmResolution, config.rsmResolution });
    // Empty texels must carry zero flux.
    const glm::vec4 previousClearColor = gpu.CaptureState().clearColor;
    gpu.SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
    gpu.Clear(rhi::ClearColor | rhi::ClearDepth);
    gpu.SetClearColor(previousClearColor);

    // Orthographic light view covering the whole grid.
    const float halfExtent = config.gridResolution * config.voxelSize * 0.5f;
    const float depthRange = halfExtent * 4.0f;
    glm::vec3 lightPos = config.gridCenter - lightDirection * (depthRange * 0.5f);
    glm::vec3 up(0, 1, 0);
    if (std::abs(glm::dot(lightDirection, up)) > 0.99f) up = glm::vec3(1, 0, 0);
    glm::mat4 lightView = glm::lookAt(lightPos, config.gridCenter, up);
    glm::mat4 lightProj = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.0f, depthRange);
    glm::mat4 lightViewProj = lightProj * lightView;

    gpu.SetDepthTest(true);
    gpu.SetDepthFunc(rhi::CompareOp::Less);
    gpu.SetBlendEnabled(false);
    gpu.UseProgram(m_rsmShader);
    gpu.SetUniform(gpu.GetUniformLocation(m_rsmShader, "u_lightDirection"), lightDirection);
    gpu.SetUniform(gpu.GetUniformLocation(m_rsmShader, "u_lightIrradiance"), lightIrradiance);

    // Cull against the light volume, not the camera: off-screen surfaces bounce light too.
    sceneGraph->RenderShadowCascade(lightViewProj, m_rsmShader);

    FrameBuffer::Unbind();
}

void LPVPass::InjectVPLs(const glm::vec3& lightIrradiance) {
    rhi::Device& gpu = rhi::Gpu();
    gpu.UseProgram(m_injectionShader);

    // RSM inputs
    gpu.BindTexture(0, m_rsmPosition);
    gpu.BindTexture(1, m_rsmNormal);
    gpu.BindTexture(2, m_rsmFlux);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_rsmPosition"), 0);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_rsmNormal"), 1);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_rsmFlux"), 2);

    // Integer LPV outputs (atomic add)
    gpu.BindImage(0, m_lpvTextures[0], 0, true, 0, rhi::ImageAccess::ReadWrite, rhi::Format::R32I);
    gpu.BindImage(1, m_lpvTextures[1], 0, true, 0, rhi::ImageAccess::ReadWrite, rhi::Format::R32I);
    gpu.BindImage(2, m_lpvTextures[2], 0, true, 0, rhi::ImageAccess::ReadWrite, rhi::Format::R32I);

    // VPLs sample the RSM on a regular grid; each stands for the light-perpendicular area around it.
    const int vplGridSize = std::clamp(static_cast<int>(std::ceil(std::sqrt(static_cast<float>(std::max(config.vplSampleCount, 1))))),
                                       1, config.rsmResolution);
    const float rsmExtent = config.gridResolution * config.voxelSize;
    const float vplSpacing = rsmExtent / static_cast<float>(vplGridSize);
    const float vplArea = vplSpacing * vplSpacing;

    // Fixed-point scale from an upper bound of a cell's SH coefficients: the light can deliver at
    // most E * (projected area of a cell <= sqrt(3) s^2) to a cell, whose intensity lobe then has
    // L0 = flux / pi * 0.886. Keep 16x headroom below the int range for overlapping VPLs.
    const float maxIrradiance = std::max(lightIrradiance.x, std::max(lightIrradiance.y, lightIrradiance.z));
    const float maxCellCoefficient = std::max(maxIrradiance * 1.7320508f * config.voxelSize * config.voxelSize * 0.2820948f, 1e-12f);
    m_fixedPointScale = 1.34217728e8f / maxCellCoefficient; // 2^27 / max

    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_gridCenter"), config.gridCenter);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_voxelSize"), config.voxelSize);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_gridResolution"), config.gridResolution);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_vplGridSize"), vplGridSize);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_vplArea"), vplArea);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_fixedPointScale"), m_fixedPointScale);
    glm::vec4 q(config.gridOrientation.x, config.gridOrientation.y, config.gridOrientation.z, config.gridOrientation.w);
    gpu.SetUniform(gpu.GetUniformLocation(m_injectionShader, "u_gridOrientation"), q);

    const int groups = (vplGridSize + 7) / 8;
    gpu.Dispatch(groups, groups, 1);
    gpu.PipelineBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
}

void LPVPass::PropagateLPV() {
    rhi::Device& gpu = rhi::Gpu();
    gpu.UseProgram(m_propagationShader);

    gpu.SetUniform(gpu.GetUniformLocation(m_propagationShader, "u_gridResolution"), config.gridResolution);
    gpu.SetUniform(gpu.GetUniformLocation(m_propagationShader, "u_enableOcclusion"), config.enableOcclusion ? 1 : 0);
    gpu.SetUniform(gpu.GetUniformLocation(m_propagationShader, "u_waveR"), 0);
    gpu.SetUniform(gpu.GetUniformLocation(m_propagationShader, "u_waveG"), 1);
    gpu.SetUniform(gpu.GetUniformLocation(m_propagationShader, "u_waveB"), 2);
    gpu.SetUniform(gpu.GetUniformLocation(m_propagationShader, "u_geometryVolume"), 3);
    gpu.BindTexture(3, m_geometryVolume);

    const int groups = (config.gridResolution + 3) / 4;
    const rhi::UniformLocation iterationLoc = gpu.GetUniformLocation(m_propagationShader, "u_iteration");

    for (int iter = 0; iter < config.propagationIterations; ++iter) {
        // Ping-pong the waves; every new wave is added to the accumulated volume.
        rhi::TextureHandle* inVol = (iter % 2 == 0) ? m_lpvSampleTextures : m_lpvSampleTexturesTemp;
        rhi::TextureHandle* outVol = (iter % 2 == 0) ? m_lpvSampleTexturesTemp : m_lpvSampleTextures;

        gpu.BindTexture(0, inVol[0]);
        gpu.BindTexture(1, inVol[1]);
        gpu.BindTexture(2, inVol[2]);
        gpu.BindImage(0, outVol[0], 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
        gpu.BindImage(1, outVol[1], 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
        gpu.BindImage(2, outVol[2], 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
        gpu.BindImage(3, m_lpvAccumTextures[0], 0, true, 0, rhi::ImageAccess::ReadWrite, rhi::Format::RGBA16F);
        gpu.BindImage(4, m_lpvAccumTextures[1], 0, true, 0, rhi::ImageAccess::ReadWrite, rhi::Format::RGBA16F);
        gpu.BindImage(5, m_lpvAccumTextures[2], 0, true, 0, rhi::ImageAccess::ReadWrite, rhi::Format::RGBA16F);
        gpu.SetUniform(iterationLoc, iter);

        gpu.Dispatch(groups, groups, groups);
        gpu.PipelineBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
    }
}

void LPVPass::ResolveIndirect(RenderContext& ctx) {
    if (!m_resolveShader || !ctx.gbufferFBO || !EnsureResolvedIndirectTexture(ctx.width, ctx.height)) {
        return;
    }

    rhi::Device& gpu = rhi::Gpu();
    rhi::TextureHandle* finalVolumes = m_lpvAccumTextures;

    gpu.UseProgram(m_resolveShader);

    gpu.BindTexture(0, finalVolumes[0]);
    gpu.BindTexture(1, finalVolumes[1]);
    gpu.BindTexture(2, finalVolumes[2]);
    gpu.BindTexture(3, ctx.gbufferFBO->GetColorAttachment(0));
    gpu.BindTexture(4, ctx.gbufferFBO->GetDepthTexture());

    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_lpvTextureR"), 0);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_lpvTextureG"), 1);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_lpvTextureB"), 2);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_packedNormalRM"), 3);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_depth"), 4);

    const glm::mat4 invProjection = glm::inverse(ctx.proj);
    const glm::mat4 invView = glm::inverse(ctx.view);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_invProjection"), invProjection);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_invView"), invView);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_gridCenter"), config.gridCenter);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_voxelSize"), config.voxelSize);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_gridResolution"), config.gridResolution);

    glm::vec4 orientation(config.gridOrientation.x, config.gridOrientation.y, config.gridOrientation.z, config.gridOrientation.w);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_gridOrientation"), orientation);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_giStrength"), config.giStrength);
    gpu.SetUniform(gpu.GetUniformLocation(m_resolveShader, "u_normalsInWorldSpace"), 1);

    gpu.BindImage(0, m_resolvedIndirectTexture, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
    const int groupsX = (ctx.width + 7) / 8;
    const int groupsY = (ctx.height + 7) / 8;
    gpu.Dispatch(groupsX, groupsY, 1);
    gpu.PipelineBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
}

void LPVPass::VoxelizeGeometry(const std::shared_ptr<SceneGraph>& sceneGraph) {
    rhi::Device& gpu = rhi::Gpu();

    // Clear geometry volume
    const uint32_t noGeometry = 0u;
    gpu.ClearTexture(m_geometryVolume, 0, rhi::PixelFormat::RInt, rhi::PixelType::UInt, &noGeometry);

    const rhi::Rect previousViewport = gpu.GetViewport();
    // One fragment per cell along the dominant axis (the geometry shader projects onto it).
    gpu.SetViewport(rhi::Rect{ 0, 0, config.gridResolution, config.gridResolution });
    gpu.SetDepthTest(false);
    gpu.SetCullEnabled(false);
    gpu.SetColorMask(false, false, false, false);

    gpu.UseProgram(m_voxelizeShader);
    gpu.BindImage(0, m_geometryVolume, 0, true, 0, rhi::ImageAccess::ReadWrite, rhi::Format::R32UI);
    gpu.SetUniform(gpu.GetUniformLocation(m_voxelizeShader, "u_gridCenter"), config.gridCenter);
    gpu.SetUniform(gpu.GetUniformLocation(m_voxelizeShader, "u_voxelSize"), config.voxelSize);
    gpu.SetUniform(gpu.GetUniformLocation(m_voxelizeShader, "u_gridResolution"), config.gridResolution);

    // Everything inside the grid, independent of the camera frustum.
    const float halfExtent = config.gridResolution * config.voxelSize * 0.5f;
    const glm::mat4 gridVolume =
        glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.0f, 2.0f * halfExtent) *
        glm::lookAt(config.gridCenter + glm::vec3(0.0f, 0.0f, halfExtent), config.gridCenter, glm::vec3(0.0f, 1.0f, 0.0f));
    sceneGraph->RenderShadowCascade(gridVolume, m_voxelizeShader);

    gpu.SetColorMask(true, true, true, true);
    gpu.SetDepthTest(true);
    gpu.SetViewport(previousViewport);
    gpu.PipelineBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
}

void LPVPass::RenderDebugVisualization(const glm::mat4& view, const glm::mat4& projection) {
    rhi::Device& gpu = rhi::Gpu();

    gpu.UseProgram(m_debugShader);
    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_view"), view);
    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_projection"), projection);
    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_gridCenter"), config.gridCenter);
    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_voxelSize"), config.voxelSize);
    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_gridResolution"), config.gridResolution);

    gpu.BindTexture(0, m_lpvAccumTextures[0]);
    gpu.BindTexture(1, m_lpvAccumTextures[1]);
    gpu.BindTexture(2, m_lpvAccumTextures[2]);
    gpu.BindTexture(3, m_geometryVolume);

    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_lpvTextureR"), 0);
    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_lpvTextureG"), 1);
    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_lpvTextureB"), 2);
    gpu.SetUniform(gpu.GetUniformLocation(m_debugShader, "u_geometryVolume"), 3);

    gpu.SetBlendEnabled(true);
    gpu.SetBlendFunc(rhi::BlendFactor::SrcAlpha, rhi::BlendFactor::OneMinusSrcAlpha);
    gpu.SetDepthTest(false);

    int sampleStep = 4;
    int count = (config.gridResolution / sampleStep);
    // The vertex shader sizes each point by its LPV energy.
    gpu.SetFeature(rhi::Feature::ProgramPointSize, true);
    gpu.BindVertexInput({});
    gpu.Draw(rhi::Topology::Points, 0, count * count * count);
    gpu.SetFeature(rhi::Feature::ProgramPointSize, false);

    gpu.SetBlendEnabled(false);
    gpu.SetDepthTest(true);
}

void LPVPass::CleanupResources() {
    rhi::Device& gpu = rhi::Gpu();
    for (int i = 0; i < 3; ++i) {
        if (m_lpvTextures[i]) gpu.DestroyTexture(m_lpvTextures[i]);
        if (m_lpvSampleTextures[i]) gpu.DestroyTexture(m_lpvSampleTextures[i]);
        if (m_lpvSampleTexturesTemp[i]) gpu.DestroyTexture(m_lpvSampleTexturesTemp[i]);
        if (m_lpvAccumTextures[i]) gpu.DestroyTexture(m_lpvAccumTextures[i]);
    }
    if (m_geometryVolume) gpu.DestroyTexture(m_geometryVolume);
    if (m_rsmPosition) gpu.DestroyTexture(m_rsmPosition);
    if (m_rsmNormal) gpu.DestroyTexture(m_rsmNormal);
    if (m_rsmFlux) gpu.DestroyTexture(m_rsmFlux);
    if (m_rsmDepth) gpu.DestroyTexture(m_rsmDepth);
    if (m_resolvedIndirectTexture) gpu.DestroyTexture(m_resolvedIndirectTexture);
    if (m_rsmShader) gpu.DestroyProgram(m_rsmShader);
    if (m_injectionShader) gpu.DestroyProgram(m_injectionShader);
    if (m_propagationShader) gpu.DestroyProgram(m_propagationShader);
    if (m_convertShader) gpu.DestroyProgram(m_convertShader);
    if (m_resolveShader) gpu.DestroyProgram(m_resolveShader);
    if (m_voxelizeShader) gpu.DestroyProgram(m_voxelizeShader);
    if (m_debugShader) gpu.DestroyProgram(m_debugShader);
    m_rsmFBO.reset();
}

glm::vec3 LPVPass::WorldToVoxel(const glm::vec3& worldPos) const {
    glm::vec3 local = (worldPos - config.gridCenter) / config.voxelSize;
    return local + glm::vec3(config.gridResolution * 0.5f);
}

glm::vec3 LPVPass::VoxelToWorld(const glm::ivec3& voxelPos) const {
    glm::vec3 local = (glm::vec3(voxelPos) - glm::vec3(config.gridResolution * 0.5f)) * config.voxelSize;
    return config.gridCenter + local;
}
