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

SSGIPass::~SSGIPass() {
	// TexturePtr handles cleanup automatically - no need for manual deletion
	// Only cleanup Kawase shader and FBO
	if (m_kawaseFBO) { glDeleteFramebuffers(1, &m_kawaseFBO); m_kawaseFBO = 0; }
	if (m_kawaseShader) { glDeleteProgram(m_kawaseShader); m_kawaseShader = 0; }
}

bool SSGIPass::Initialize(RenderContext& ctx) {
	std::cout << "[SSGIPass] Initializing Screen Space Global Illumination..." << std::endl;

	// Create compute shaders for each stage
	m_csDirections = std::make_unique<ComputeShader>();
	m_csRaymarch = std::make_unique<ComputeShader>();
	m_csDownsample = std::make_unique<ComputeShader>();
	m_csBilateral = std::make_unique<ComputeShader>();
	m_csUpsample = std::make_unique<ComputeShader>();
	m_csTemporal = std::make_unique<ComputeShader>();

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

	// Allocate render targets using new Texture system
	Resize(ctx, ctx.width, ctx.height);

	std::cout << "[SSGIPass] Initialization complete" << std::endl;
	return true;
}

void SSGIPass::Resize(RenderContext& ctx, int w, int h) {
	if (w == m_w && h == m_h) {
		return;
	}

	m_w = w;
	m_h = h;
	m_halfRes = ctx.ssgiHalfRes;
	m_hw = m_halfRes ? std::max(1, w / 2) : w;
	m_hh = m_halfRes ? std::max(1, h / 2) : h;
	m_qw = std::max(1, m_hw / 2);
	m_qh = std::max(1, m_hh / 2);

	std::cout << "[SSGIPass] Resizing to " << w << "x" << h
		<< " (working resolution: " << m_hw << "x" << m_hh << ", quarter: " << m_qw << "x" << m_qh << ")" << std::endl;

	// REFACTORED: Use Texture::Builder for clean, declarative texture creation
	// All textures use RGBA16F for compute shader image2D compatibility (except directions which use RG16F)
	m_dirTex = Texture::Builder::Texture2D(m_hw, m_hh, GL_RG16F)
		.Format(GL_RG).DataType(GL_HALF_FLOAT)
		.FilterMode(GL_NEAREST, GL_NEAREST)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	m_ssgiRaw = TextureFactory::CreateHDR(m_hw, m_hh);  // Uses builder internally
	m_ssgiQuarter = TextureFactory::CreateHDR(m_qw, m_qh);
	m_ssgiQuarterBlur = TextureFactory::CreateHDR(m_qw, m_qh);
	m_ssgiBlur = TextureFactory::CreateHDR(m_hw, m_hh);
	m_ssgiTex = TextureFactory::CreateHDR(m_hw, m_hh);

	// History textures for temporal accumulation - initialize to black for first frame
	m_historyColor = TextureFactory::CreateHDR(m_hw, m_hh);
	m_historySSGI = TextureFactory::CreateHDR(m_hw, m_hh);
	
	// CRITICAL: Initialize history buffers to zero on first frame/resize
	// This prevents artifacts from uninitialized memory
	std::vector<float> blackPixels(m_hw * m_hh * 4, 0.0f);
	m_historyColor->Upload2D(0, 0, 0, m_hw, m_hh, GL_RGBA, GL_FLOAT, blackPixels.data());
	m_historySSGI->Upload2D(0, 0, 0, m_hw, m_hh, GL_RGBA, GL_FLOAT, blackPixels.data());

	// Kawase ping/pong targets at working resolution
	m_kawasePing = TextureFactory::CreateHDR(m_hw, m_hh);
	m_kawasePong = TextureFactory::CreateHDR(m_hw, m_hh);

	std::cout << "[SSGIPass] All textures allocated successfully using new Texture system" << std::endl;
	std::cout << "[SSGIPass] History buffers initialized to zero for first frame convergence" << std::endl;
}

void SSGIPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&) {
	if (!ctx.enableSSGI) return;
	if (!ctx.gbufferFBO || !camera) return;

	// DIAGNOSTIC: Log temporal accumulation parameters once per second
	static int frameCounter = 0;
	if (frameCounter++ % 60 == 0) {
		std::cout << "[SSGIPass] Temporal params - alpha: " << ctx.ssgiTemporalAlpha 
			<< ", depth threshold: " << ctx.taaDepthThreshold 
			<< ", normal threshold: " << ctx.taaNormalThreshold << std::endl;
	}

	// Run all SSGI stages
	runDirections(ctx);
	runRaymarch(ctx, camera);
	runDownsample(ctx);
	runBilateral(ctx); // quarter-res blur
	runUpsample(ctx); // upsample to half with edge-aware combine
	runTemporal(ctx);  // temporal accumulation (history updated here)
	runKawase(ctx);    // final smoothing (no history update)
}

