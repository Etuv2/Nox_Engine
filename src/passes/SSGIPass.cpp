#include "SSGIPass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../ComputeShader.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

// Helper to create floating-point2D texture
static void createFloatTex2D(GLuint& tex, int w, int h, GLenum internal = GL_RGBA16F) {
    if (tex) glDeleteTextures(1, &tex);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D,0, internal, w, h,0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D,0);
}

// Helper to create RG texture for direction storage
static void createRGTex2D(GLuint& tex, int w, int h, GLenum internal = GL_RG16F) {
    if (tex) glDeleteTextures(1, &tex);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D,0, internal, w, h,0, GL_RG, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D,0);
}

SSGIPass::~SSGIPass() { 
    freeTargets(); 
    // Delete Kawase resources
    if (m_kawaseFBO) { glDeleteFramebuffers(1, &m_kawaseFBO); m_kawaseFBO =0; }
    if (m_kawaseShader) { glDeleteProgram(m_kawaseShader); m_kawaseShader =0; }
}

bool SSGIPass::Initialize(RenderContext& ctx) {
    std::cout << "[SSGIPass] Initializing Screen Space Global Illumination..." << std::endl;

    // Create compute shaders for each stage
    m_csDirections = std::make_unique<ComputeShader>();
    m_csRaymarch   = std::make_unique<ComputeShader>();
    m_csDownsample = std::make_unique<ComputeShader>();
    m_csBilateral  = std::make_unique<ComputeShader>();
    m_csUpsample   = std::make_unique<ComputeShader>();
    m_csTemporal   = std::make_unique<ComputeShader>();

    // Load shaders from files
    if (!m_csDirections->CreateFromFile("shaders/ssgi_directions_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_directions_comp.glsl" << std::endl;
        return false;
    }
    
    if (!m_csRaymarch->CreateFromFile("shaders/ssgi_raymarch_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_raymarch_comp.glsl" << std::endl;
        return false;
    }

    if (!m_csDownsample->CreateFromFile("shaders/ssgi_downsample2x_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_downsample2x_comp.glsl" << std::endl;
        return false;
    }
    
    if (!m_csBilateral->CreateFromFile("shaders/ssgi_bilateral_blur_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_bilateral_blur_comp.glsl" << std::endl;
        return false;
    }
    
    if (!m_csUpsample->CreateFromFile("shaders/ssgi_upsample2x_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_upsample2x_comp.glsl" << std::endl;
        return false;
    }
    
    if (!m_csTemporal->CreateFromFile("shaders/ssgi_temporal_resolve_comp.glsl")) {
        std::cerr << "[SSGIPass] Failed to compile ssgi_temporal_resolve_comp.glsl" << std::endl;
        return false;
    }

    // Kawase blur fullscreen shader
    m_kawaseShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", "shaders/kawase_blur.glsl");
    if (!m_kawaseShader) {
        std::cerr << "[SSGIPass] Failed to create Kawase blur shader." << std::endl;
        // Not fatal; allow running without Kawase
    }
    glGenFramebuffers(1, &m_kawaseFBO);

    std::cout << "[SSGIPass] All compute shaders compiled successfully" << std::endl;

    // Allocate render targets
    Resize(ctx, ctx.width, ctx.height);
    
    std::cout << "[SSGIPass] Initialization complete" << std::endl;
    return true;
}

void SSGIPass::Resize(RenderContext& ctx, int w, int h) {
    m_w = w; 
    m_h = h;
    m_halfRes = ctx.ssgiHalfRes;
    m_hw = m_halfRes ? std::max(1, w /2) : w;
    m_hh = m_halfRes ? std::max(1, h /2) : h;
    m_qw = std::max(1, m_hw /2);
    m_qh = std::max(1, m_hh /2);
    
    std::cout << "[SSGIPass] Resizing to " << w << "x" << h 
              << " (working resolution: " << m_hw << "x" << m_hh << ", quarter: " << m_qw << "x" << m_qh << ")" << std::endl;
    
    allocTargets(m_hw, m_hh, m_halfRes);
}

void SSGIPass::allocTargets(int w, int h, bool halfRes) {
    freeTargets();
    
    // Create all render targets with RGBA16F (image2D requires RGBA, not RGB)
    createRGTex2D(m_dirTex, w, h, GL_RG16F); // Stochastic directions (RG is fine)
    createFloatTex2D(m_ssgiRaw, w, h, GL_RGBA16F); // Raw raymarch output
    createFloatTex2D(m_ssgiQuarter, std::max(1, w/2), std::max(1, h/2), GL_RGBA16F); // quarter downsample
    createFloatTex2D(m_ssgiQuarterBlur, std::max(1, w/2), std::max(1, h/2), GL_RGBA16F); // quarter blurred
    createFloatTex2D(m_ssgiBlur, w, h, GL_RGBA16F); // Denoised/upsampled output
    createFloatTex2D(m_ssgiTex, w, h, GL_RGBA16F); // Final resolved output
    
    // History textures for temporal accumulation
    createFloatTex2D(m_historyColor, w, h, GL_RGBA16F); // Previous frame color (HDR pre-tonemap)
    createFloatTex2D(m_historySSGI, w, h, GL_RGBA16F); // Previous frame SSGI

    // Kawase ping/pong targets at working resolution
    createFloatTex2D(m_kawasePing, w, h, GL_RGBA16F);
    createFloatTex2D(m_kawasePong, w, h, GL_RGBA16F);
}

void SSGIPass::freeTargets() {
    GLuint arr[] = { m_dirTex, m_ssgiRaw, m_ssgiQuarter, m_ssgiQuarterBlur, m_ssgiBlur, m_ssgiTex, m_historyColor, m_historySSGI, m_kawasePing, m_kawasePong };
    for (GLuint& t : arr) {
        if (t) { 
            glDeleteTextures(1, &t); 
            t =0; 
        }
    }
}

void SSGIPass::Execute(RenderContext& ctx,
                       const std::shared_ptr<SceneGraph>&,
                       const std::shared_ptr<Camera>& camera,
                       const std::shared_ptr<DirectionalLight>&,
                       const std::shared_ptr<Skybox>&) {
    if (!ctx.enableSSGI) return;
    if (!ctx.gbufferFBO || !camera) return;

    // Run all SSGI stages
    runDirections(ctx);
    runRaymarch(ctx, camera);
    runDownsample(ctx);
    runBilateral(ctx); // quarter-res blur
    runUpsample(ctx); // upsample to half with edge-aware combine
    runTemporal(ctx);
    runKawase(ctx); // final smoothing
}

void SSGIPass::runDirections(RenderContext& ctx) {
    // Stage1: Generate stochastic cosine-weighted directions per pixel
    glUseProgram(m_csDirections->GetProgramID());
    
    // Bind output texture
    glBindImageTexture(0, m_dirTex,0, GL_FALSE,0, GL_WRITE_ONLY, GL_RG16F);
    
    // Set uniforms
    glUniform2f(glGetUniformLocation(m_csDirections->GetProgramID(), "invScreen"),
1.0f / float(m_hw),1.0f / float(m_hh));
 glUniform1i(glGetUniformLocation(m_csDirections->GetProgramID(), "frameIndex"), m_frameIndex++);
    
    // Dispatch compute shader (8x8 local work groups)
    GLuint gx = (m_hw +7) /8;
    GLuint gy = (m_hh +7) /8;
    glDispatchCompute(gx, gy,1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    
    glUseProgram(0);
}

void SSGIPass::runRaymarch(RenderContext& ctx, const std::shared_ptr<Camera>& camera) {
    // Stage2: Screen-space ray marching to find indirect lighting
    glUseProgram(m_csRaymarch->GetProgramID());
    
    // Bind input textures
    glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture()); // Depth buffer
 glBindTextureUnit(1, ctx.gbufferFBO->GetColorAttachment(0)); // Normal buffer (oct-encoded)
 glBindTextureUnit(2, ctx.gbufferFBO->GetColorAttachment(2)); // Albedo buffer
 glBindTextureUnit(3, m_dirTex); // Stochastic directions
 glBindTextureUnit(4, m_historyColor); // Previous frame color
    
    // Bind output texture with RGBA16F format
    glBindImageTexture(5, m_ssgiRaw,0, GL_FALSE,0, GL_WRITE_ONLY, GL_RGBA16F);

    // Set uniforms for ray marching
    glm::mat4 invProj = glm::inverse(ctx.proj);
    glm::mat4 invView = glm::inverse(ctx.view);
    
    glUniformMatrix4fv(glGetUniformLocation(m_csRaymarch->GetProgramID(), "invProj"), 
1, GL_FALSE, glm::value_ptr(invProj));
 glUniformMatrix4fv(glGetUniformLocation(m_csRaymarch->GetProgramID(), "invView"), 
1, GL_FALSE, glm::value_ptr(invView));
 glUniformMatrix4fv(glGetUniformLocation(m_csRaymarch->GetProgramID(), "view"), 
1, GL_FALSE, glm::value_ptr(ctx.view));
 glUniformMatrix4fv(glGetUniformLocation(m_csRaymarch->GetProgramID(), "proj"), 
1, GL_FALSE, glm::value_ptr(ctx.proj));
 glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "maxRayLenVS"), 
 ctx.ssgiRadius);
 glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "numSteps"), 
 std::max(8, ctx.ssgiSampleCount));
 glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "thickness"),
 ctx.ssgiThickness);
 glUniform2f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "screenSize"), 
 float(m_w), float(m_h));
 glUniform2f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "workSize"), 
 float(m_hw), float(m_hh));

 // Camera near/far
 glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "cameraNear"), camera->GetCameraNearPlane());
 glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "cameraFar"), camera->GetCameraFarPlane());

 // Dispatch
 GLuint gx = (m_hw +7) /8;
 GLuint gy = (m_hh +7) /8;
 glDispatchCompute(gx, gy,1);
 glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    
    glUseProgram(0);
}

