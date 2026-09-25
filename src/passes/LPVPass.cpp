#include "LPVPass.h"
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
    for (int i = 0; i < 3; ++i) {
        if (m_lpvTextures[i]) { glDeleteTextures(1, &m_lpvTextures[i]); m_lpvTextures[i] = 0; }
        if (m_lpvSampleTextures[i]) { glDeleteTextures(1, &m_lpvSampleTextures[i]); m_lpvSampleTextures[i] = 0; }
        if (m_lpvSampleTexturesTemp[i]) { glDeleteTextures(1, &m_lpvSampleTexturesTemp[i]); m_lpvSampleTexturesTemp[i] = 0; }
        if (m_lpvAccumTextures[i]) { glDeleteTextures(1, &m_lpvAccumTextures[i]); m_lpvAccumTextures[i] = 0; }
    }
    if (m_geometryVolume) { glDeleteTextures(1, &m_geometryVolume); m_geometryVolume = 0; }
}

void LPVPass::DestroyRSMResourcesOnly() {
    if (m_rsmPosition) { glDeleteTextures(1, &m_rsmPosition); m_rsmPosition = 0; }
    if (m_rsmNormal)   { glDeleteTextures(1, &m_rsmNormal);   m_rsmNormal = 0; }
    if (m_rsmFlux)     { glDeleteTextures(1, &m_rsmFlux);     m_rsmFlux = 0; }
    if (m_rsmDepth)    { glDeleteTextures(1, &m_rsmDepth);    m_rsmDepth = 0; }
    if (m_resolvedIndirectTexture) { glDeleteTextures(1, &m_resolvedIndirectTexture); m_resolvedIndirectTexture = 0; }
    m_rsmFBO.reset();
}

void LPVPass::Execute(RenderContext& ctx,
                      const std::shared_ptr<SceneGraph>& sceneGraph,
                      const std::shared_ptr<Camera>& camera,
                      const std::shared_ptr<DirectionalLight>& dirLight,
                      const std::shared_ptr<Skybox>& /*skybox*/) {
    if (!config.enableLPV || !sceneGraph || !camera) return;

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
        const GLint zero = 0;
        glClearTexImage(m_lpvTextures[i], 0, GL_RED_INTEGER, GL_INT, &zero);
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
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // Convert the fixed-point accumulators into the first float wave
    if (m_convertShader != 0) {
        glUseProgram(m_convertShader);
        glUniform1i(glGetUniformLocation(m_convertShader, "u_gridResolution"), config.gridResolution);
        glUniform1f(glGetUniformLocation(m_convertShader, "u_fixedPointScale"), m_fixedPointScale);
        glBindImageTexture(0, m_lpvTextures[0], 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32I);
        glBindImageTexture(1, m_lpvTextures[1], 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32I);
        glBindImageTexture(2, m_lpvTextures[2], 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32I);
        glBindImageTexture(3, m_lpvSampleTextures[0], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(4, m_lpvSampleTextures[1], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(5, m_lpvSampleTextures[2], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        const int groups = (config.gridResolution + 7) / 8;
        glDispatchCompute(groups, groups, groups);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);

        // The injected light itself is the first term of the accumulated volume.
        for (int i = 0; i < 3; ++i) {
            glCopyImageSubData(m_lpvSampleTextures[i], GL_TEXTURE_3D, 0, 0, 0, 0,
                               m_lpvAccumTextures[i], GL_TEXTURE_3D, 0, 0, 0, 0,
                               config.gridResolution, config.gridResolution, config.gridResolution);
        }
    }

    PropagateLPV();

    // Resolve LPV's 3D SH representation into a generic full-screen indirect texture.
    ResolveIndirect(ctx);

    // Ensure the final float volumes and resolved texture are visible to downstream passes
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
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
    const int res = config.gridResolution;
    const int extendedResX = res * 4; // 4 SH coefficients along X for the integer accumulators

    auto setVolumeParams = [](GLenum filter) {
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    };

    for (int i = 0; i < 3; ++i) {
        glGenTextures(1, &m_lpvTextures[i]);
        glBindTexture(GL_TEXTURE_3D, m_lpvTextures[i]);
        glTexStorage3D(GL_TEXTURE_3D, 1, GL_R32I, extendedResX, res, res);
        setVolumeParams(GL_NEAREST);

        GLuint* floatVolumes[3] = { &m_lpvSampleTextures[i], &m_lpvSampleTexturesTemp[i], &m_lpvAccumTextures[i] };
        for (GLuint* volume : floatVolumes) {
            glGenTextures(1, volume);
            glBindTexture(GL_TEXTURE_3D, *volume);
            glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, res, res, res);
            setVolumeParams(GL_LINEAR);
            const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            glClearTexImage(*volume, 0, GL_RGBA, GL_FLOAT, zero);
        }
    }

    // Directional geometry volume for occlusion (6 bits per cell, see lpv_voxelize_frag.glsl)
    glGenTextures(1, &m_geometryVolume);
    glBindTexture(GL_TEXTURE_3D, m_geometryVolume);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_R32UI, res, res, res);
    setVolumeParams(GL_NEAREST);
    const GLuint noGeometry = 0u;
    glClearTexImage(m_geometryVolume, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &noGeometry);

    glBindTexture(GL_TEXTURE_3D, 0);

    return glGetError() == GL_NO_ERROR;
}

