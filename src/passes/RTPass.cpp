#include "RTPass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../ComputeShader.h"
#include "../BVHBuilder.h"
#include "../LightManager.h"
#include <iostream>


RTPass::~RTPass()
{
	if (m_bvhBuffers.triangleSSBO) {
		glDeleteBuffers(1, &m_bvhBuffers.triangleSSBO);
	}
	if (m_bvhBuffers.bvhSSBO) {
		glDeleteBuffers(1, &m_bvhBuffers.bvhSSBO);
	}
	if (m_rtFBO_ID) {
		glDeleteFramebuffers(1, &m_rtFBO_ID);
	}
	// Removed deletion of m_lightBuffers.lightSSBO (managed by LightManager)
	
	// Clean up SVGF buffers
	if (m_svgfBuffers.momentsSSBO) {
		glDeleteBuffers(1, &m_svgfBuffers.momentsSSBO);
	}
	if (m_svgfBuffers.historyLengthSSBO) {
		glDeleteBuffers(1, &m_svgfBuffers.historyLengthSSBO);
	}
}

bool RTPass::Initialize(RenderContext& context)
{
	std::cout << "[RTPass] Initializing Ray Tracing Pass..." << std::endl;

	// Create compute shader for ray tracing
	m_rtShader = std::make_unique<ComputeShader>();
	if (!m_rtShader->CreateFromFile("shaders/rt_bidirectional_comp.glsl")) {
		std::cerr << "[RTPass] Failed to compile rt_bidirectional_comp.glsl" << std::endl;
		return false;
	}

	// Create SVGF denoising shaders
	m_svgfTemporalShader = std::make_unique<ComputeShader>();
	if (!m_svgfTemporalShader->CreateFromFile("shaders/svgf_temporal_comp.glsl")) {
		std::cerr << "[RTPass] Failed to compile svgf_temporal_comp.glsl" << std::endl;
		return false;
	}

	m_svgfVarianceShader = std::make_unique<ComputeShader>();
	if (!m_svgfVarianceShader->CreateFromFile("shaders/svgf_variance_comp.glsl")) {
		std::cerr << "[RTPass] Failed to compile svgf_variance_comp.glsl" << std::endl;
		return false;
	}

	m_svgfAtrousShader = std::make_unique<ComputeShader>();
	if (!m_svgfAtrousShader->CreateFromFile("shaders/svgf_atrous_comp.glsl")) {
		std::cerr << "[RTPass] Failed to compile svgf_atrous_comp.glsl" << std::endl;
		return false;
	}

	// Allocate render target texture
	m_rtTexture = Texture::Builder::Texture2D(context.width, context.height, GL_RGBA16F)
		.Format(GL_RGBA).DataType(GL_HALF_FLOAT)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	// Allocate accumulation texture
	m_accumulationTexture = Texture::Builder::Texture2D(context.width, context.height, GL_RGBA32F)
		.Format(GL_RGBA).DataType(GL_FLOAT)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	// Allocate variance texture
	m_varianceTexture = Texture::Builder::Texture2D(context.width, context.height, GL_RG16F)
		.Format(GL_RG).DataType(GL_HALF_FLOAT)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	// SVGF denoising textures
	m_prevRadianceTexture = Texture::Builder::Texture2D(context.width, context.height, GL_RGBA16F)
		.Format(GL_RGBA).DataType(GL_HALF_FLOAT)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	m_momentsTexture = Texture::Builder::Texture2D(context.width, context.height, GL_RG32F)
		.Format(GL_RG).DataType(GL_FLOAT)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	m_historyLengthTexture = Texture::Builder::Texture2D(context.width, context.height, GL_R16F)
		.Format(GL_RED).DataType(GL_HALF_FLOAT)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	m_denoisedTexture = Texture::Builder::Texture2D(context.width, context.height, GL_RGBA16F)
		.Format(GL_RGBA).DataType(GL_HALF_FLOAT)
		.FilterMode(GL_LINEAR, GL_LINEAR)
		.WrapMode(GL_CLAMP_TO_EDGE)
		.Build();

	// Create framebuffer to hold ray traced output (for blitting to HDR)
	// We'll manually attach our compute shader output texture
	glGenFramebuffers(1, &m_rtFBO_ID);
	glBindFramebuffer(GL_FRAMEBUFFER, m_rtFBO_ID);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_rtTexture->ID(), 0);

	// Check framebuffer completeness
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		std::cerr << "[RTPass] RT Framebuffer is not complete!" << std::endl;
		return false;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// Initialize BVH buffers
	glGenBuffers(1, &m_bvhBuffers.triangleSSBO);
	glGenBuffers(1, &m_bvhBuffers.bvhSSBO);

	// Initialize SVGF buffers
	glGenBuffers(1, &m_svgfBuffers.momentsSSBO);
	glGenBuffers(1, &m_svgfBuffers.historyLengthSSBO);
	
	size_t pixelCount = context.width * context.height;
	m_svgfBuffers.pixelCount = pixelCount;

	// Allocate SVGF buffers (zero-initialized)
	std::vector<char> emptyData(pixelCount * 8, 0); // 8 bytes per pixel for RG32F moments
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_svgfBuffers.momentsSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, emptyData.size(), emptyData.data(), GL_DYNAMIC_COPY);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	std::vector<char> emptyHistoryData(pixelCount * 2, 0); // 2 bytes per pixel for R16F
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_svgfBuffers.historyLengthSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, emptyHistoryData.size(), emptyHistoryData.data(), GL_DYNAMIC_COPY);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	// Removed generation of separate light SSBO; will use LightManager's SSBO directly

	// Removed ReSTIR reservoir buffers

	m_w = context.width;
	m_h = context.height;

	std::cout << "[RTPass] Initialization complete" << std::endl;
	return true;
}