void SSGIPass::runDirections(RenderContext& ctx) {
	// Stage1: Generate stochastic cosine-weighted directions per pixel
	glUseProgram(m_csDirections->GetProgramID());

	// Bind output texture using new system
	glBindImageTexture(0, m_dirTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);

	// Set uniforms
	glUniform2f(glGetUniformLocation(m_csDirections->GetProgramID(), "invScreen"),
		1.0f / float(m_hw), 1.0f / float(m_hh));
	glUniform1i(glGetUniformLocation(m_csDirections->GetProgramID(), "frameIndex"), m_frameIndex++);

	// Dispatch compute shader (8x8 local work groups)
	GLuint gx = (m_hw + 7) / 8;
	GLuint gy = (m_hh + 7) / 8;
	glDispatchCompute(gx, gy, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	glUseProgram(0);
}

void SSGIPass::runRaymarch(RenderContext& ctx, const std::shared_ptr<Camera>& camera) {
	// Stage2: Screen-space ray marching to find indirect lighting
	glUseProgram(m_csRaymarch->GetProgramID());

	// Bind input textures
	glBindTextureUnit(0, ctx.gbufferFBO->GetDepthTexture()); // Depth buffer
	glBindTextureUnit(1, ctx.gbufferFBO->GetColorAttachment(0)); // Normal buffer
	glBindTextureUnit(2, ctx.gbufferFBO->GetColorAttachment(2)); // Albedo buffer
	glBindTextureUnit(3, m_dirTex->ID()); // Stochastic directions
	glBindTextureUnit(4, m_historyColor->ID()); // Previous frame color

	// Bind output texture
	glBindImageTexture(5, m_ssgiRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	// Set uniforms for ray marching
	glm::mat4 invProj = glm::inverse(ctx.proj);
	glm::mat4 invView = glm::inverse(ctx.view);

	GLint loc = glGetUniformLocation(m_csRaymarch->GetProgramID(), "invProj");
	glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(invProj));
	loc = glGetUniformLocation(m_csRaymarch->GetProgramID(), "invView");
	glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(invView));
	loc = glGetUniformLocation(m_csRaymarch->GetProgramID(), "view");
	glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(ctx.view));
	loc = glGetUniformLocation(m_csRaymarch->GetProgramID(), "proj");
	glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(ctx.proj));

	glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "maxRayLenVS"), ctx.ssgiRadius);
	glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "numSteps"), std::max(8, ctx.ssgiSampleCount));
	glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "thickness"), ctx.ssgiThickness);
	glUniform2f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "screenSize"), float(m_w), float(m_h));
	glUniform2f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "workSize"), float(m_hw), float(m_hh));
	glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "cameraNear"), camera->GetCameraNearPlane());
	glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "cameraFar"), camera->GetCameraFarPlane());

	// Dispatch
	GLuint gx = (m_hw + 7) / 8;
	GLuint gy = (m_hh + 7) / 8;
	glDispatchCompute(gx, gy, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	glUseProgram(0);
}

void SSGIPass::runDownsample(RenderContext& ctx) {
	// Stage2.5: Downsample half -> quarter
	glUseProgram(m_csDownsample->GetProgramID());

	glBindTextureUnit(0, m_ssgiRaw->ID());
	glBindImageTexture(1, m_ssgiQuarter->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	glUniform2f(glGetUniformLocation(m_csDownsample->GetProgramID(), "invSrc"), 1.0f / float(m_hw), 1.0f / float(m_hh));

	GLuint gx = (m_qw + 7) / 8;
	GLuint gy = (m_qh + 7) / 8;
	glDispatchCompute(gx, gy, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	glUseProgram(0);
}

void SSGIPass::runBilateral(RenderContext& ctx) {
	// Stage3: Edge-aware bilateral blur for denoising (quarter-res)
	glUseProgram(m_csBilateral->GetProgramID());

	// Bind input textures
	glBindTextureUnit(0, m_ssgiQuarter->ID());
	glBindTextureUnit(1, ctx.gbufferFBO->GetDepthTexture());
	glBindTextureUnit(2, ctx.gbufferFBO->GetColorAttachment(0));

	// Bind output texture
	glBindImageTexture(3, m_ssgiQuarterBlur->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	// Set bilateral filter parameters
	glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "depthSigma"), ctx.ssgiDepthReject);
	glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "normalThresh"), ctx.ssgiNormalReject);
	glUniform2f(glGetUniformLocation(m_csBilateral->GetProgramID(), "invWork"), 1.0f / float(m_qw), 1.0f / float(m_qh));

	// Dispatch
	GLuint gx = (m_qw + 7) / 8;
	GLuint gy = (m_qh + 7) / 8;
	glDispatchCompute(gx, gy, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	glUseProgram(0);
}