bool LPVPass::CreateRSMResources() {
    int res = config.rsmResolution;

    glGenTextures(1, &m_rsmPosition);
    glBindTexture(GL_TEXTURE_2D, m_rsmPosition);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB16F, res, res);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_rsmNormal);
    glBindTexture(GL_TEXTURE_2D, m_rsmNormal);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB16F, res, res);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_rsmFlux);
    glBindTexture(GL_TEXTURE_2D, m_rsmFlux);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB16F, res, res);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_rsmDepth);
    glBindTexture(GL_TEXTURE_2D, m_rsmDepth);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, res, res);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Use existing FrameBuffer abstraction; we attach our textures after creation
    m_rsmFBO = std::make_unique<FrameBuffer>(res, res, std::vector<GLenum>{GL_RGB16F, GL_RGB16F, GL_RGB16F}, true);
    if (!m_rsmFBO->IsComplete()) return false;

    GLuint fbo = m_rsmFBO->GetFBO();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_rsmPosition, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_rsmNormal, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_rsmFlux, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_rsmDepth, 0);
    GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, bufs);
    bool complete = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return complete;
}

bool LPVPass::EnsureResolvedIndirectTexture(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    if (m_resolvedIndirectTexture != 0 &&
        m_resolvedIndirectWidth == width &&
        m_resolvedIndirectHeight == height) {
        return true;
    }

    if (m_resolvedIndirectTexture != 0) {
        glDeleteTextures(1, &m_resolvedIndirectTexture);
        m_resolvedIndirectTexture = 0;
    }

    glGenTextures(1, &m_resolvedIndirectTexture);
    glBindTexture(GL_TEXTURE_2D, m_resolvedIndirectTexture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    m_resolvedIndirectWidth = width;
    m_resolvedIndirectHeight = height;
    return glGetError() == GL_NO_ERROR;
}

void LPVPass::RenderRSM(const std::shared_ptr<SceneGraph>& sceneGraph,
                        const glm::vec3& lightDirection,
                        const glm::vec3& lightIrradiance) {
    if (!m_rsmFBO) return;

    m_rsmFBO->Bind();
    glViewport(0, 0, config.rsmResolution, config.rsmResolution);
    // Empty texels must carry zero flux.
    GLfloat previousClearColor[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);

    // Orthographic light view covering the whole grid.
    const float halfExtent = config.gridResolution * config.voxelSize * 0.5f;
    const float depthRange = halfExtent * 4.0f;
    glm::vec3 lightPos = config.gridCenter - lightDirection * (depthRange * 0.5f);
    glm::vec3 up(0, 1, 0);
    if (std::abs(glm::dot(lightDirection, up)) > 0.99f) up = glm::vec3(1, 0, 0);
    glm::mat4 lightView = glm::lookAt(lightPos, config.gridCenter, up);
    glm::mat4 lightProj = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.0f, depthRange);
    glm::mat4 lightViewProj = lightProj * lightView;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glUseProgram(m_rsmShader);
    glUniform3fv(glGetUniformLocation(m_rsmShader, "u_lightDirection"), 1, glm::value_ptr(lightDirection));
    glUniform3fv(glGetUniformLocation(m_rsmShader, "u_lightIrradiance"), 1, glm::value_ptr(lightIrradiance));

    // Cull against the light volume, not the camera: off-screen surfaces bounce light too.
    sceneGraph->RenderShadowCascade(lightViewProj, m_rsmShader);

    FrameBuffer::Unbind();
}

