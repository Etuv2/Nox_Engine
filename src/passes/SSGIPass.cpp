#include "SSGIPass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../ComputeShader.h"
#include "../Skybox.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

SSGIPass::~SSGIPass() {
	// Only cleanup Kawase shader and FBO
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
	m_csFinalUpsample = std::make_unique<ComputeShader>();

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

	//Load final upsample shader
	if (!m_csFinalUpsample->CreateFromFile("shaders/ssgi_final_upsample_comp.glsl")) {
		std::cerr << "[SSGIPass] Failed to compile ssgi_final_upsample_comp.glsl" << std::endl;
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

	//Using the Texture::Builder for clean, declarative texture creation,lets me set all params in one place
	// All textures use RGBA16F for compute shader image2D compatibility (except directions which use RG16F)
	m_dirTex = Texture::Builder::Texture2D(m_hw, m_hh, GL_RGBA16F)
		.Format(GL_RGBA).DataType(GL_HALF_FLOAT)
		.FilterMode(GL_NEAREST, GL_NEAREST)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	m_ssgiRaw = TextureFactory::CreateHDR(m_hw, m_hh); // Uses builder internally
	m_ssgiQuarter = TextureFactory::CreateHDR(m_qw, m_qh);
	m_ssgiQuarterBlur = TextureFactory::CreateHDR(m_qw, m_qh);
	m_ssgiBlur = TextureFactory::CreateHDR(m_hw, m_hh);

	//Create working-res and FULL-RES textures separately
	m_ssgiWork = TextureFactory::CreateHDR(m_hw, m_hh);  // Working-res (for temporal)
	m_ssgiTex = TextureFactory::CreateHDR(m_w, m_h);      // FULL-RES (for lighting pass)

	// History textures for temporal accumulation - initialize to black for first frame
	m_historyColor = TextureFactory::CreateHDR(m_hw, m_hh);
	m_historySSGI = TextureFactory::CreateHDR(m_hw, m_hh);

	//Initialize history buffers to zero on first frame/resize
	// This prevents artifacts from uninitialized memory
	std::vector<float> blackPixels(m_hw * m_hh * 4, 0.0f);
	m_historyColor->Upload2D(0, 0, 0, m_hw, m_hh, GL_RGBA, GL_FLOAT, blackPixels.data());
	m_historySSGI->Upload2D(0, 0, 0, m_hw, m_hh, GL_RGBA, GL_FLOAT, blackPixels.data());

	// Kawase ping/pong targets at working resolution
	m_kawasePing = TextureFactory::CreateHDR(m_hw, m_hh);
	m_kawasePong = TextureFactory::CreateHDR(m_hw, m_hh);
}

void SSGIPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>& skybox) {
	if (!ctx.enableSSGI) return;
	if (!ctx.gbufferFBO || !camera) return;
	// Run all SSGI stages
	runDirections(ctx);
	runRaymarch(ctx, camera, skybox);
	runDownsample(ctx);
	runBilateral(ctx); // quarter-res blur
	runUpsample(ctx); // upsample to working-res with edge-aware combine
	runTemporal(ctx); // temporal accumulation (history updated here, output to m_ssgiWork)
	runKawase(ctx); // optional final smoothing at working-res
	runFinalUpsample(ctx); //Upsample from working-res to FULL-RES for lighting pass
}

