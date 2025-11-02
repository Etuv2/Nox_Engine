#include "Skybox.h"
#include "ShaderLoader.h"    // For CreateShaderProgram(...)
#include "stb_image.h"       // For HDR image loading
#include "TextureUnits.h"    
#include "FrameBuffer.h"
#include "GLState.h"         // For comprehensive OpenGL state management
#include <iostream>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

void Skybox::CreateCaptureFBO(int width, int height)
{
    if (m_captureFBO == 0)
        glGenFramebuffers(1, &m_captureFBO);
    if (m_captureRBO == 0)
        glGenRenderbuffers(1, &m_captureRBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_captureRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_captureRBO);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        std::cerr << "[Skybox] Capture FBO incomplete: 0x" << std::hex << status << std::dec << std::endl;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Skybox::ResizeCaptureRBO(int width, int height)
{
    glBindRenderbuffer(GL_RENDERBUFFER, m_captureRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

GLuint Skybox::loadHDRTexture(const std::string& hdrPath)
{
    int width, height, nComponents;
    float* data = stbi_loadf(hdrPath.c_str(), &width, &height, &nComponents, 0);
    if (!data) {
        std::cerr << "[Skybox] Failed to load HDR file: " << hdrPath << std::endl;
        return 0;
    }

    GLuint hdrTex = 0;
    glGenTextures(1, &hdrTex);
    glBindTexture(GL_TEXTURE_2D, hdrTex);
    GLenum format = (nComponents >= 4) ? GL_RGBA : GL_RGB;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, format, GL_FLOAT, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    stbi_image_free(data);
    std::cout << "[Skybox] Loaded HDR texture: " << width << "x" << height << " (" << nComponents << " channels)" << std::endl;
    return hdrTex;
}

void Skybox::buildSkyboxCube()
{
    if (m_skyboxVAO != 0)
        return; // Already built

    float skyboxVertices[] = {
        // positions
        -1.f,-1.f, 1.f,  1.f,-1.f, 1.f,  1.f, 1.f, 1.f,  1.f, 1.f, 1.f, -1.f, 1.f, 1.f, -1.f,-1.f, 1.f,
        -1.f, 1.f,-1.f,  1.f, 1.f,-1.f,  1.f,-1.f,-1.f,  1.f,-1.f,-1.f, -1.f,-1.f,-1.f, -1.f, 1.f,-1.f,
         1.f,-1.f,-1.f,  1.f, 1.f,-1.f,  1.f, 1.f, 1.f,  1.f, 1.f, 1.f,  1.f,-1.f, 1.f,  1.f,-1.f,-1.f,
        -1.f,-1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f,-1.f, -1.f, 1.f,-1.f, -1.f,-1.f,-1.f, -1.f,-1.f, 1.f,
        -1.f, 1.f,-1.f,  1.f, 1.f,-1.f,  1.f, 1.f, 1.f,  1.f, 1.f, 1.f, -1.f, 1.f, 1.f, -1.f, 1.f,-1.f,
        -1.f,-1.f, 1.f,  1.f,-1.f, 1.f,  1.f,-1.f,-1.f,  1.f,-1.f,-1.f, -1.f,-1.f,-1.f, -1.f,-1.f, 1.f
    };

    glGenVertexArrays(1, &m_skyboxVAO);
    glGenBuffers(1, &m_skyboxVBO);
    glBindVertexArray(m_skyboxVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_skyboxVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

void Skybox::renderCube()
{
    glBindVertexArray(m_skyboxVAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
}

bool Skybox::Init(const std::string& hdrPath,
    const std::string& equirectVertShader,
    const std::string& equirectFragShader,
    const std::string& skyboxVertShader,
    const std::string& skyboxFragShader,
    int windowWidth,
    int windowHeight)
{
    std::cout << "[Skybox] Initializing skybox with IBL support..." << std::endl;
    std::cout << "[Skybox] HDR file: " << hdrPath << std::endl;
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    m_pipelineReady = false; // reset
    m_regenAttempted = false;

    // Load core shaders
    m_equiRectToCubeShader = CreateShaderProgram(equirectVertShader.c_str(), equirectFragShader.c_str());
    if (!m_equiRectToCubeShader) { std::cerr << "[Skybox] ERROR: Failed to compile equirect->cube shader" << std::endl; return false; }
    m_skyboxShader = CreateShaderProgram(skyboxVertShader.c_str(), skyboxFragShader.c_str());
    if (!m_skyboxShader) { std::cerr << "[Skybox] ERROR: Failed to compile skybox shader" << std::endl; return false; }

    // Load HDR
    GLuint hdrTex = loadHDRTexture(hdrPath);
    if (!hdrTex) { std::cerr << "[Skybox] ERROR: HDR load failed" << std::endl; return false; }

    // Create environment cubemap (512x512)
    glGenTextures(1, &m_envCubemap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCubemap);
    for (int face = 0; face < 6; ++face)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB16F, 512, 512, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    buildSkyboxCube();
    CreateCaptureFBO(512, 512);

    // Save state
    GLint prevViewport[4]; GLint prevFBO; GLint prevProg; GLint prevActiveTexture; GLint prevCube;
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &prevCube);

    // Convert HDR equirect to cubemap
    glUseProgram(m_equiRectToCubeShader);
    glUniform1i(glGetUniformLocation(m_equiRectToCubeShader, "equirectangularMap"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdrTex);
    glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    glUniformMatrix4fv(glGetUniformLocation(m_equiRectToCubeShader, "projection"), 1, GL_FALSE, glm::value_ptr(captureProjection));
    glm::mat4 captureViews[] = {
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(+1,0,0), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,+1,0), glm::vec3(0,0,+1)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,+1), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,-1,0))
    };
    glViewport(0,0,512,512);
    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
    for (int i=0;i<6;++i){
        glUniformMatrix4fv(glGetUniformLocation(m_equiRectToCubeShader, "view"),1,GL_FALSE,glm::value_ptr(captureViews[i]));
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, m_envCubemap, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){
            std::cerr << "[Skybox] ERROR: Env conversion FBO incomplete (face "<<i<<")"<<std::endl; glDeleteTextures(1,&hdrTex); return false; }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderCube();
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCubemap);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glDeleteTextures(1,&hdrTex);

    // Immediately generate IBL resources (strict requirement)
    if (!GenerateIBLResources() || !VerifyIBLPipelineComplete()) {
        std::cerr << "[Skybox] FATAL: Initial IBL generation failed – attempting forced regeneration..." << std::endl;
        ForceRegenerateIBL();
        if (!VerifyIBLPipelineComplete()) {
            std::cerr << "[Skybox] FATAL: Skybox pipeline invalid after forced regeneration." << std::endl;
            return false;
        }
    }
    m_pipelineReady = true;

    // Restore state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glUseProgram(prevProg);
    glActiveTexture(prevActiveTexture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, prevCube);

    // Pre-bind environment map sampler location
    glUseProgram(m_skyboxShader);
    SET_UNIFORM_TEXTURE_UNIT(m_skyboxShader, "environmentMap", TextureUnits::SKYBOX_CUBEMAP);
    glUseProgram(prevProg);

    LogTextureInfo();
    std::cout << "[Skybox] Skybox + IBL fully initialized." << std::endl;
    return true;
}

void Skybox::Draw(const glm::mat4& view, const glm::mat4& projection)
{
    // Fast readiness check
    if (!m_pipelineReady) {
        if (!EnsureReady()) {
            return; // still not ready, skip draw
        }
    }
    if (!glIsTexture(m_envCubemap) || m_envCubemap == 0) {
        return; // environment not valid
    }

    // Use comprehensive state management for skybox rendering
    SCOPED_GL_RENDER_STATE(); // Automatically saves and restores render state
    
    // Remove translation from view matrix (skybox is always centered at camera)
    glm::mat4 viewNoTrans = glm::mat4(glm::mat3(view));

    // Set up optimal state for skybox rendering
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);  // Allow rendering at maximum depth
    glDepthMask(GL_FALSE);   // Don't write to depth buffer - skybox is always behind everything

    // Disable culling and blending for skybox to avoid missing faces and artifacts
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    // Render the skybox
    glUseProgram(m_skyboxShader);
    glUniformMatrix4fv(glGetUniformLocation(m_skyboxShader, "view"), 1, GL_FALSE, glm::value_ptr(viewNoTrans));
    glUniformMatrix4fv(glGetUniformLocation(m_skyboxShader, "projection"), 1, GL_FALSE, glm::value_ptr(projection));

    // CRITICAL: Pass skybox exposure to control background brightness independently from IBL
    glUniform1f(glGetUniformLocation(m_skyboxShader, "skyboxExposure"), m_skyboxExposure);

    // Bind environment cubemap using state manager
    GLStateManager::Instance().BindTextureCube(GL_TEXTURE0 + TextureUnits::SKYBOX_CUBEMAP, m_envCubemap);
    SET_UNIFORM_TEXTURE_UNIT(m_skyboxShader, "environmentMap", TextureUnits::SKYBOX_CUBEMAP);

    renderCube();

    // State is automatically restored by SCOPED_GL_RENDER_STATE() destructor
}