void RTPass::Resize(RenderContext& context, int newWidth, int newHeight)
{
	if (newWidth == m_w && newHeight == m_h) {
		return;
	}

	std::cout << "[RTPass] Resizing to " << newWidth << "x" << newHeight << std::endl;

	m_w = newWidth;
	m_h = newHeight;
	m_renderResolutionScale = context.rtResolutionScale;

	// Resize ray traced output texture
	m_rtTexture->Resize(newWidth, newHeight);
	m_accumulationTexture->Resize(newWidth, newHeight);
	m_varianceTexture->Resize(newWidth, newHeight);

	// Resize SVGF textures
	m_prevRadianceTexture->Resize(newWidth, newHeight);
	m_momentsTexture->Resize(newWidth, newHeight);
	m_historyLengthTexture->Resize(newWidth, newHeight);
	m_denoisedTexture->Resize(newWidth, newHeight);

	// Reattach resized texture to FBO
	glBindFramebuffer(GL_FRAMEBUFFER, m_rtFBO_ID);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_rtTexture->ID(), 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// Resize SVGF buffers
	size_t pixelCount = newWidth * newHeight;
	m_svgfBuffers.pixelCount = pixelCount;
	
	std::vector<char> emptyData(pixelCount * 8, 0);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_svgfBuffers.momentsSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, emptyData.size(), emptyData.data(), GL_DYNAMIC_COPY);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	std::vector<char> emptyHistoryData(pixelCount * 2, 0);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_svgfBuffers.historyLengthSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, emptyHistoryData.size(), emptyHistoryData.data(), GL_DYNAMIC_COPY);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	// Reset accumulation on resize
	resetAccumulation();
}

void RTPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox)
{
	// Only run if path tracing mode is enabled
	if (ctx.rendererMode != RenderContext::RendererMode::PATH_TRACED) {
		return;
	}

	//  Only rebuild BVH when transforms actually change
	// Check if scene geometry has been modified
	if (sceneGraph->IsBVHDirty()) {
		std::cout << "[RTPass] BVH dirty - rebuilding with updated transforms..." << std::endl;
		m_bvhDirty = true;
	}

	// Build BVH if dirty
	if (m_bvhDirty) {
		runWarmup(sceneGraph);
		// Clear dirty flag after successful rebuild
		sceneGraph->ClearBVHDirty();
	}

	// Check if camera moved (reset accumulation)
	static glm::vec3 lastCameraPos = camera->GetCameraPosition();
	static glm::vec3 lastCameraFront = camera->GetCameraFrontVector();

	glm::vec3 currentPos = camera->GetCameraPosition();
	glm::vec3 currentFront = camera->GetCameraFrontVector();

	float posDelta = glm::length(currentPos - lastCameraPos);
	float rotDelta = glm::dot(currentFront, lastCameraFront);

	if (posDelta > 0.001f || rotDelta < 0.9999f) {
		resetAccumulation();
		lastCameraPos = currentPos;
		lastCameraFront = currentFront;
	}

	// Run ray tracing
	runRayTracing(ctx, sceneGraph, camera, skybox);

	// Run SVGF denoising pipeline
	if (ctx.rtDenoise) {
		runSVGFTemporal(ctx, camera);
		runSVGFVariance(ctx);
		
		// Run multiple à-trous iterations for progressive filtering
		for (int i = 0; i < ctx.svgfAtrousIterations; i++) {
			runSVGFAtrous(ctx, i);
		}
	}

	// Accumulate results
	accumulateFrame(ctx);

	// Copy ray traced output to HDR buffer for post-processing
	copyToHDRBuffer(ctx);
}

