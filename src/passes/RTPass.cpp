#include "RTPass.h"
#include "../RenderContext.h"
#include "../FrameBuffer.h"
#include "../ShaderLoader.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../ComputeShader.h"
#include "../BVHBuilder.h"
#include <iostream>


RTPass::~RTPass()
{
	if (m_bvhBuffers.triangleSSBO) {
		glDeleteBuffers(1, &m_bvhBuffers.triangleSSBO);
	}
	if (m_bvhBuffers.bvhSSBO) {
		glDeleteBuffers(1, &m_bvhBuffers.bvhSSBO);
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

	// Create framebuffer to hold ray traced output
	m_rtFBO = std::make_unique<FrameBuffer>(context.width, context.height,
		std::vector<GLenum>{ GL_RGBA16F }, false, false, 1, false);
		
	// Initialize BVH buffers
	glGenBuffers(1, &m_bvhBuffers.triangleSSBO);
	glGenBuffers(1, &m_bvhBuffers.bvhSSBO);
	
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
	
	// Resize framebuffer
	m_rtFBO->Resize(newWidth, newHeight);
	
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
	
	// Build BVH if dirty
	if (m_bvhDirty) {
		runWarmup(sceneGraph);
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
	runRayTracing(ctx, sceneGraph, camera);
	
	// Accumulate results
	accumulateFrame(ctx);
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
	params.maxLeafPrimitives = 4;
	params.maxDepth = 30;
	params.sahBuckets = 16;
	
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
	const std::shared_ptr<Camera>& camera)
{
	if (!m_rtShader) return;
	
	// Activate compute shader
	glUseProgram(m_rtShader->GetProgramID());
	
	// Bind BVH buffers
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_bvhBuffers.triangleSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_bvhBuffers.bvhSSBO);
	
	// Bind G-buffer textures for input
	if (ctx.gbufferFBO) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(0)); // Position
		
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(1)); // Normal
		
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(2)); // Albedo
		
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetColorAttachment(3)); // Material (roughness/metallic)
		
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());  // Depth
	}
	
	// Bind output texture
	m_rtShader->BindTextureWrite(m_rtTexture->ID(), 5, 0, GL_RGBA16F);
	
	// Set uniforms
	m_rtShader->SetUniform("u_frameIndex", m_frameIndex);
	m_rtShader->SetUniform("u_sampleCount", ctx.rtSamplesPerPixel);
	m_rtShader->SetUniform("u_maxBounces", ctx.rtMaxBounces);
	m_rtShader->SetUniform("u_triangleCount", static_cast<int>(m_bvhBuffers.triangleCount));
	m_rtShader->SetUniform("u_bvhNodeCount", static_cast<int>(m_bvhBuffers.nodeCount));
	
	// Camera uniforms
	m_rtShader->SetUniform("u_cameraPos", camera->GetCameraPosition());
	m_rtShader->SetUniform("u_cameraFront", camera->GetCameraFrontVector());
	m_rtShader->SetUniform("u_cameraRight", camera->GetCameraRightVector());
	m_rtShader->SetUniform("u_cameraUp", camera->GetCameraUpVector());
	m_rtShader->SetUniform("u_fov", glm::radians(camera->GetCameraFov()));
	
	// Resolution
	m_rtShader->SetUniform("u_resolution", glm::vec2(m_w, m_h));
	
	// Dispatch compute shader
	int groupsX = (m_w + 7) / 8;
	int groupsY = (m_h + 7) / 8;
	m_rtShader->Dispatch(groupsX, groupsY, 1);
	
	// Memory barrier
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	
	// Cleanup
	glUseProgram(0);
}

void RTPass::accumulateFrame(RenderContext& ctx)
{
	// Accumulation is now handled in the compute shader itself
	// The shader uses: color = mix(prevColor, currentColor, 1.0 / (frameIndex + 1))
	// This provides progressive refinement over time
	
	m_frameIndex++;
	
	// Optional: Log convergence progress
	if (m_frameIndex % 100 == 0) {
		std::cout << "[RTPass] Accumulated " << m_frameIndex << " frames" << std::endl;
	}
	
	// If we want to limit accumulation for performance
	if (ctx.rtAccumulate && m_frameIndex >= 1000) {
		// Stop accumulating after 1000 frames (fully converged)
		// But keep rendering to maintain the result
	}
}

void RTPass::resetAccumulation()
{
	m_frameIndex = 0;
	std::cout << "[RTPass] Accumulation reset" << std::endl;
}