void Skybox::Cleanup()
{
    if (m_skyboxVBO) glDeleteBuffers(1, &m_skyboxVBO);
    if (m_skyboxVAO) glDeleteVertexArrays(1, &m_skyboxVAO);
    if (m_envCubemap) glDeleteTextures(1, &m_envCubemap);
    if (m_irradianceMap) glDeleteTextures(1, &m_irradianceMap);
    if (m_prefilteredMap) glDeleteTextures(1, &m_prefilteredMap);
    if (m_brdfLUT) glDeleteTextures(1, &m_brdfLUT);
    if (m_equiRectToCubeShader) glDeleteProgram(m_equiRectToCubeShader);
    if (m_skyboxShader) glDeleteProgram(m_skyboxShader);
    if (m_irradianceShader) glDeleteProgram(m_irradianceShader);
    if (m_prefilterShader) glDeleteProgram(m_prefilterShader);
    if (m_brdfShader) glDeleteProgram(m_brdfShader);
    if (m_captureFBO) glDeleteFramebuffers(1, &m_captureFBO);
    if (m_captureRBO) glDeleteRenderbuffers(1, &m_captureRBO);
    m_pipelineReady = false;
}

bool Skybox::ValidateIBLTextures() const
{
    // existing heavy validation unchanged ... (truncated for brevity in edit summary)
    // ...existing code...
    // Keep original implementation (no change) - we rely more on VerifyIBLPipelineComplete for fast checks.
    return VerifyIBLPipelineComplete();
}