void SSGIPass::runUpsample(RenderContext& ctx) {
	// Stage3.5: Bilateral upsample quarter->half and combine with raw half
	glUseProgram(m_csUpsample->GetProgramID());

	glBindTextureUnit(0, m_ssgiQuarterBlur->ID());
	glBindTextureUnit(1, m_ssgiRaw->ID());
	glBindTextureUnit(2, ctx.gbufferFBO->GetDepthTexture());
	glBindTextureUnit(3, ctx.gbufferFBO->GetColorAttachment(0));

	glBindImageTexture(4, m_ssgiBlur->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	glUniform2f(glGetUniformLocation(m_csUpsample->GetProgramID(), "invDst"), 1.0f / float(m_hw), 1.0f / float(m_hh));
	glUniform2f(glGetUniformLocation(m_csUpsample->GetProgramID(), "invFull"), 1.0f / float(m_w), 1.0f / float(m_h));
	glUniform1f(glGetUniformLocation(m_csUpsample->GetProgramID(), "depthSigma"), ctx.ssgiDepthReject);
	glUniform1f(glGetUniformLocation(m_csUpsample->GetProgramID(), "normalThresh"), ctx.ssgiNormalReject);

	GLuint gx = (m_hw + 7) / 8;
	GLuint gy = (m_hh + 7) / 8;
	glDispatchCompute(gx, gy, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	glUseProgram(0);
}

void SSGIPass::runTemporal(RenderContext& ctx) {
	// Stage4: Temporal accumulation for stability
	glUseProgram(m_csTemporal->GetProgramID());

	// Bind input textures
	glBindTextureUnit(0, m_ssgiBlur->ID());
	glBindTextureUnit(1, m_historySSGI->ID());
	glBindTextureUnit(2, ctx.velocityTex);
	glBindTextureUnit(3, ctx.gbufferFBO->GetDepthTexture());
	glBindTextureUnit(4, ctx.gbufferFBO->GetColorAttachment(0));

	// Bind output texture
	glBindImageTexture(5, m_ssgiTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	// Set temporal parameters
	glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "alpha"), ctx.ssgiTemporalAlpha);
	glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "depthThreshold"), ctx.taaDepthThreshold);
	glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "normalThreshold"), ctx.taaNormalThreshold);

	// Dispatch
	GLuint gx = (m_hw + 7) / 8;
	GLuint gy = (m_hh + 7) / 8;
	glDispatchCompute(gx, gy, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	// CRITICAL FIX: Copy AFTER temporal resolve completes, not before
	// This ensures history contains the converged result for next frame
	glCopyImageSubData(m_ssgiTex->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
		m_historySSGI->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
		m_hw, m_hh, 1);

	glUseProgram(0);
}

void SSGIPass::runKawase(RenderContext& ctx) {
	if (!m_kawaseShader || !ctx.screenQuad) return;
	if (!m_kawaseFBO) return;

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glViewport(0, 0, m_hw, m_hh);

	GLuint src = m_ssgiTex->ID();
	for (int i = 0; i < m_kawasePasses; ++i) {
		TexturePtr& dst = (i % 2 == 0) ? m_kawasePing : m_kawasePong;
		glBindFramebuffer(GL_FRAMEBUFFER, m_kawaseFBO);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->ID(), 0);
		GLenum drawBuf = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, &drawBuf);

		glUseProgram(m_kawaseShader);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src);
		glUniform1i(glGetUniformLocation(m_kawaseShader, "image"), 0);
		glUniform2f(glGetUniformLocation(m_kawaseShader, "texelSize"), 1.0f / float(m_hw), 1.0f / float(m_hh));
		glUniform1i(glGetUniformLocation(m_kawaseShader, "pass"), i);

		ctx.screenQuad->Render();

		// Next pass reads from previous output
		src = dst->ID();
	}

	// Copy last output back into m_ssgiTex
	TexturePtr& last = ((m_kawasePasses - 1) % 2 == 0) ? m_kawasePing : m_kawasePong;
	glCopyImageSubData(last->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
		m_ssgiTex->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
		m_hw, m_hh, 1);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glUseProgram(0);
}

void SSGIPass::CaptureHistory(RenderContext& ctx) {
	// Capture current frame's HDR color for next frame's ray marching
	FrameBuffer* sourceFBO = (ctx.taaFBO && ctx.enableTAA) ? ctx.taaFBO : ctx.hdrFBO.get();
	if (!sourceFBO) return;

	// Create temporary FBO for blit target
	GLuint tmpFBO = 0;
	glGenFramebuffers(1, &tmpFBO);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFBO->GetFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmpFBO);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, m_historyColor->ID(), 0);

	glBlitFramebuffer(0, 0, ctx.width, ctx.height,
		0, 0, m_hw, m_hh,
		GL_COLOR_BUFFER_BIT, GL_LINEAR);

	// Cleanup
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &tmpFBO);
}