void RTPass::runWarmup(const std::shared_ptr<SceneGraph>& sceneGraph)
{
	std::cout << "[RTPass] Building BVH..." << std::endl;
	buildAndUploadBVH(sceneGraph);
	m_bvhDirty = false;
}

void RTPass::buildAndUploadBVH(const std::shared_ptr<SceneGraph>& sceneGraph)
{
	// Build BVH from scene
	BVHBuilder::BuildParams params;
	params.maxLeafPrimitives = 2;
	params.maxDepth = 24;
	params.sahBuckets = 32;

	RT::BVHData bvhData = BVHBuilder::BuildFromScene(sceneGraph, params);

	if (bvhData.triangles.empty() || bvhData.nodes.empty()) {
		std::cerr << "[RTPass] Failed to build BVH - no geometry" << std::endl;
		return;
	}

	m_bvhBuffers.triangleCount = bvhData.triangles.size();
	m_bvhBuffers.nodeCount = bvhData.nodes.size();

	// Upload triangle data
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_bvhBuffers.triangleSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER,
		bvhData.GetTriangleBufferSize(),
		bvhData.triangles.data(),
		GL_STATIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	// Upload BVH node data
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_bvhBuffers.bvhSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER,
		bvhData.GetNodeBufferSize(),
		bvhData.nodes.data(),
		GL_STATIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	std::cout << "[RTPass] BVH uploaded: "
		<< m_bvhBuffers.triangleCount << " triangles, "
		<< m_bvhBuffers.nodeCount << " nodes, "
		<< "max depth " << bvhData.maxDepth << std::endl;
}