void SSGIPass::runDownsample(RenderContext& ctx) {
    // Stage2.5: Downsample half -> quarter
    glUseProgram(m_csDownsample->GetProgramID());

 glBindTextureUnit(0, m_ssgiRaw);
 glBindImageTexture(1, m_ssgiQuarter,0, GL_FALSE,0, GL_WRITE_ONLY, GL_RGBA16F);

 glUniform2f(glGetUniformLocation(m_csDownsample->GetProgramID(), "invSrc"),1.0f / float(m_hw),1.0f / float(m_hh));

 GLuint gx = (m_qw +7) /8;
 GLuint gy = (m_qh +7) /8;
 glDispatchCompute(gx, gy,1);
 glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

 glUseProgram(0);
}

void SSGIPass::runBilateral(RenderContext& ctx) {
    // Stage3: Edge-aware bilateral blur for denoising (quarter-res)
    glUseProgram(m_csBilateral->GetProgramID());
    
    // Bind input textures
    glBindTextureUnit(0, m_ssgiQuarter); // Quarter SSGI
 glBindTextureUnit(1, ctx.gbufferFBO->GetDepthTexture()); // Full-res depth for guidance
 glBindTextureUnit(2, ctx.gbufferFBO->GetColorAttachment(0)); // Full-res normals for guidance
    
    // Bind output texture with RGBA16F format (quarter-res output)
    glBindImageTexture(3, m_ssgiQuarterBlur,0, GL_FALSE,0, GL_WRITE_ONLY, GL_RGBA16F);

 // Set bilateral filter parameters
 glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "depthSigma"), 
 ctx.ssgiDepthReject);
 glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "normalThresh"), 
 ctx.ssgiNormalReject);
 glUniform2f(glGetUniformLocation(m_csBilateral->GetProgramID(), "invWork"), 
1.0f / float(m_qw),1.0f / float(m_qh));

 // Dispatch
 GLuint gx = (m_qw +7) /8;
 GLuint gy = (m_qh +7) /8;
 glDispatchCompute(gx, gy,1);
 glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    
    glUseProgram(0);
}