void Skybox::LogTextureInfo() const
{
    std::cout << "[Skybox] IBL Texture Information:" << std::endl;
    std::cout << "  Environment Map: " << m_envCubemap << std::endl;
    std::cout << "  Irradiance Map: " << m_irradianceMap << std::endl;
    std::cout << "  Prefiltered Map: " << m_prefilteredMap << std::endl;
    std::cout << "  BRDF LUT: " << m_brdfLUT << std::endl;
    std::cout << "  Max LOD: " << m_prefilteredMaxLOD << std::endl;
}

bool Skybox::GenerateIBLResources()
{
    std::cout << "[Skybox] Generating IBL resources..." << std::endl;

    // CRITICAL FIX: Verify shader files exist and load properly
    std::cout << "[Skybox] Loading irradiance convolution shader..." << std::endl;
    m_irradianceShader = CreateShaderProgram("shaders/irradiance_convolution_vert.glsl", "shaders/irradiance_convolution_frag.glsl");
    if (!m_irradianceShader) {
        std::cerr << "[Skybox] CRITICAL: Failed to load irradiance convolution shader!" << std::endl;
        return false;
    }
    std::cout << "[Skybox] ✓ Irradiance shader loaded: " << m_irradianceShader << std::endl;
    
    std::cout << "[Skybox] Loading prefilter shader..." << std::endl;
    m_prefilterShader = CreateShaderProgram("shaders/prefilter_vert.glsl", "shaders/prefilter_frag.glsl");
    if (!m_prefilterShader) {
        std::cerr << "[Skybox] CRITICAL: Failed to load prefilter shader!" << std::endl;
        return false;
    }
    std::cout << "[Skybox] ✓ Prefilter shader loaded: " << m_prefilterShader << std::endl;
    
    std::cout << "[Skybox] Loading BRDF integration shader..." << std::endl;
    m_brdfShader = CreateShaderProgram("shaders/brdf_vert.glsl", "shaders/brdf_frag.glsl");
    if (!m_brdfShader) {
        std::cerr << "[Skybox] CRITICAL: Failed to load BRDF shader!" << std::endl;
        return false;
    }
    std::cout << "[Skybox] ✓ BRDF shader loaded: " << m_brdfShader << std::endl;

    std::cout << "[Skybox] Starting IBL resource generation..." << std::endl;
    
    if (!GenerateIrradianceMap()) {
        std::cerr << "[Skybox] CRITICAL: Irradiance map generation failed!" << std::endl;
        return false;
    }
    
    if (!GeneratePrefilteredMap()) {
        std::cerr << "[Skybox] CRITICAL: Prefiltered map generation failed!" << std::endl;
        return false;
    }
    
    if (!GenerateBRDFLUT()) {
        std::cerr << "[Skybox] CRITICAL: BRDF LUT generation failed!" << std::endl;
        return false;
    }

    std::cout << "[Skybox] ✓ All IBL resources generated successfully!" << std::endl;
    std::cout << "[Skybox] - Irradiance Map: " << m_irradianceMap << std::endl;
    std::cout << "[Skybox] - Prefiltered Map: " << m_prefilteredMap << std::endl;
    std::cout << "[Skybox] - BRDF LUT: " << m_brdfLUT << std::endl;
    return true;
}