void SSGIPass::runDirections(RenderContext& ctx) {
	// Stage1: Generate stochastic cosine-weighted directions per pixel
	glUseProgram(m_csDirections->GetProgramID());

	// Bind output texture using new system
	glBindImageTexture(0, m_dirTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

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

void SSGIPass::runRaymarch(RenderContext& ctx, const std::shared_ptr<Camera>& camera, const std::shared_ptr<Skybox>& skybox) {
	// Stage2: Screen-space ray marching to find indirect lighting
	glUseProgram(m_csRaymarch->GetProgramID());

	//Bind ALL required textures for raymarch shader
	// Bind input textures - OPTIMIZED G-buffer layout (3 RTs)
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "gDepth"), 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0)); // RT0: Packed normal+rough+metal
	glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "gPackedNormalRM"), 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(1)); // RT1: Albedo+AO
	glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "gAlbedoAO"), 2);

	//Bind stochastic direction texture (was missing!)
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, m_dirTex->ID());
	glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "randTex"), 3);

	//Bind previous frame HDR color for ray hit sampling (was missing!)
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, m_historyColor->ID());
	glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "prevColor"), 4);

	//Bind IBL irradiance cubemap for fallback (was missing!)
	GLuint ibl = 0;
	if (skybox) {
		// Prefer diffuse irradiance map for indirect diffuse
		ibl = skybox->GetIrradianceMap();
		if (ibl == 0) ibl = skybox->GetEnvironmentMap();
	}
	glActiveTexture(GL_TEXTURE5);
	if (ibl != 0) {
		glBindTexture(GL_TEXTURE_CUBE_MAP, ibl);
	}
	glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "iblIrradiance"), 5);

	// Bind output texture
	glBindImageTexture(0, m_ssgiRaw->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

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
	glUniform1f(glGetUniformLocation(m_csRaymarch->GetProgramID(), "iblFallbackStrength"), 1.0f);
	glUniform1i(glGetUniformLocation(m_csRaymarch->GetProgramID(), "hasIBL"), ibl != 0 ? 1 : 0);

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

	//Bind textures with correct samplers
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_ssgiQuarter->ID());
	glUniform1i(glGetUniformLocation(m_csBilateral->GetProgramID(), "inTex"), 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	glUniform1i(glGetUniformLocation(m_csBilateral->GetProgramID(), "gDepth"), 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0)); // RT0: Packed normal+rough+metal
	glUniform1i(glGetUniformLocation(m_csBilateral->GetProgramID(), "gPackedNormalRM"), 2);

	// Bind output texture
	glBindImageTexture(3, m_ssgiQuarterBlur->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	//Use simple exponential thresholds matching SSAO exactly
	// SSAO uses depthThreshold=0.02 and normalThreshold=0.2 successfully
	// These are RAW depth/normal differences, not view-space scaled values

	float depthSigma = 0.02f; // Match SSAO's proven depth threshold
	float normalThresh = 0.2f; // Match SSAO's proven normal threshold

	glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "depthSigma"), depthSigma);
	glUniform1f(glGetUniformLocation(m_csBilateral->GetProgramID(), "normalThresh"), normalThresh);
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

	//Bind textures with correct samplers
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_ssgiQuarterBlur->ID());
	glUniform1i(glGetUniformLocation(m_csUpsample->GetProgramID(), "inLow"), 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_ssgiRaw->ID());
	glUniform1i(glGetUniformLocation(m_csUpsample->GetProgramID(), "inHigh"), 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	glUniform1i(glGetUniformLocation(m_csUpsample->GetProgramID(), "depthTex"), 2);

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0)); // RT0: Packed normal+rough+metal
	glUniform1i(glGetUniformLocation(m_csUpsample->GetProgramID(), "normalTex"), 3);

	glBindImageTexture(4, m_ssgiBlur->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	glUniform2f(glGetUniformLocation(m_csUpsample->GetProgramID(), "invDst"), 1.0f / float(m_hw), 1.0f / float(m_hh));
	glUniform2f(glGetUniformLocation(m_csUpsample->GetProgramID(), "invFull"), 1.0f / float(m_w), 1.0f / float(m_h));

	//Use same thresholds as bilateral for consistency
	float depthSigma = 0.02f; // Match SSAO and bilateral
	float normalThresh = 0.2f; // Match SSAO and bilateral

	glUniform1f(glGetUniformLocation(m_csUpsample->GetProgramID(), "depthSigma"), depthSigma);
	glUniform1f(glGetUniformLocation(m_csUpsample->GetProgramID(), "normalThresh"), normalThresh);

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
	glBindTextureUnit(4, ctx.gbufferFBO->GetColorAttachment(0)); // RT0: Packed normal+rough+metal

	//Bind output texture - now writes to WORKING-RES (not final)
	glBindImageTexture(5, m_ssgiWork->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	//Use proven temporal parameters
	// Alpha should be SMALL (0.1-0.15) for stable convergence
	// Thresholds should match SSAO for consistent rejection

	float alpha = std::max(0.05f, std::min(ctx.ssgiTemporalAlpha, 0.3f)); // Clamp to safe range
	float depthThreshold = 0.02f; // Match SSAO's depth threshold
	float normalThreshold = 0.2f; // Match SSAO's normal threshold

	glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "alpha"), alpha);
	glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "depthThreshold"), depthThreshold);
	glUniform1f(glGetUniformLocation(m_csTemporal->GetProgramID(), "normalThreshold"), normalThreshold);

	//Pass YCoCg flag to match TAA settings
	glUniform1i(glGetUniformLocation(m_csTemporal->GetProgramID(), "useYCoCg"), ctx.taaUseYCoCg ? 1 : 0);


	// Dispatch
	GLuint gx = (m_hw + 7) / 8;
	GLuint gy = (m_hh + 7) / 8;
	glDispatchCompute(gx, gy, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	//Copy AFTER temporal resolve completes, not before
	// This ensures history contains the converged result for next frame
	glCopyImageSubData(m_ssgiWork->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
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

	//Start from working-res texture (not final)
	GLuint src = m_ssgiWork->ID();
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

	//Copy last output back into m_ssgiWork (not m_ssgiTex)
	TexturePtr& last = ((m_kawasePasses - 1) % 2 == 0) ? m_kawasePing : m_kawasePong;
	glCopyImageSubData(last->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
		m_ssgiWork->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
		m_hw, m_hh, 1);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glUseProgram(0);
}

void SSGIPass::runFinalUpsample(RenderContext& ctx) {
	//Final stage - upsample from working-res to FULL-RES
	// This is what the lighting pass expects!
	glUseProgram(m_csFinalUpsample->GetProgramID());

	// Bind input textures
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_ssgiWork->ID()); // Working-res SSGI
	glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "inSSGI"), 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture()); // Full-res depth
	glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "depthTex"), 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0)); // Full-res normals
	glUniform1i(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "normalTex"), 2);

	// Bind output texture - FULL RESOLUTION
	glBindImageTexture(3, m_ssgiTex->ID(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

	// Set uniforms
	glUniform2f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "invDst"),
		1.0f / float(m_w), 1.0f / float(m_h));

	// Use same thresholds as other passes for consistency
	float depthSigma = 0.02f;
	float normalThresh = 0.2f;

	glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "depthSigma"), depthSigma);
	glUniform1f(glGetUniformLocation(m_csFinalUpsample->GetProgramID(), "normalThresh"), normalThresh);


	// Dispatch for FULL resolution
	GLuint gx = (m_w + 7) / 8;
	GLuint gy = (m_h + 7) / 8;
	glDispatchCompute(gx, gy, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	glUseProgram(0);
}
void SSGIPass::CaptureHistory(RenderContext& ctx) {
	//Capture current frame's HDR color for next frame's ray marching
	// This must match the WORKING RESOLUTION (half-res by default) for correct sampling

	FrameBuffer* sourceFBO = (ctx.taaFBO && ctx.enableTAA) ? ctx.taaFBO : ctx.hdrFBO.get();
	if (!sourceFBO) return;

	//Use FrameBuffer's existing GetColorAttachment() directly - no temp FBO needed
	// Blit from FULL resolution to WORKING resolution with LINEAR filtering
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFBO->GetFBO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_kawaseFBO); // Reuse existing FBO
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, m_historyColor->ID(), 0);

	glBlitFramebuffer(0, 0, ctx.width, ctx.height,
		0, 0, m_hw, m_hh,
		GL_COLOR_BUFFER_BIT, GL_LINEAR);

	// Cleanup - restore default framebuffer
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