void SSGIPass::runUpsample(RenderContext& ctx) {
    // Stage3.5: Bilateral upsample quarter->half and combine with raw half
    glUseProgram(m_csUpsample->GetProgramID());

 glBindTextureUnit(0, m_ssgiQuarterBlur);
 glBindTextureUnit(1, m_ssgiRaw);
 glBindTextureUnit(2, ctx.gbufferFBO->GetDepthTexture());
 glBindTextureUnit(3, ctx.gbufferFBO->GetColorAttachment(0));

 glBindImageTexture(4, m_ssgiBlur,0, GL_FALSE,0, GL_WRITE_ONLY, GL_RGBA16F);

 glUniform2f(glGetUniformLocation(m_csUpsample->GetProgramID(), "invDst"),1.0f / float(m_hw),1.0f / float(m_hh));
 glUniform2f(glGetUniformLocation(m_csUpsample->GetProgramID(), "invFull"),1.0f / float(m_w),1.0f / float(m_h));
 glUniform1f(glGetUniformLocation(m_csUpsample->GetProgramID(), "depthSigma"), ctx.ssgiDepthReject);
 glUniform1f(glGetUniformLocation(m_csUpsample->GetProgramID(), "normalThresh"), ctx.ssgiNormalReject);

 GLuint gx = (m_hw +7) /8;
 GLuint gy = (m_hh +7) /8;
 glDispatchCompute(gx, gy,1);
 glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

 glUseProgram(0);
}

