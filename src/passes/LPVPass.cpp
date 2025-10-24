#include "LPVPass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../DirectionalLight.h"
#include "../RenderContext.h"
#include <iostream>
#include <random>
#include <glm/gtc/matrix_transform.hpp>

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
        if (m_lpvTexturesTemp[i]) { glDeleteTextures(1, &m_lpvTexturesTemp[i]); m_lpvTexturesTemp[i] = 0; }
        if (m_lpvSampleTextures[i]) { glDeleteTextures(1, &m_lpvSampleTextures[i]); m_lpvSampleTextures[i] = 0; }
        if (m_lpvSampleTexturesTemp[i]) { glDeleteTextures(1, &m_lpvSampleTexturesTemp[i]); m_lpvSampleTexturesTemp[i] = 0; }
    }
    if (m_geometryVolume) { glDeleteTextures(1, &m_geometryVolume); m_geometryVolume = 0; }
}

void LPVPass::DestroyRSMResourcesOnly() {
    if (m_rsmPosition) { glDeleteTextures(1, &m_rsmPosition); m_rsmPosition = 0; }
    if (m_rsmNormal)   { glDeleteTextures(1, &m_rsmNormal);   m_rsmNormal = 0; }
    if (m_rsmFlux)     { glDeleteTextures(1, &m_rsmFlux);     m_rsmFlux = 0; }
    if (m_rsmDepth)    { glDeleteTextures(1, &m_rsmDepth);    m_rsmDepth = 0; }
    m_rsmFBO.reset();
}

void LPVPass::Execute(RenderContext& ctx,
                      const std::shared_ptr<SceneGraph>& sceneGraph,
                      const std::shared_ptr<Camera>& camera,
                      const std::shared_ptr<DirectionalLight>& dirLight,
                      const std::shared_ptr<Skybox>& /*skybox*/) {
    if (!config.enableLPV || !sceneGraph || !camera || !dirLight) return;

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

    // Follow camera for grid center by default
    config.gridCenter = camera->GetCameraPosition();
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

    // Voxelize occluders if needed
    if (m_geometryDirty && config.enableOcclusion) {
        VoxelizeGeometry(sceneGraph);
        m_geometryDirty = false;
    }

    // Clear integer LPV volumes and temp
    for (int i = 0; i < 3; ++i) {
        const GLuint zero = 0u;
        glBindTexture(GL_TEXTURE_3D, m_lpvTextures[i]);
        glClearTexImage(m_lpvTextures[i], 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindTexture(GL_TEXTURE_3D, m_lpvTexturesTemp[i]);
        glClearTexImage(m_lpvTexturesTemp[i], 0, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    }

    // Render RSM for current light configuration
    RenderRSM(ctx, sceneGraph, dirLight);

    // Inject VPLs into integer LPV grid (atomic adds)
    InjectVPLs();

    // Make sure injection writes are visible
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // Convert integer LPV to float RGBA16F sampling textures BEFORE propagation
    if (m_convertShader != 0) {
        glUseProgram(m_convertShader);
        GLint locRes = glGetUniformLocation(m_convertShader, "u_gridResolution");
        if (locRes >= 0) glUniform1i(locRes, config.gridResolution);
        // Inputs: integer (extended X)
        glBindImageTexture(0, m_lpvTextures[0], 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32UI);
        glBindImageTexture(1, m_lpvTextures[1], 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32UI);
        glBindImageTexture(2, m_lpvTextures[2], 0, GL_TRUE, 0, GL_READ_ONLY, GL_R32UI);
        // Outputs: float (per-voxel RGBA16F)
        glBindImageTexture(3, m_lpvSampleTextures[0], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(4, m_lpvSampleTextures[1], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(5, m_lpvSampleTextures[2], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        int groups = (config.gridResolution + 7) / 8;
        glDispatchCompute(groups, groups, groups);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    // Now propagate in float domain using ping-pong between m_lpvSampleTextures and m_lpvSampleTexturesTemp
    PropagateLPV();

    // Ensure the final float volumes are visible to the lighting pass
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
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
    int res = config.gridResolution;
    int extendedResX = res * 4; // 4 SH bands along X for integer storage

    for (int i = 0; i < 3; ++i) {
        glGenTextures(1, &m_lpvTextures[i]);
        glBindTexture(GL_TEXTURE_3D, m_lpvTextures[i]);
        glTexStorage3D(GL_TEXTURE_3D, 1, GL_R32UI, extendedResX, res, res);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &m_lpvTexturesTemp[i]);
        glBindTexture(GL_TEXTURE_3D, m_lpvTexturesTemp[i]);
        glTexStorage3D(GL_TEXTURE_3D, 1, GL_R32UI, extendedResX, res, res);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        // Float volumes used by propagation and for sampling in lighting pass
        glGenTextures(1, &m_lpvSampleTextures[i]);
        glBindTexture(GL_TEXTURE_3D, m_lpvSampleTextures[i]);
        glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, res, res, res);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &m_lpvSampleTexturesTemp[i]);
        glBindTexture(GL_TEXTURE_3D, m_lpvSampleTexturesTemp[i]);
        glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, res, res, res);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }

    // Geometry occlusion volume
    glGenTextures(1, &m_geometryVolume);
    glBindTexture(GL_TEXTURE_3D, m_geometryVolume);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_R8, res, res, res);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

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

void LPVPass::RenderRSM(RenderContext& /*ctx*/,
                        const std::shared_ptr<SceneGraph>& sceneGraph,
                        const std::shared_ptr<DirectionalLight>& dirLight) {
    if (!m_rsmFBO) return;

    m_rsmFBO->Bind();
    glViewport(0, 0, config.rsmResolution, config.rsmResolution);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glm::vec3 lightDir = glm::normalize(dirLight->GetDirection());
    glm::vec3 lightPos = config.gridCenter - lightDir * 50.0f;
    glm::vec3 up(0, 1, 0);
    if (std::abs(glm::dot(lightDir, up)) > 0.99f) up = glm::vec3(1, 0, 0);
    glm::mat4 lightView = glm::lookAt(lightPos, config.gridCenter, up);
    float orthoSize = config.gridResolution * config.voxelSize * 0.5f;
    glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, 0.1f, 100.0f);
    glm::mat4 lightViewProj = lightProj * lightView;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glUseProgram(m_rsmShader);

    GLint loc = glGetUniformLocation(m_rsmShader, "u_lightViewProj");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, &lightViewProj[0][0]);

    sceneGraph->DrawGeometry(m_rsmShader);

    FrameBuffer::Unbind();
}