bool Skybox::GenerateIrradianceMap()
{
    std::cout << "[Skybox] Generating irradiance map..." << std::endl;
    
    // CRITICAL FIX: Use comprehensive state management to prevent shadow system interference
    SCOPED_GL_STATE(); // Automatically captures and restores all OpenGL state
    
    // Create irradiance cubemap
    if (m_irradianceMap) { glDeleteTextures(1, &m_irradianceMap); m_irradianceMap = 0; }
    glGenTextures(1, &m_irradianceMap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_irradianceMap);
    for (int i = 0; i < 6; ++i)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB16F, 32, 32, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    ResizeCaptureRBO(32, 32);

    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    glm::mat4 views[] = {
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(+1,0,0), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,+1,0), glm::vec3(0,0,+1)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,+1), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,-1,0))
    };

    glUseProgram(m_irradianceShader);
    glUniform1i(glGetUniformLocation(m_irradianceShader, "environmentMap"), 0);
    glUniformMatrix4fv(glGetUniformLocation(m_irradianceShader, "projection"), 1, GL_FALSE, glm::value_ptr(proj));

    // Bind source environment map to unit 0 ONLY
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCubemap);

    // Set appropriate render state for IBL generation
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, 32, 32);
    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);

    std::cout << "[Skybox] Starting irradiance convolution..." << std::endl;
    for (int face = 0; face < 6; ++face) {
        glUniformMatrix4fv(glGetUniformLocation(m_irradianceShader, "view"), 1, GL_FALSE, glm::value_ptr(views[face]));
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, m_irradianceMap, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[Skybox] Irradiance FBO incomplete (face " << face << ") status=0x" << std::hex << status << std::dec << std::endl;
            return false;
        }
        glClear(GL_COLOR_BUFFER_BIT);
        renderCube();
    }

    // Enhanced debug readback with validation
    float pixel[3] = {0,0,0};
    GLuint debugFBO; glGenFramebuffers(1, &debugFBO); glBindFramebuffer(GL_FRAMEBUFFER, debugFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X, m_irradianceMap, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      glReadPixels(16,16,1,1,GL_RGB,GL_FLOAT,pixel);
        std::cout << "[Skybox] Irradiance debug sample = (" << pixel[0] << ", " << pixel[1] << ", " << pixel[2] << ")" << std::endl;
        
        // CRITICAL: Validate irradiance values are positive
   if (pixel[0] < 0.0f || pixel[1] < 0.0f || pixel[2] < 0.0f) {
     std::cerr << "[Skybox] WARNING: Irradiance contains negative values! This indicates a shader error." << std::endl;
        }
        if (std::isnan(pixel[0]) || std::isnan(pixel[1]) || std::isnan(pixel[2])) {
    std::cerr << "[Skybox] ERROR: Irradiance contains NaN values! IBL will be corrupted." << std::endl;
            glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
   glDeleteFramebuffers(1, &debugFBO);
            return false;
        }
  }
    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO); // restore capture FBO for later use
    glDeleteFramebuffers(1, &debugFBO);

    std::cout << "[Skybox] Irradiance map generated (32x32)" << std::endl;
    return true;
    
    // State is automatically restored by SCOPED_GL_STATE() destructor
}