void RTPass::runRayTracing(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<Skybox>& skybox)
{
	if (!m_rtShader) return;

	// Upload light data from LightManager
	if (ctx.lightManager) {
		ctx.lightManager->UpdateGPUBuffers();
		auto lights = ctx.lightManager->GetEnabledLights();
		m_lightBuffers.lightCount = lights.size();
		// Use LightManager SSBO directly; no assignment altering ownership
		m_lightBuffers.lightSSBO = ctx.lightManager->GetLightDataSSBO();
	}
	else {
		m_lightBuffers.lightCount =0;
	}
	// Activate compute shader
	glUseProgram(m_rtShader->GetProgramID());

	// Bind BVH buffers
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_bvhBuffers.triangleSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_bvhBuffers.bvhSSBO);

	// Bind light buffer (from LightManager)
	if (m_lightBuffers.lightSSBO != 0) {
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_lightBuffers.lightSSBO);
		std::cout << "[RTPass] Bound light SSBO to binding point 2" << std::endl;

		// Verify binding
		GLint boundBuffer = 0;
		glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 2, &boundBuffer);
		std::cout << "[RTPass] Verified bound buffer at point 2: " << boundBuffer << std::endl;
	}
	else {
		std::cout << "[RTPass] WARNING: Light SSBO is 0, not binding!" << std::endl;
	}

	// Bind G-buffer textures for input (CORRECTED LAYOUT)
	if (ctx.gbufferFBO) {
		// RT0: Oct normal (RG) + Roughness (B) + Metallic (A)
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));

		// RT1: Albedo (RGB) + Occlusion (A)
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(1));

		// RT2: Emissive (RGB) + Specular luminance (A)
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(2));

		// Depth buffer (for position reconstruction)
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	}

	// Bind output texture
	m_rtShader->BindTextureWrite(m_rtTexture->ID(), 5, 0, GL_RGBA16F);

	// Set uniforms
	m_rtShader->SetUniform("u_frameIndex", m_frameIndex);
	m_rtShader->SetUniform("u_sampleCount", ctx.rtSamplesPerPixel);
	m_rtShader->SetUniform("u_maxBounces", ctx.rtMaxBounces);
	m_rtShader->SetUniform("u_triangleCount", static_cast<int>(m_bvhBuffers.triangleCount));
	m_rtShader->SetUniform("u_bvhNodeCount", static_cast<int>(m_bvhBuffers.nodeCount));

	// Light system uniforms
	m_rtShader->SetUniform("u_lightCount", static_cast<int>(m_lightBuffers.lightCount));
	m_rtShader->SetUniform("u_enableNEE", ctx.rtEnableNEE);
	m_rtShader->SetUniform("u_enableMIS", ctx.rtEnableMIS);
	m_rtShader->SetUniform("u_misWeight", 1.0f);

	// Removed ReSTIR settings uniforms

	// BVH Debug Visualization
	m_rtShader->SetUniform("u_displayBVH", ctx.rtDisplayBVH);
	m_rtShader->SetUniform("u_displayMultipleBVHLayers", ctx.rtDisplayMultipleBVHLayers);
	m_rtShader->SetUniform("u_BVHLayerToDisplay", ctx.rtBVHLayerToDisplay);
	m_rtShader->SetUniform("u_heatmapColorLimit", ctx.rtHeatmapColorLimit);

	// IBL Environment
	m_rtShader->SetUniform("u_enableIBL", ctx.rtEnableIBL);
	m_rtShader->SetUniform("u_iblIntensity", ctx.rtIBLIntensity);

	// Bind IBL cubemaps if available
	if (ctx.rtEnableIBL && skybox && skybox->IsReady()) {
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetEnvironmentMap());

		glActiveTexture(GL_TEXTURE6);
		glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetIrradianceMap());

		glActiveTexture(GL_TEXTURE7);
		glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetPrefilteredMap());

		std::cout << "[RTPass] IBL textures bound: Environment=" << skybox->GetEnvironmentMap()
			<< " Irradiance=" << skybox->GetIrradianceMap()
			<< " Prefiltered=" << skybox->GetPrefilteredMap() << std::endl;
	}

	// Camera uniforms
	m_rtShader->SetUniform("u_cameraPos", camera->GetCameraPosition());
	m_rtShader->SetUniform("u_cameraFront", camera->GetCameraFrontVector());
	m_rtShader->SetUniform("u_cameraRight", camera->GetCameraRightVector());
	m_rtShader->SetUniform("u_cameraUp", camera->GetCameraUpVector());
	m_rtShader->SetUniform("u_fov", glm::radians(camera->GetCameraFov()));

	// Matrices for depth reconstruction
	glm::mat4 invView = glm::inverse(ctx.view);
	glm::mat4 invProj = glm::inverse(ctx.proj);
	m_rtShader->SetUniform("u_invView", invView);
	m_rtShader->SetUniform("u_invProj", invProj);

	// Camera planes
	m_rtShader->SetUniform("u_cameraNear", camera->GetCameraNearPlane());
	m_rtShader->SetUniform("u_cameraFar", camera->GetCameraFarPlane());

	// Resolution
	m_rtShader->SetUniform("u_resolution", glm::vec2(m_w, m_h));

	// Dispatch compute shader
	int groupsX = (m_w + 7) / 8;
	int groupsY = (m_h + 7) / 8;
	m_rtShader->Dispatch(groupsX, groupsY, 1);

	// Memory barrier
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

	// Cleanup
	glUseProgram(0);
}

void RTPass::accumulateFrame(RenderContext& ctx)
{
	m_frameIndex++;

	// Optional: Log convergence progress
	if (m_frameIndex % 100 == 0) {
		std::cout << "[RTPass] Accumulated " << m_frameIndex << " frames" << std::endl;
	}
}

void RTPass::runSVGFTemporal(RenderContext& ctx, const std::shared_ptr<Camera>& camera)
{
	if (!m_svgfTemporalShader) return;

	glUseProgram(m_svgfTemporalShader->GetProgramID());

	// Bind input textures
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_rtTexture->ID());

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_prevRadianceTexture->ID());

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));

	// Previous frame depth (use current for now, will be improved with proper history)
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());

	// Bind output images
	m_svgfTemporalShader->BindTextureWrite(m_denoisedTexture->ID(), 5, 0, GL_RGBA16F);
	m_svgfTemporalShader->BindTextureWrite(m_momentsTexture->ID(), 6, 0, GL_RG32F);
	m_svgfTemporalShader->BindTextureWrite(m_historyLengthTexture->ID(), 7, 0, GL_R16F);

	// Set uniforms
	m_svgfTemporalShader->SetUniform("u_resolution", glm::vec2(m_w, m_h));
	m_svgfTemporalShader->SetUniform("u_invView", glm::inverse(ctx.view));
	m_svgfTemporalShader->SetUniform("u_invProj", glm::inverse(ctx.proj));
	m_svgfTemporalShader->SetUniform("u_prevViewProj", ctx.prevProj * ctx.prevView);
	m_svgfTemporalShader->SetUniform("u_cameraNear", camera->GetCameraNearPlane());
	m_svgfTemporalShader->SetUniform("u_cameraFar", camera->GetCameraFarPlane());
	m_svgfTemporalShader->SetUniform("u_frameIndex", m_frameIndex);
	m_svgfTemporalShader->SetUniform("u_temporalAlpha", ctx.svgfTemporalAlpha);
	m_svgfTemporalShader->SetUniform("u_varianceClipGamma", ctx.svgfVarianceClipGamma);
	m_svgfTemporalShader->SetUniform("u_depthThreshold", ctx.svgfDepthThreshold);
	m_svgfTemporalShader->SetUniform("u_normalThreshold", ctx.svgfNormalThreshold);

	// Dispatch
	int groupsX = (m_w + 7) / 8;
	int groupsY = (m_h + 7) / 8;
	m_svgfTemporalShader->Dispatch(groupsX, groupsY, 1);

	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	glUseProgram(0);

	// Copy current radiance to prev radiance for next frame
	glCopyImageSubData(
		m_rtTexture->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
		m_prevRadianceTexture->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
		m_w, m_h, 1
	);
}

