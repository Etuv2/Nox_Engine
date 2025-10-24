#include "LightingPass.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../Skybox.h"
#include "../LightManager.h"
#include "../TextureUnits.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

LightingPass::LightingPass() {}

LightingPass::~LightingPass() {
    if (m_shader) glDeleteProgram(m_shader);
    if (m_fallbackCubemap) glDeleteTextures(1, &m_fallbackCubemap);
    if (m_fallbackBRDF) glDeleteTextures(1, &m_fallbackBRDF);
}

bool LightingPass::Initialize(RenderContext& context) {
    m_shader = CreateShaderProgram("shaders/deferred_lighting_vert.glsl", 
                                   "shaders/deferred_lighting_frag.glsl");
    if (!m_shader) {
        std::cout << "[LightingPass] Trying fallback unified multi-light shader...\n";
        m_shader = CreateShaderProgram("shaders/fullscreen_vert.glsl", 
                                      "shaders/unified_multi_light_deferred.glsl");
        if (!m_shader) {
            std::cerr << "[LightingPass] Failed to create lighting shader.\n";
            return false;
        }
    }
    
    SetupFallbackIBL();
    
    std::cout << "[LightingPass] Initialized successfully.\n";
    return true;
}

void LightingPass::SetupFallbackIBL() {
    // Create white cubemap fallback
    glGenTextures(1, &m_fallbackCubemap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_fallbackCubemap);
    
    float whitePixel[3] = {1.0f, 1.0f, 1.0f};
    for (int face = 0; face < 6; ++face) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB16F, 
                     1, 1, 0, GL_RGB, GL_FLOAT, whitePixel);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    
    // Create neutral BRDF LUT fallback
    glGenTextures(1, &m_fallbackBRDF);
    glBindTexture(GL_TEXTURE_2D, m_fallbackBRDF);
    float brdfPixel[2] = {0.5f, 0.5f};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, 1, 1, 0, GL_RG, GL_FLOAT, brdfPixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void LightingPass::Resize(RenderContext& context, int newWidth, int newHeight) {
    // HDR FBO resized by coordinator
}