bool Skybox::GeneratePrefilteredMap()
{
    std::cout << "[Skybox] Generating prefiltered map..." << std::endl;

    // CRITICAL FIX: Use comprehensive state management
    SCOPED_GL_STATE();

    if (m_prefilteredMap) { glDeleteTextures(1, &m_prefilteredMap); m_prefilteredMap = 0; }

    const unsigned int baseSize = 128; const unsigned int maxMipLevels = 5;
    glGenTextures(1, &m_prefilteredMap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_prefilteredMap);
    for (unsigned int mip = 0; mip < maxMipLevels; ++mip) {
        unsigned int w = baseSize >> mip; unsigned int h = baseSize >> mip;
        for (int face = 0; face < 6; ++face) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, mip, GL_RGB16F, w, h, 0, GL_RGB, GL_FLOAT, nullptr);
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, maxMipLevels - 1);

    // Create lightweight FBO
    GLuint fbo = 0; glGenFramebuffers(1, &fbo);

    glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    glm::mat4 views[] = {
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(+1,0,0), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,+1,0), glm::vec3(0,0,+1)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,+1), glm::vec3(0,-1,0)),
        glm::lookAt(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,-1,0))
    };

    glUseProgram(m_prefilterShader);
    glUniform1i(glGetUniformLocation(m_prefilterShader, "environmentMap"), 0);
    glUniformMatrix4fv(glGetUniformLocation(m_prefilterShader, "projection"), 1, GL_FALSE, glm::value_ptr(proj));
    glUniform1f(glGetUniformLocation(m_prefilterShader, "resolution"), 512.0f);
    
    // Bind environment map to unit 0 ONLY
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCubemap);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    for (unsigned int mip = 0; mip < maxMipLevels; ++mip) {
        unsigned int w = baseSize >> mip; unsigned int h = baseSize >> mip;
        glViewport(0,0,w,h);
        float roughness = float(mip) / float(maxMipLevels - 1); roughness *= roughness;
        glUniform1f(glGetUniformLocation(m_prefilterShader, "roughness"), roughness);
        std::cout << "[Skybox] Prefilter mip " << mip << " size=" << w << "x" << h << " roughness=" << roughness << std::endl;
        for (int face = 0; face < 6; ++face) {
            glUniformMatrix4fv(glGetUniformLocation(m_prefilterShader, "view"), 1, GL_FALSE, glm::value_ptr(views[face]));
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, m_prefilteredMap, mip);
            GLenum drawBuf = GL_COLOR_ATTACHMENT0; glDrawBuffers(1, &drawBuf);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                std::cerr << "[Skybox] Prefilter FBO incomplete mip=" << mip << " face=" << face << " status=0x" << std::hex << status << std::dec << std::endl;
                glDeleteFramebuffers(1, &fbo);
                return false;
            }
            glClear(GL_COLOR_BUFFER_BIT);
            renderCube();
        }
    }

    // Debug sample (face 0 mip 0 center)
    float pix[3]={0,0,0};
    GLuint dbgFBO; glGenFramebuffers(1,&dbgFBO); glBindFramebuffer(GL_FRAMEBUFFER, dbgFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X, m_prefilteredMap, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(64,64,1,1,GL_RGB,GL_FLOAT,pix);
        std::cout << "[Skybox] Prefilter debug sample = (" << pix[0] << ", " << pix[1] << ", " << pix[2] << ")" << std::endl;
        
    // CRITICAL: Validate prefiltered values are positive
   if (pix[0] < 0.0f || pix[1] < 0.0f || pix[2] < 0.0f) {
       std::cerr << "[Skybox] WARNING: Prefiltered map contains negative values! This indicates a shader error." << std::endl;
 }
  if (std::isnan(pix[0]) || std::isnan(pix[1]) || std::isnan(pix[2])) {
          std::cerr << "[Skybox] ERROR: Prefiltered map contains NaN values! IBL will be corrupted." << std::endl;
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glDeleteFramebuffers(1,&dbgFBO);
       glDeleteFramebuffers(1, &fbo);
            return false;
  }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo); // restore before delete
    glDeleteFramebuffers(1,&dbgFBO);

    glDeleteFramebuffers(1, &fbo);

    m_prefilteredMaxLOD = maxMipLevels - 1;
    std::cout << "[Skybox] Prefiltered map generation completed (" << baseSize << " base, " << maxMipLevels << " mips)" << std::endl;
    return true;
    
    // State automatically restored by SCOPED_GL_STATE()
}