void LPVPass::InjectVPLs(const glm::vec3& lightIrradiance) {
    glUseProgram(m_injectionShader);

    // RSM inputs
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_rsmPosition);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_rsmNormal);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_rsmFlux);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_rsmPosition"), 0);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_rsmNormal"), 1);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_rsmFlux"), 2);

    // Integer LPV outputs (atomic add)
    glBindImageTexture(0, m_lpvTextures[0], 0, GL_TRUE, 0, GL_READ_WRITE, GL_R32I);
    glBindImageTexture(1, m_lpvTextures[1], 0, GL_TRUE, 0, GL_READ_WRITE, GL_R32I);
    glBindImageTexture(2, m_lpvTextures[2], 0, GL_TRUE, 0, GL_READ_WRITE, GL_R32I);

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

    glUniform3fv(glGetUniformLocation(m_injectionShader, "u_gridCenter"), 1, &config.gridCenter[0]);
    glUniform1f(glGetUniformLocation(m_injectionShader, "u_voxelSize"), config.voxelSize);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_gridResolution"), config.gridResolution);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_vplGridSize"), vplGridSize);
    glUniform1f(glGetUniformLocation(m_injectionShader, "u_vplArea"), vplArea);
    glUniform1f(glGetUniformLocation(m_injectionShader, "u_fixedPointScale"), m_fixedPointScale);
    glm::vec4 q(config.gridOrientation.x, config.gridOrientation.y, config.gridOrientation.z, config.gridOrientation.w);
    glUniform4fv(glGetUniformLocation(m_injectionShader, "u_gridOrientation"), 1, &q[0]);

    const int groups = (vplGridSize + 7) / 8;
    glDispatchCompute(groups, groups, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void LPVPass::PropagateLPV() {
    glUseProgram(m_propagationShader);

    glUniform1i(glGetUniformLocation(m_propagationShader, "u_gridResolution"), config.gridResolution);
    glUniform1i(glGetUniformLocation(m_propagationShader, "u_enableOcclusion"), config.enableOcclusion ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_propagationShader, "u_waveR"), 0);
    glUniform1i(glGetUniformLocation(m_propagationShader, "u_waveG"), 1);
    glUniform1i(glGetUniformLocation(m_propagationShader, "u_waveB"), 2);
    glUniform1i(glGetUniformLocation(m_propagationShader, "u_geometryVolume"), 3);
    glBindTextureUnit(3, m_geometryVolume);

    const int groups = (config.gridResolution + 3) / 4;
    const GLint iterationLoc = glGetUniformLocation(m_propagationShader, "u_iteration");

    for (int iter = 0; iter < config.propagationIterations; ++iter) {
        // Ping-pong the waves; every new wave is added to the accumulated volume.
        GLuint* inVol = (iter % 2 == 0) ? m_lpvSampleTextures : m_lpvSampleTexturesTemp;
        GLuint* outVol = (iter % 2 == 0) ? m_lpvSampleTexturesTemp : m_lpvSampleTextures;

        glBindTextureUnit(0, inVol[0]);
        glBindTextureUnit(1, inVol[1]);
        glBindTextureUnit(2, inVol[2]);
        glBindImageTexture(0, outVol[0], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(1, outVol[1], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(2, outVol[2], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(3, m_lpvAccumTextures[0], 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
        glBindImageTexture(4, m_lpvAccumTextures[1], 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
        glBindImageTexture(5, m_lpvAccumTextures[2], 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA16F);
        glUniform1i(iterationLoc, iter);

        glDispatchCompute(groups, groups, groups);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
}

void LPVPass::ResolveIndirect(RenderContext& ctx) {
    if (m_resolveShader == 0 || !ctx.gbufferFBO || !EnsureResolvedIndirectTexture(ctx.width, ctx.height)) {
        return;
    }

    GLuint* finalVolumes = m_lpvAccumTextures;

    glUseProgram(m_resolveShader);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, finalVolumes[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, finalVolumes[1]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_3D, finalVolumes[2]);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());

    glUniform1i(glGetUniformLocation(m_resolveShader, "u_lpvTextureR"), 0);
    glUniform1i(glGetUniformLocation(m_resolveShader, "u_lpvTextureG"), 1);
    glUniform1i(glGetUniformLocation(m_resolveShader, "u_lpvTextureB"), 2);
    glUniform1i(glGetUniformLocation(m_resolveShader, "u_packedNormalRM"), 3);
    glUniform1i(glGetUniformLocation(m_resolveShader, "u_depth"), 4);

    const glm::mat4 invProjection = glm::inverse(ctx.proj);
    const glm::mat4 invView = glm::inverse(ctx.view);
    glUniformMatrix4fv(glGetUniformLocation(m_resolveShader, "u_invProjection"), 1, GL_FALSE, glm::value_ptr(invProjection));
    glUniformMatrix4fv(glGetUniformLocation(m_resolveShader, "u_invView"), 1, GL_FALSE, glm::value_ptr(invView));
    glUniform3fv(glGetUniformLocation(m_resolveShader, "u_gridCenter"), 1, glm::value_ptr(config.gridCenter));
    glUniform1f(glGetUniformLocation(m_resolveShader, "u_voxelSize"), config.voxelSize);
    glUniform1i(glGetUniformLocation(m_resolveShader, "u_gridResolution"), config.gridResolution);

    glm::vec4 orientation(config.gridOrientation.x, config.gridOrientation.y, config.gridOrientation.z, config.gridOrientation.w);
    glUniform4fv(glGetUniformLocation(m_resolveShader, "u_gridOrientation"), 1, glm::value_ptr(orientation));
    glUniform1f(glGetUniformLocation(m_resolveShader, "u_giStrength"), config.giStrength);
    glUniform1i(glGetUniformLocation(m_resolveShader, "u_normalsInWorldSpace"), 1);

    glBindImageTexture(0, m_resolvedIndirectTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    const int groupsX = (ctx.width + 7) / 8;
    const int groupsY = (ctx.height + 7) / 8;
    glDispatchCompute(groupsX, groupsY, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void LPVPass::VoxelizeGeometry(const std::shared_ptr<SceneGraph>& sceneGraph) {
    // Clear geometry volume
    const GLuint noGeometry = 0u;
    glClearTexImage(m_geometryVolume, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &noGeometry);

    GLint previousViewport[4];
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    // One fragment per cell along the dominant axis (the geometry shader projects onto it).
    glViewport(0, 0, config.gridResolution, config.gridResolution);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glUseProgram(m_voxelizeShader);
    glBindImageTexture(0, m_geometryVolume, 0, GL_TRUE, 0, GL_READ_WRITE, GL_R32UI);
    glUniform3fv(glGetUniformLocation(m_voxelizeShader, "u_gridCenter"), 1, &config.gridCenter[0]);
    glUniform1f(glGetUniformLocation(m_voxelizeShader, "u_voxelSize"), config.voxelSize);
    glUniform1i(glGetUniformLocation(m_voxelizeShader, "u_gridResolution"), config.gridResolution);

    // Everything inside the grid, independent of the camera frustum.
    const float halfExtent = config.gridResolution * config.voxelSize * 0.5f;
    const glm::mat4 gridVolume =
        glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.0f, 2.0f * halfExtent) *
        glm::lookAt(config.gridCenter + glm::vec3(0.0f, 0.0f, halfExtent), config.gridCenter, glm::vec3(0.0f, 1.0f, 0.0f));
    sceneGraph->RenderShadowCascade(gridVolume, m_voxelizeShader);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void LPVPass::RenderDebugVisualization(const glm::mat4& view, const glm::mat4& projection) {
    glUseProgram(m_debugShader);
    glUniformMatrix4fv(glGetUniformLocation(m_debugShader, "u_view"), 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(m_debugShader, "u_projection"), 1, GL_FALSE, &projection[0][0]);
    glUniform3fv(glGetUniformLocation(m_debugShader, "u_gridCenter"), 1, &config.gridCenter[0]);
    glUniform1f(glGetUniformLocation(m_debugShader, "u_voxelSize"), config.voxelSize);
    glUniform1i(glGetUniformLocation(m_debugShader, "u_gridResolution"), config.gridResolution);

    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_3D, m_lpvAccumTextures[0]);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_3D, m_lpvAccumTextures[1]);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_3D, m_lpvAccumTextures[2]);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_3D, m_geometryVolume);

    glUniform1i(glGetUniformLocation(m_debugShader, "u_lpvTextureR"), 0);
    glUniform1i(glGetUniformLocation(m_debugShader, "u_lpvTextureG"), 1);
    glUniform1i(glGetUniformLocation(m_debugShader, "u_lpvTextureB"), 2);
    glUniform1i(glGetUniformLocation(m_debugShader, "u_geometryVolume"), 3);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    int sampleStep = 4;
    int count = (config.gridResolution / sampleStep);
    // The vertex shader sizes each point by its LPV energy.
    glEnable(GL_PROGRAM_POINT_SIZE);
    glDrawArrays(GL_POINTS, 0, count * count * count);
    glDisable(GL_PROGRAM_POINT_SIZE);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void LPVPass::CleanupResources() {
    for (int i = 0; i < 3; ++i) {
        if (m_lpvTextures[i]) glDeleteTextures(1, &m_lpvTextures[i]);
        if (m_lpvSampleTextures[i]) glDeleteTextures(1, &m_lpvSampleTextures[i]);
        if (m_lpvSampleTexturesTemp[i]) glDeleteTextures(1, &m_lpvSampleTexturesTemp[i]);
        if (m_lpvAccumTextures[i]) glDeleteTextures(1, &m_lpvAccumTextures[i]);
    }
    if (m_geometryVolume) glDeleteTextures(1, &m_geometryVolume);
    if (m_rsmPosition) glDeleteTextures(1, &m_rsmPosition);
    if (m_rsmNormal) glDeleteTextures(1, &m_rsmNormal);
    if (m_rsmFlux) glDeleteTextures(1, &m_rsmFlux);
    if (m_rsmDepth) glDeleteTextures(1, &m_rsmDepth);
    if (m_resolvedIndirectTexture) glDeleteTextures(1, &m_resolvedIndirectTexture);
    if (m_rsmShader) glDeleteProgram(m_rsmShader);
    if (m_injectionShader) glDeleteProgram(m_injectionShader);
    if (m_propagationShader) glDeleteProgram(m_propagationShader);
    if (m_convertShader) glDeleteProgram(m_convertShader);
    if (m_resolveShader) glDeleteProgram(m_resolveShader);
    if (m_voxelizeShader) glDeleteProgram(m_voxelizeShader);
    if (m_debugShader) glDeleteProgram(m_debugShader);
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