void RTPass::runSVGFVariance(RenderContext& ctx)
{
	if (!m_svgfVarianceShader) return;

	glUseProgram(m_svgfVarianceShader->GetProgramID());

	// Bind input textures
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_momentsTexture->ID());

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_historyLengthTexture->ID());

	// Bind output image (reuse variance texture)
	m_svgfVarianceShader->BindTextureWrite(m_varianceTexture->ID(), 2, 0, GL_R16F);

	// Set uniforms
	m_svgfVarianceShader->SetUniform("u_resolution", glm::vec2(m_w, m_h));

	// Dispatch
	int groupsX = (m_w + 7) / 8;
	int groupsY = (m_h + 7) / 8;
	m_svgfVarianceShader->Dispatch(groupsX, groupsY, 1);

	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	glUseProgram(0);
}

void RTPass::runSVGFAtrous(RenderContext& ctx, int iteration)
{
	if (!m_svgfAtrousShader) return;

	glUseProgram(m_svgfAtrousShader->GetProgramID());

	// Ping-pong between denoised texture and rt texture
	GLuint inputTex = (iteration == 0) ? m_denoisedTexture->ID() : m_rtTexture->ID();
	GLuint outputTex = m_rtTexture->ID();

	// Bind input textures
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, inputTex);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_varianceTexture->ID());

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0));

	// Bind output image
	m_svgfAtrousShader->BindTextureWrite(outputTex, 4, 0, GL_RGBA16F);

	// Set uniforms
	m_svgfAtrousShader->SetUniform("u_resolution", glm::vec2(m_w, m_h));
	m_svgfAtrousShader->SetUniform("u_iteration", iteration);
	m_svgfAtrousShader->SetUniform("u_phiColor", ctx.svgfPhiColor);
	m_svgfAtrousShader->SetUniform("u_phiNormal", ctx.svgfPhiNormal);
	m_svgfAtrousShader->SetUniform("u_phiDepth", ctx.svgfPhiDepth);

	// Dispatch
	int groupsX = (m_w + 7) / 8;
	int groupsY = (m_h + 7) / 8;
	m_svgfAtrousShader->Dispatch(groupsX, groupsY, 1);

	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	// Copy output back to denoised texture for next iteration
	if (iteration < ctx.svgfAtrousIterations - 1) {
		glCopyImageSubData(
			m_rtTexture->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
			m_denoisedTexture->ID(), GL_TEXTURE_2D, 0, 0, 0, 0,
			m_w, m_h, 1
		);
	}

	glUseProgram(0);
}

void RTPass::resetAccumulation()
{
	m_frameIndex = 0;
	std::cout << "[RTPass] Accumulation reset" << std::endl;
}

void RTPass::copyToHDRBuffer(RenderContext& ctx)
{
	if (!ctx.hdrFBO || !m_rtTexture || !m_rtFBO_ID) {
		std::cerr << "[RTPass] Cannot copy to HDR buffer - missing FBO or texture" << std::endl;
		return;
	}

	// Disable depth test and blending for blit
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	// Bind RT FBO to read framebuffer
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_rtFBO_ID);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ctx.hdrFBO->GetFBO());

	// Blit from RT output to HDR buffer
	glBlitFramebuffer(
		0, 0, m_w, m_h,    // src rect
		0, 0, ctx.width, ctx.height,       // dst rect
		GL_COLOR_BUFFER_BIT,
		GL_LINEAR
	);

	// Unbind framebuffers
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	std::cout << "[RTPass] Copied ray traced output to HDR buffer" << std::endl;
}

GLuint RTPass::GetOutputTexture() const
{
	return m_rtTexture ? m_rtTexture->ID() : 0;
}