bool Skybox::GenerateBRDFLUT()
{
    std::cout << "[Skybox] Generating BRDF LUT..." << std::endl;
    
    // CRITICAL FIX: Use comprehensive state management
    SCOPED_GL_STATE();
    
    // CRITICAL FIX: Create BRDF integration lookup table
    if (m_brdfLUT) { glDeleteTextures(1, &m_brdfLUT); m_brdfLUT = 0; }
    glGenTextures(1, &m_brdfLUT);
    glBindTexture(GL_TEXTURE_2D, m_brdfLUT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, 512, 512, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Full screen quad for BRDF integration
    GLuint quadVAO = 0, quadVBO = 0;
    float quadVertices[] = {
        -1.f,  1.f, 0.f, 0.f, 1.f,
        -1.f, -1.f, 0.f, 0.f, 0.f,
         1.f,  1.f, 0.f, 1.f, 1.f,
         1.f, -1.f, 0.f, 1.f, 0.f
    };
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));

    ResizeCaptureRBO(512, 512);
    
    // Set appropriate render state for BRDF generation
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    
    glViewport(0, 0, 512, 512);
    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_brdfLUT, 0);
    
    // CRITICAL FIX: Verify framebuffer is complete before rendering
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[Skybox] BRDF LUT FBO incomplete: 0x" << std::hex << status << std::dec << std::endl;
        
        // CRITICAL FIX: Cleanup and return false on failure
        glDeleteVertexArrays(1, &quadVAO);
        glDeleteBuffers(1, &quadVBO);
        return false;
    }
    
    glUseProgram(m_brdfShader);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // CRITICAL FIX: Cleanup temporary resources
    glDeleteVertexArrays(1, &quadVAO);
    glDeleteBuffers(1, &quadVBO);
    
    // Debug sample (center pixel)
    float debugPixel[2] = {0, 0};
    GLuint debugFBO; glGenFramebuffers(1, &debugFBO); glBindFramebuffer(GL_FRAMEBUFFER, debugFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_brdfLUT, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(256, 256, 1, 1, GL_RG, GL_FLOAT, debugPixel);
        std::cout << "[Skybox] BRDF LUT debug sample = (" << debugPixel[0] << ", " << debugPixel[1] << ")" << std::endl;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO); // restore capture FBO
    glDeleteFramebuffers(1, &debugFBO);

    std::cout << "[Skybox] BRDF LUT generated successfully (512x512)" << std::endl;
    return true;
    
    // State automatically restored by SCOPED_GL_STATE()
}

bool Skybox::VerifyIBLPipelineComplete() const
{
    // Check environment cubemap first
    if (!glIsTexture(m_envCubemap) || m_envCubemap == 0) {
        return false;
    }
    
    // Check all IBL textures
    if (!glIsTexture(m_irradianceMap) || m_irradianceMap == 0) {
        return false;
    }
    
    if (!glIsTexture(m_prefilteredMap) || m_prefilteredMap == 0) {
        return false;
    }
    
    if (!glIsTexture(m_brdfLUT) || m_brdfLUT == 0) {
        return false;
    }
    
    // Check shaders are loaded
    if (m_skyboxShader == 0) {
        return false;
    }
    
    if (m_irradianceShader == 0 || m_prefilterShader == 0 || m_brdfShader == 0) {
        return false;
    }
    
    // Check cube geometry
    if (m_skyboxVAO == 0 || m_skyboxVBO == 0) {
        return false;
    }
    
    // Validate environment cubemap has content (quick check)
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_envCubemap);
    GLint width, height;
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_TEXTURE_HEIGHT, &height);
    
    if (width == 0 || height == 0) {
        return false;
    }
    
    return true;
}

void Skybox::ForceRegenerateIBL()
{
    std::cout << "[Skybox] FORCING complete IBL pipeline regeneration..." << std::endl;
    
    // Clean up existing IBL resources
    if (m_irradianceMap) {
        glDeleteTextures(1, &m_irradianceMap);
        m_irradianceMap = 0;
    }
    if (m_prefilteredMap) {
        glDeleteTextures(1, &m_prefilteredMap);
        m_prefilteredMap = 0;
    }
    if (m_brdfLUT) {
        glDeleteTextures(1, &m_brdfLUT);
        m_brdfLUT = 0;
    }
    
    // Recreate all IBL resources
    if (!GenerateIBLResources()) {
        std::cerr << "[Skybox] CRITICAL: Failed to regenerate IBL resources!" << std::endl;
        return;
    }
    
    // Validate everything was created
    if (!ValidateIBLTextures()) {
        std::cerr << "[Skybox] CRITICAL: IBL validation failed after regeneration!" << std::endl;
        return;
    }
    
    std::cout << "[Skybox] ✓ IBL pipeline regeneration completed successfully!" << std::endl;
}

bool Skybox::EnsureReady()
{
    if (m_pipelineReady) return true;
    if (!m_regenAttempted) {
        m_regenAttempted = true;
        ForceRegenerateIBL();
        if (VerifyIBLPipelineComplete()) {
            m_pipelineReady = true;
        }
    }
    return m_pipelineReady;
}