void SSGIPass::runTemporal(RenderContext& ctx) {
    // Stage4: Temporal accumulation for stability (with velocity reprojection)
    glUseProgram(m_csTemporal->GetProgramID());
 
    // Bind input textures
    glBindTextureUnit(0, m_ssgiBlur); // Current frame (denoised + upsampled)
 glBindTextureUnit(1, m_historySSGI); // Previous frame SSGI
 glBindTextureUnit(2, ctx.velocityTex); // Motion vectors from TAA pass (UV-space)
 glBindTextureUnit(3, ctx.gbufferFBO->GetDepthTexture()); // Depth for rejection
 glBindTextureUnit(4, ctx.gbufferFBO->GetColorAttachment(0)); // Normals for rejection
 
    // Bind output texture with RGBA16F format
    glBindImageTexture(5, m_ssgiTex,0, GL_FALSE,0, GL_WRITE_ONLY, GL_RGBA16F);
 
    // Set temporal parameters using TAA thresholds for consistency
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "alpha"), ctx.ssgiTemporalAlpha);
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "depthThreshold"), ctx.taaDepthThreshold);
    glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "normalThreshold"), ctx.taaNormalThreshold);

    // Dispatch
    GLuint gx = (m_hw +7) /8;
    GLuint gy = (m_hh +7) /8;
 glDispatchCompute(gx, gy,1);
 glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // Copy current SSGI to history for next frame
    glCopyImageSubData(m_ssgiTex, GL_TEXTURE_2D,0,0,0,0, 
 m_historySSGI, GL_TEXTURE_2D,0,0,0,0, 
 m_hw, m_hh,1);
 
    glUseProgram(0);
}

void SSGIPass::runKawase(RenderContext& ctx) {
    if (!m_kawaseShader || !ctx.screenQuad) return;
    if (!m_kawaseFBO) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0,0, m_hw, m_hh);

    GLuint src = m_ssgiTex;
    for (int i =0; i < m_kawasePasses; ++i) {
        GLuint dst = (i %2 ==0) ? m_kawasePing : m_kawasePong;
        glBindFramebuffer(GL_FRAMEBUFFER, m_kawaseFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst,0);
        GLenum drawBuf = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuf);

        glUseProgram(m_kawaseShader);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        GLint locImg = glGetUniformLocation(m_kawaseShader, "image");
        glUniform1i(locImg,0);
        GLint locTexel = glGetUniformLocation(m_kawaseShader, "texelSize");
        glUniform2f(locTexel,1.0f / float(m_hw),1.0f / float(m_hh));
        GLint locPass = glGetUniformLocation(m_kawaseShader, "pass");
        glUniform1i(locPass, i);

        ctx.screenQuad->Render();

        // Next pass reads from previous output
        src = dst;
    }

    // Copy last output back into m_ssgiTex so downstream uses blurred
    GLuint last = ( (m_kawasePasses -1) %2 ==0 ) ? m_kawasePing : m_kawasePong;
    glCopyImageSubData(last, GL_TEXTURE_2D,0,0,0,0,
 m_ssgiTex, GL_TEXTURE_2D,0,0,0,0,
 m_hw, m_hh,1);

 // Update history with final smoothed result for better stability next frame
 glCopyImageSubData(m_ssgiTex, GL_TEXTURE_2D,0,0,0,0,
 m_historySSGI, GL_TEXTURE_2D,0,0,0,0,
 m_hw, m_hh,1);

 glBindFramebuffer(GL_FRAMEBUFFER,0);
 glUseProgram(0);
}

void SSGIPass::CaptureHistory(RenderContext& ctx) {
    // Capture current frame's HDR color (pre-tonemap) as history for next frame's ray marching
    // Prefer TAA-stabilized output if available, otherwise fall back to raw HDR
    FrameBuffer* sourceFBO = (ctx.taaFBO && ctx.enableTAA) ? ctx.taaFBO : ctx.hdrFBO.get();
    if (!sourceFBO) return;

    // Use glCopyImageSubData if sizes match; otherwise use blit
    // Here we blit from source to our history texture, scaling as needed when half-res
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFBO->GetFBO());

  // Create temporary FBO for blit target
    GLuint tmpFBO =0;
    glGenFramebuffers(1, &tmpFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmpFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 
           GL_TEXTURE_2D, m_historyColor,0);

    glBlitFramebuffer(0,0, ctx.width, ctx.height, 
    0,0, m_hw, m_hh, 
 GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDeleteFramebuffers(1, &tmpFBO);
}