void LPVPass::InjectVPLs() {
    glUseProgram(m_injectionShader);

    // RSM inputs
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_rsmPosition);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_rsmNormal);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_rsmFlux);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_rsmPosition"), 0);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_rsmNormal"), 1);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_rsmFlux"), 2);

    // Integer LPV outputs (atomic add)
    glBindImageTexture(0, m_lpvTextures[0], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32UI);
    glBindImageTexture(1, m_lpvTextures[1], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32UI);
    glBindImageTexture(2, m_lpvTextures[2], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32UI);

    // Grid params
    glUniform3fv(glGetUniformLocation(m_injectionShader, "u_gridCenter"), 1, &config.gridCenter[0]);
    glUniform1f(glGetUniformLocation(m_injectionShader, "u_voxelSize"), config.voxelSize);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_gridResolution"), config.gridResolution);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_rsmResolution"), config.rsmResolution);
    glUniform1i(glGetUniformLocation(m_injectionShader, "u_sampleCount"), config.vplSampleCount);
    glm::vec4 q(config.gridOrientation.x, config.gridOrientation.y, config.gridOrientation.z, config.gridOrientation.w);
    glUniform4fv(glGetUniformLocation(m_injectionShader, "u_gridOrientation"), 1, &q[0]);

    int groups = (config.vplSampleCount + 63) / 64;
    glDispatchCompute(groups, 1, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void LPVPass::PropagateLPV() {
    glUseProgram(m_propagationShader);

    // Common uniforms
    glUniform1i(glGetUniformLocation(m_propagationShader, "u_gridResolution"), config.gridResolution);
    glUniform1f(glGetUniformLocation(m_propagationShader, "u_attenuation"), config.propagationAttenuation);
    glUniform1f(glGetUniformLocation(m_propagationShader, "u_bias"), config.propagationBias);
    glUniform1i(glGetUniformLocation(m_propagationShader, "u_enableOcclusion"), config.enableOcclusion ? 1 : 0);

    // Occlusion sampler (binding via uniform sampler3D)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, m_geometryVolume);
    glUniform1i(glGetUniformLocation(m_propagationShader, "u_geometryVolume"), 0);

    // Work group dims
    int groups = (config.gridResolution + 7) / 8;

    for (int iter = 0; iter < config.propagationIterations; ++iter) {
        // Ping-pong float volumes
        GLuint* inVol = (iter % 2 == 0) ? m_lpvSampleTextures : m_lpvSampleTexturesTemp;
        GLuint* outVol = (iter % 2 == 0) ? m_lpvSampleTexturesTemp : m_lpvSampleTextures;

        // Bind as images with RGBA16F (matching shader layout)
        glBindImageTexture(0, inVol[0], 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
        glBindImageTexture(1, inVol[1], 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
        glBindImageTexture(2, inVol[2], 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);

        glBindImageTexture(3, outVol[0], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(4, outVol[1], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindImageTexture(5, outVol[2], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);

        glDispatchCompute(groups, groups, groups);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    // If iterations is odd, result already in m_lpvSampleTextures (last outVol)
    if (config.propagationIterations % 2 == 1) {
        // nothing to do; we wrote into m_lpvSampleTextures in the last iteration
    }
}

void LPVPass::VoxelizeGeometry(const std::shared_ptr<SceneGraph>& sceneGraph) {
    // Clear geometry volume
    glBindTexture(GL_TEXTURE_3D, m_geometryVolume);
    glClearTexImage(m_geometryVolume, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glUseProgram(m_voxelizeShader);
    glBindImageTexture(0, m_geometryVolume, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R8);
    glUniform3fv(glGetUniformLocation(m_voxelizeShader, "u_gridCenter"), 1, &config.gridCenter[0]);
    glUniform1f(glGetUniformLocation(m_voxelizeShader, "u_voxelSize"), config.voxelSize);
    glUniform1i(glGetUniformLocation(m_voxelizeShader, "u_gridResolution"), config.gridResolution);

    sceneGraph->DrawGeometry(m_voxelizeShader);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void LPVPass::RenderDebugVisualization(const glm::mat4& view, const glm::mat4& projection) {
    glUseProgram(m_debugShader);
    glUniformMatrix4fv(glGetUniformLocation(m_debugShader, "u_view"), 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(m_debugShader, "u_projection"), 1, GL_FALSE, &projection[0][0]);
    glUniform3fv(glGetUniformLocation(m_debugShader, "u_gridCenter"), 1, &config.gridCenter[0]);
    glUniform1f(glGetUniformLocation(m_debugShader, "u_voxelSize"), config.voxelSize);
    glUniform1i(glGetUniformLocation(m_debugShader, "u_gridResolution"), config.gridResolution);

    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_3D, m_lpvSampleTextures[0]);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_3D, m_lpvSampleTextures[1]);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_3D, m_lpvSampleTextures[2]);
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
    glPointSize(5.0f);
    glDrawArrays(GL_POINTS, 0, count * count * count);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void LPVPass::CleanupResources() {
    for (int i = 0; i < 3; ++i) {
        if (m_lpvTextures[i]) glDeleteTextures(1, &m_lpvTextures[i]);
        if (m_lpvTexturesTemp[i]) glDeleteTextures(1, &m_lpvTexturesTemp[i]);
        if (m_lpvSampleTextures[i]) glDeleteTextures(1, &m_lpvSampleTextures[i]);
        if (m_lpvSampleTexturesTemp[i]) glDeleteTextures(1, &m_lpvSampleTexturesTemp[i]);
    }
    if (m_geometryVolume) glDeleteTextures(1, &m_geometryVolume);
    if (m_rsmPosition) glDeleteTextures(1, &m_rsmPosition);
    if (m_rsmNormal) glDeleteTextures(1, &m_rsmNormal);
    if (m_rsmFlux) glDeleteTextures(1, &m_rsmFlux);
    if (m_rsmDepth) glDeleteTextures(1, &m_rsmDepth);
    if (m_rsmShader) glDeleteProgram(m_rsmShader);
    if (m_injectionShader) glDeleteProgram(m_injectionShader);
    if (m_propagationShader) glDeleteProgram(m_propagationShader);
    if (m_convertShader) glDeleteProgram(m_convertShader);
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