void LightingPass::Execute(RenderContext& ctx,
                           const std::shared_ptr<SceneGraph>& sceneGraph,
                           const std::shared_ptr<Camera>& camera,
                           const std::shared_ptr<DirectionalLight>& dirLight,
                           const std::shared_ptr<Skybox>& skybox) {
    std::cout << "[LightingPass] Starting execution..." << std::endl;
    
    if (!camera) {
        std::cerr << "[LightingPass] ERROR: No camera!" << std::endl;
        return;
    }

    if (!ctx.hdrFBO) {
        std::cerr << "[LightingPass] ERROR: HDR FBO is null!" << std::endl;
        return;
    }

    if (!ctx.gbufferFBO) {
        std::cerr << "[LightingPass] ERROR: G-buffer FBO is null!" << std::endl;
        return;
    }

    std::cout << "[LightingPass] Binding HDR FBO (ID: " << ctx.hdrFBO->GetFBO() << ")" << std::endl;
    
    // Bind HDR FBO
    ctx.hdrFBO->Bind();
    glViewport(0, 0, ctx.width, ctx.height);
    
    // CRITICAL: Match legacy renderer - disable blending and enable depth test
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    std::cout << "[LightingPass] Copying depth from G-buffer..." << std::endl;
    // Copy depth from G-buffer to HDR FBO
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.gbufferFBO->GetFBO());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx.hdrFBO->GetFBO());
    glBlitFramebuffer(0, 0, ctx.width, ctx.height, 
                      0, 0, ctx.width, ctx.height, 
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx.hdrFBO->GetFBO());

    // CRITICAL: After depth copy, disable depth test for fullscreen quad
    glDisable(GL_DEPTH_TEST);

    std::cout << "[LightingPass] Using shader program: " << m_shader << std::endl;
    glUseProgram(m_shader);

    std::cout << "[LightingPass] Binding G-buffer textures..." << std::endl;
    // Bind G-buffer attachments
    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_NORMAL);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));
    
    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_ROUGH_METAL);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(1));
    
    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_ALBEDO);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(2));
    
    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_EMISSIVE);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(3));
    
    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_SPECULAR);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(4));
    
    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_OCCLUSION);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(5));
    
    glActiveTexture(GL_TEXTURE0 + TextureUnits::GBUFFER_DEPTH);
    glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());

    // Bind SSAO (from SSAOPass output - blur buffer 1)
    std::cout << "[LightingPass] Binding SSAO texture: " << m_ssaoTexture << std::endl;
    glActiveTexture(GL_TEXTURE0 + TextureUnits::SSAO_MAP);
    if (m_ssaoTexture > 0 && glIsTexture(m_ssaoTexture)) {
        glBindTexture(GL_TEXTURE_2D, m_ssaoTexture);
    } else {
        std::cerr << "[LightingPass] WARNING: Invalid SSAO texture!" << std::endl;
    }

    // Bind Screen-Space Shadow (contact shadow)
    std::cout << "[LightingPass] Binding screen-space shadow texture: " << m_sssTexture << std::endl;
    glActiveTexture(GL_TEXTURE0 + TextureUnits::SCREEN_SPACE_SHADOW_MAP);
    if (m_sssTexture > 0 && glIsTexture(m_sssTexture)) {
        glBindTexture(GL_TEXTURE_2D, m_sssTexture);
    } else {
        // Bind a white texture as fallback (no shadowing)
        std::cout << "[LightingPass] No valid screen-space shadow texture - using white fallback" << std::endl;
    }
    
    // NEW: Bind SSGI (Screen Space Global Illumination)
    std::cout << "[LightingPass] Binding SSGI texture: " << m_ssgiTexture << std::endl;
    glActiveTexture(GL_TEXTURE0 + TextureUnits::SSGI_MAP);
    if (m_ssgiTexture > 0 && glIsTexture(m_ssgiTexture)) {
        glBindTexture(GL_TEXTURE_2D, m_ssgiTexture);
    } else {
        std::cout << "[LightingPass] No valid SSGI texture - indirect diffuse will be disabled" << std::endl;
    }
    
    // NEW: Bind LPV 3D textures for global illumination
    bool lpvEnabled = ctx.enableLPV && m_lpvTextureR > 0 && m_lpvTextureG > 0 && m_lpvTextureB > 0;
    std::cout << "[LightingPass] LPV enabled: " << (lpvEnabled ? "YES" : "NO") << std::endl;
    
    if (lpvEnabled) {
        std::cout << "[LightingPass] Binding LPV textures - R:" << m_lpvTextureR 
                  << " G:" << m_lpvTextureG << " B:" << m_lpvTextureB << std::endl;
        
        glActiveTexture(GL_TEXTURE0 + TextureUnits::LPV_TEXTURE_R);
        glBindTexture(GL_TEXTURE_3D, m_lpvTextureR);
        
        glActiveTexture(GL_TEXTURE0 + TextureUnits::LPV_TEXTURE_G);
        glBindTexture(GL_TEXTURE_3D, m_lpvTextureG);
        
        glActiveTexture(GL_TEXTURE0 + TextureUnits::LPV_TEXTURE_B);
        glBindTexture(GL_TEXTURE_3D, m_lpvTextureB);
    } else {
        std::cout << "[LightingPass] No LPV textures available" << std::endl;
    }
    
    // Set sampler uniforms
    glUniform1i(glGetUniformLocation(m_shader, "gNormal"), TextureUnits::GBUFFER_NORMAL);
    glUniform1i(glGetUniformLocation(m_shader, "gRoughMetal"), TextureUnits::GBUFFER_ROUGH_METAL);
    glUniform1i(glGetUniformLocation(m_shader, "gAlbedo"), TextureUnits::GBUFFER_ALBEDO);
    glUniform1i(glGetUniformLocation(m_shader, "gEmissive"), TextureUnits::GBUFFER_EMISSIVE);
    glUniform1i(glGetUniformLocation(m_shader, "gSpecularF0"), TextureUnits::GBUFFER_SPECULAR);
    glUniform1i(glGetUniformLocation(m_shader, "gOcclusion"), TextureUnits::GBUFFER_OCCLUSION);
    glUniform1i(glGetUniformLocation(m_shader, "gDepth"), TextureUnits::GBUFFER_DEPTH);
    glUniform1i(glGetUniformLocation(m_shader, "ssaoMap"), TextureUnits::SSAO_MAP);
    glUniform1i(glGetUniformLocation(m_shader, "screenSpaceShadowMap"), TextureUnits::SCREEN_SPACE_SHADOW_MAP);
    
    // NEW: Set LPV sampler uniforms
    if (lpvEnabled) {
        glUniform1i(glGetUniformLocation(m_shader, "lpvTextureR"), TextureUnits::LPV_TEXTURE_R);
        glUniform1i(glGetUniformLocation(m_shader, "lpvTextureG"), TextureUnits::LPV_TEXTURE_G);
        glUniform1i(glGetUniformLocation(m_shader, "lpvTextureB"), TextureUnits::LPV_TEXTURE_B);
    }

    // Bind IBL textures (skybox or fallback)
    bool useValidIBL = (skybox && skybox->ValidateIBLTextures());
    std::cout << "[LightingPass] Using " << (useValidIBL ? "valid" : "fallback") << " IBL" << std::endl;
    
    if (useValidIBL) {
        glActiveTexture(GL_TEXTURE0 + TextureUnits::IRRADIANCE_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetIrradianceMap());
        
        glActiveTexture(GL_TEXTURE0 + TextureUnits::PREFILTERED_ENV_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetPrefilteredMap());
        
        glActiveTexture(GL_TEXTURE0 + TextureUnits::BRDF_LUT);
        glBindTexture(GL_TEXTURE_2D, skybox->GetBRDFLUT());
        
        glUniform1f(glGetUniformLocation(m_shader, "prefilteredMaxLOD"), 
                    skybox->GetPrefilteredMaxLOD());
    } else {
        // Use fallback IBL
        glActiveTexture(GL_TEXTURE0 + TextureUnits::IRRADIANCE_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, m_fallbackCubemap);
        
        glActiveTexture(GL_TEXTURE0 + TextureUnits::PREFILTERED_ENV_MAP);
        glBindTexture(GL_TEXTURE_CUBE_MAP, m_fallbackCubemap);
        
        glActiveTexture(GL_TEXTURE0 + TextureUnits::BRDF_LUT);
        glBindTexture(GL_TEXTURE_2D, m_fallbackBRDF);
        
        glUniform1f(glGetUniformLocation(m_shader, "prefilteredMaxLOD"), 0.0f);
    }

    glUniform1i(glGetUniformLocation(m_shader, "irradianceMap"), TextureUnits::IRRADIANCE_MAP);
    glUniform1i(glGetUniformLocation(m_shader, "prefilteredMap"), TextureUnits::PREFILTERED_ENV_MAP);
    glUniform1i(glGetUniformLocation(m_shader, "brdfLUT"), TextureUnits::BRDF_LUT);

    std::cout << "[LightingPass] Uploading matrices and camera position..." << std::endl;
    // Upload matrices
    glm::mat4 invProj = glm::inverse(ctx.proj);
    glm::mat4 invView = glm::inverse(ctx.view);
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "invProjection"), 
                       1, GL_FALSE, glm::value_ptr(invProj));
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "invView"), 
                       1, GL_FALSE, glm::value_ptr(invView));
    glUniformMatrix4fv(glGetUniformLocation(m_shader, "view"), 
                       1, GL_FALSE, glm::value_ptr(ctx.view));
    
    glm::vec3 camPos = camera->GetCameraPosition();
    glUniform3fv(glGetUniformLocation(m_shader, "viewPos"), 1, glm::value_ptr(camPos));

    // SSAO strength (from context)
    float aoStrength = ctx.enableSSAO ? ctx.ssaoIntensity : 0.0f;
    glUniform1f(glGetUniformLocation(m_shader, "aoStrength"), aoStrength);
    std::cout << "[LightingPass] SSAO strength set to: " << aoStrength << std::endl;

    // Screen-space shadow strength
    float sssStrength = ctx.enableScreenSpaceShadows ? 1.0f : 0.0f;
    glUniform1f(glGetUniformLocation(m_shader, "sssStrength"), sssStrength);
    std::cout << "[LightingPass] Screen-space shadow strength set to: " << sssStrength << std::endl;

    // NEW: SSGI uniforms
    std::cout << "[LightingPass] Binding SSGI texture: " << m_ssgiTexture << std::endl;
    glActiveTexture(GL_TEXTURE0 + TextureUnits::SSGI_MAP);
    if (m_ssgiTexture > 0 && glIsTexture(m_ssgiTexture)) {
        glBindTexture(GL_TEXTURE_2D, m_ssgiTexture);
    } else {
        std::cout << "[LightingPass] No valid SSGI texture - indirect diffuse will be disabled" << std::endl;
    }
    glUniform1i(glGetUniformLocation(m_shader, "ssgiMap"), TextureUnits::SSGI_MAP);
    glUniform1f(glGetUniformLocation(m_shader, "ssgiStrength"), ctx.enableSSGI ? ctx.ssgiStrength : 0.0f);
    std::cout << "[LightingPass] SSGI strength set to: " << (ctx.enableSSGI ? ctx.ssgiStrength : 0.0f) << std::endl;

    // NEW: Upload LPV parameters
    glUniform1i(glGetUniformLocation(m_shader, "enableLPV"), lpvEnabled ? 1 : 0);
    if (lpvEnabled) {
        std::cout << "[LightingPass] Setting LPV parameters..." << std::endl;
        
        // CRITICAL: Pass grid parameters to shader
        glUniform3fv(glGetUniformLocation(m_shader, "lpvGridCenter"), 1, glm::value_ptr(ctx.lpvGridCenter));
        glUniform1i(glGetUniformLocation(m_shader, "lpvGridResolution"), ctx.lpvGridResolution);
        glUniform1f(glGetUniformLocation(m_shader, "lpvVoxelSize"), ctx.lpvVoxelSize);
        glUniform1f(glGetUniformLocation(m_shader, "lpvGIStrength"), ctx.lpvGIStrength);
        
        // CRITICAL: Pass grid orientation as quaternion
        glm::vec4 orientQuat = glm::vec4(ctx.lpvGridOrientation.x, ctx.lpvGridOrientation.y,
                                          ctx.lpvGridOrientation.z, ctx.lpvGridOrientation.w);
        glUniform4fv(glGetUniformLocation(m_shader, "lpvGridOrientation"), 1, glm::value_ptr(orientQuat));
        
        // Debug parameters
        glUniform1i(glGetUniformLocation(m_shader, "lpvDebugVisualization"), ctx.lpvDebugVisualization ? 1 : 0);
        glUniform1f(glGetUniformLocation(m_shader, "lpvDebugBoost"), ctx.lpvDebugBoost);
        
        std::cout << "[LightingPass] LPV grid center: (" << ctx.lpvGridCenter.x << ", " 
                  << ctx.lpvGridCenter.y << ", " << ctx.lpvGridCenter.z << ")" << std::endl;
        std::cout << "[LightingPass] LPV grid resolution: " << ctx.lpvGridResolution << std::endl;
        std::cout << "[LightingPass] LPV voxel size: " << ctx.lpvVoxelSize << std::endl;
        std::cout << "[LightingPass] LPV GI strength: " << ctx.lpvGIStrength << std::endl;
        std::cout << "[LightingPass] LPV debug mode: " << (ctx.lpvDebugVisualization ? "ON" : "OFF") 
                  << " (boost: " << ctx.lpvDebugBoost << "x)" << std::endl;
        
        // Verify texture binding
        std::cout << "[LightingPass] LPV textures bound: R=" << m_lpvTextureR 
                  << " G=" << m_lpvTextureG << " B=" << m_lpvTextureB << std::endl;
    } else {
        std::cout << "[LightingPass] LPV disabled" << std::endl;
    }

    // Bind LightManager data
    if (ctx.lightManager) {
        ctx.lightManager->UpdateGPUBuffers();
        
        int activeLightCount = ctx.lightManager->GetActiveLightCount();
        std::cout << "[LightingPass] Binding light data - Active lights: " << activeLightCount << std::endl;
        
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx.lightManager->GetLightDataSSBO());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ctx.lightManager->GetShadowMatricesSSBO());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ctx.lightManager->GetTileDataSSBO());

        glUniform1i(glGetUniformLocation(m_shader, "numLights"), activeLightCount);
        glUniform1i(glGetUniformLocation(m_shader, "numDirectionalLights"), 
                    static_cast<int>(ctx.lightManager->GetDirectionalLightCount()));
        glUniform1i(glGetUniformLocation(m_shader, "numPointLights"), 
                    static_cast<int>(ctx.lightManager->GetPointLightCount()));
        glUniform1i(glGetUniformLocation(m_shader, "numSpotLights"), 
                    static_cast<int>(ctx.lightManager->GetSpotLightCount()));

        glUniform1i(glGetUniformLocation(m_shader, "enableShadows"), ctx.enableShadows ? 1 : 0);

        // Bind shadow array
        GLuint shadowArray = ctx.lightManager->GetShadowArrayTexture();
        std::cout << "[LightingPass] Shadow array texture: " << shadowArray << std::endl;
        if (shadowArray > 0 && glIsTexture(shadowArray)) {
            glActiveTexture(GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY);
            glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArray);
            glUniform1i(glGetUniformLocation(m_shader, "multiLightShadowArray"), 
                       TextureUnits::SHADOW_MAP_ARRAY);
        } else {
            std::cerr << "[LightingPass] WARNING: Invalid shadow array texture!" << std::endl;
        }

        // Shadow bias configuration
        glUniform1f(glGetUniformLocation(m_shader, "shadowBias"), ctx.shadowBias);
        glUniform1f(glGetUniformLocation(m_shader, "maxShadowBias"), ctx.shadowBias * 10.0f);
        glUniform1f(glGetUniformLocation(m_shader, "normalOffsetScale"), 0.1f);
        glUniform1f(glGetUniformLocation(m_shader, "cascadeBiasScale"), 1.0f);

        // Disable legacy cascade system
        glUniform1i(glGetUniformLocation(m_shader, "cascadeCount"), 0);
    } else {
        std::cout << "[LightingPass] No LightManager - setting zero lights" << std::endl;
        // No lights
        glUniform1i(glGetUniformLocation(m_shader, "numLights"), 0);
        glUniform1i(glGetUniformLocation(m_shader, "numDirectionalLights"), 0);
        glUniform1i(glGetUniformLocation(m_shader, "numPointLights"), 0);
        glUniform1i(glGetUniformLocation(m_shader, "numSpotLights"), 0);
        glUniform1i(glGetUniformLocation(m_shader, "enableShadows"), 0);
        glUniform1i(glGetUniformLocation(m_shader, "cascadeCount"), 0);
    }

    std::cout << "[LightingPass] Rendering fullscreen quad..." << std::endl;
    // Render fullscreen quad
    if (ctx.screenQuad) {
        ctx.screenQuad->Render();
    } else {
        std::cerr << "[LightingPass] ERROR: ScreenQuad is null!" << std::endl;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::cout << "[LightingPass] Execution complete" << std::endl;
}
