#include "TAAPass.h"
#include "../PresentTarget.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../SceneGraph.h"
#include "../Camera.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <iostream>

#pragma warning(disable: 4996)  // Suppress deprecated function warnings for SceneGraph legacy API

TAAPass::TAAPass() {}

TAAPass::~TAAPass() {
	if (m_velocityShader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_velocityShader);
	if (m_resolveShader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_resolveShader);
	DestroyHistoryValidationTextures();
}

void TAAPass::DestroyHistoryValidationTextures() {
	if (m_historyDepthTex) {
		if (rhi::HasGpu()) rhi::Gpu().DestroyTexture(m_historyDepthTex);
		m_historyDepthTex = rhi::TextureHandle{};
	}
	if (m_historyNormalTex) {
		if (rhi::HasGpu()) rhi::Gpu().DestroyTexture(m_historyNormalTex);
		m_historyNormalTex = rhi::TextureHandle{};
	}
}

void TAAPass::RecreateHistoryValidationTextures(int width, int height) {
	DestroyHistoryValidationTextures();

	rhi::Device& gpu = rhi::Gpu();
	rhi::TextureDesc depthDesc;
	depthDesc.type = rhi::TextureType::Tex2D;
	depthDesc.format = rhi::Format::D24;
	depthDesc.width = width;
	depthDesc.height = height;
	m_historyDepthTex = gpu.CreateTexture(depthDesc);
	gpu.SetTextureFilter(m_historyDepthTex, rhi::Filter::Nearest, rhi::Filter::Nearest);
	gpu.SetTextureWrapAxis(m_historyDepthTex, 0, rhi::Wrap::ClampToEdge);
	gpu.SetTextureWrapAxis(m_historyDepthTex, 1, rhi::Wrap::ClampToEdge);
	gpu.SetTextureCompare(m_historyDepthTex, false, rhi::CompareOp::LessEqual);
	float clearDepth = 1.0f;
	gpu.ClearTexture(m_historyDepthTex, 0, rhi::PixelFormat::Depth, rhi::PixelType::Float, &clearDepth);

	rhi::TextureDesc normalDesc;
	normalDesc.type = rhi::TextureType::Tex2D;
	// Must match G-buffer RT0 (CopyTexture requires compatible formats).
	normalDesc.format = rhi::Format::RGBA16;
	normalDesc.width = width;
	normalDesc.height = height;
	m_historyNormalTex = gpu.CreateTexture(normalDesc);
	gpu.SetTextureFilter(m_historyNormalTex, rhi::Filter::Nearest, rhi::Filter::Nearest);
	gpu.SetTextureWrapAxis(m_historyNormalTex, 0, rhi::Wrap::ClampToEdge);
	gpu.SetTextureWrapAxis(m_historyNormalTex, 1, rhi::Wrap::ClampToEdge);
	const uint16_t clearNormal[4] = { 0, 0, 0, 0 };
	gpu.ClearTexture(m_historyNormalTex, 0, rhi::PixelFormat::RGBA, rhi::PixelType::UShort, clearNormal);
}

bool TAAPass::Initialize(RenderContext& context) {
	m_velocityShader = CreateShaderProgram("shaders/velocity_vert.glsl",
		"shaders/velocity_frag.glsl");
	m_resolveShader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
		"shaders/taa_resolve.glsl");

	if (!m_velocityShader || !m_resolveShader) {
		std::cerr << "[TAAPass] Failed to create shaders.\n";
		return false;
	}

	// Create framebuffers
	m_velocityFBO = std::make_unique<FrameBuffer>(
		context.width, context.height,
		std::vector<rhi::Format>{ rhi::Format::RG16F },
		true, false
	);

	m_currentFBO = std::make_unique<FrameBuffer>(
		context.width, context.height,
		std::vector<rhi::Format>{ rhi::Format::RGB16F },
		false, false
	);

	m_historyFBO = std::make_unique<FrameBuffer>(
		context.width, context.height,
		std::vector<rhi::Format>{ rhi::Format::RGB16F },
		false, false
	);

	if (!m_velocityFBO->IsComplete() || !m_currentFBO->IsComplete() || !m_historyFBO->IsComplete()) {
		std::cerr << "[TAAPass] TAA framebuffers not complete!\n";
		return false;
	}
	RecreateHistoryValidationTextures(context.width, context.height);

	// Expose velocity texture in context for other passes (e.g., SSGI)
	context.velocityTex = m_velocityFBO->GetColorAttachment(0);

	rhi::Device& gpu = rhi::Gpu();
	m_velocityUniforms.view = gpu.GetUniformLocation(m_velocityShader, "view");
	m_velocityUniforms.projection = gpu.GetUniformLocation(m_velocityShader, "projection");
	m_velocityUniforms.prevView = gpu.GetUniformLocation(m_velocityShader, "prevView");
	m_velocityUniforms.prevProjection = gpu.GetUniformLocation(m_velocityShader, "prevProjection");

	m_resolveUniforms.currentFrame = gpu.GetUniformLocation(m_resolveShader, "currentFrame");
	m_resolveUniforms.historyFrame = gpu.GetUniformLocation(m_resolveShader, "historyFrame");
	m_resolveUniforms.velocityBuffer = gpu.GetUniformLocation(m_resolveShader, "velocityBuffer");
	m_resolveUniforms.depthBuffer = gpu.GetUniformLocation(m_resolveShader, "depthBuffer");
	m_resolveUniforms.normalBuffer = gpu.GetUniformLocation(m_resolveShader, "normalBuffer");
	m_resolveUniforms.historyDepthBuffer = gpu.GetUniformLocation(m_resolveShader, "historyDepthBuffer");
	m_resolveUniforms.historyNormalBuffer = gpu.GetUniformLocation(m_resolveShader, "historyNormalBuffer");
	m_resolveUniforms.blendFactor = gpu.GetUniformLocation(m_resolveShader, "blendFactor");
	m_resolveUniforms.varianceThreshold = gpu.GetUniformLocation(m_resolveShader, "varianceThreshold");
	m_resolveUniforms.lumaWeight = gpu.GetUniformLocation(m_resolveShader, "lumaWeight");
	m_resolveUniforms.useYCoCg = gpu.GetUniformLocation(m_resolveShader, "useYCoCg");
	m_resolveUniforms.historyValid = gpu.GetUniformLocation(m_resolveShader, "historyValid");
	m_resolveUniforms.screenSize = gpu.GetUniformLocation(m_resolveShader, "screenSize");
	m_resolveUniforms.depthThreshold = gpu.GetUniformLocation(m_resolveShader, "depthThreshold");
	m_resolveUniforms.normalThreshold = gpu.GetUniformLocation(m_resolveShader, "normalThreshold");
	m_resolveUniforms.edgeThreshold = gpu.GetUniformLocation(m_resolveShader, "edgeThreshold");
	m_resolveUniforms.reactiveMaskStrength = gpu.GetUniformLocation(m_resolveShader, "reactiveMaskStrength");

	if constexpr (VerboseLogging) {
		if (m_runtimeVerboseLogging) {
			std::cout << "[TAAPass] Initialized successfully.\n";
		}
	}
	return true;
}

void TAAPass::Resize(RenderContext& context, int newWidth, int newHeight) {
	if (m_velocityFBO) m_velocityFBO->Resize(newWidth, newHeight);
	if (m_currentFBO) m_currentFBO->Resize(newWidth, newHeight);
	if (m_historyFBO) m_historyFBO->Resize(newWidth, newHeight);
	RecreateHistoryValidationTextures(newWidth, newHeight);
	context.velocityTex = m_velocityFBO ? m_velocityFBO->GetColorAttachment(0) : rhi::TextureHandle{};
	// Invalidate history on resize
	m_historyValid = false;
}

glm::vec2 TAAPass::GetJitter(int frameIndex, int pattern) {
	if (pattern == 0) {
		// Halton sequence - properly normalized to pixel space
		auto halton = [](int index, int base) -> float {
			float f = 1.0f;
			float r = 0.0f;
			while (index > 0) {
				f = f / base;
				r = r + f * (index % base);
				index = index / base;
			}
			return r;
			};

		// Generate in [-0.5, 0.5] range, then normalize to pixel space
		float x = (halton((frameIndex % 16) + 1, 2) - 0.5f);
		float y = (halton((frameIndex % 16) + 1, 3) - 0.5f);

		// Return in normalized screen space (not pixel space)
		// The shader will apply this as an offset in projection space
		return glm::vec2(x, y);
	}
	else {
		// Hammersley 8-sample pattern
		const glm::vec2 samples[8] = {
			glm::vec2(0.000000f, 0.000000f),
			glm::vec2(0.500000f, 0.333333f),
			glm::vec2(0.250000f, 0.666667f),
			glm::vec2(0.750000f, 0.111111f),
			glm::vec2(0.125000f, 0.444444f),
			glm::vec2(0.625000f, 0.777778f),
			glm::vec2(0.375000f, 0.222222f),
			glm::vec2(0.875000f, 0.555556f)
		};
		return samples[frameIndex % 8] - glm::vec2(0.5f);
	}
}

void TAAPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox) {
	ExecuteVelocity(ctx, sceneGraph, camera);

	if (ctx.enableTAA) {
		ExecuteResolve(ctx);
	} else {
		ctx.taaFBO = ctx.hdrFBO.get();
	}
}

void TAAPass::PrepareJitter(int pattern) {
	m_prevJitter = m_jitter;
	m_jitter = GetJitter(m_frameIndex, pattern);
}

void TAAPass::ExecuteVelocity(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera) {
	RenderVelocity(ctx, sceneGraph, camera);
}

void TAAPass::ExecuteResolve(RenderContext& ctx) {
	if (!ctx.enableTAA) {
		ctx.taaFBO = ctx.hdrFBO.get();
		return;
	}

	ResolveTemporalAntiAliasing(ctx);
	m_frameIndex++;
}

glm::mat4 TAAPass::RemoveProjectionJitter(const glm::mat4& projection, const glm::vec2& jitterPixels, const RenderContext& ctx) const {
	glm::mat4 unjittered = projection;
	if (ctx.width > 0 && ctx.height > 0) {
		const glm::vec2 jitterNdc = jitterPixels / glm::vec2(static_cast<float>(ctx.width), static_cast<float>(ctx.height));
		unjittered[2][0] -= jitterNdc.x;
		unjittered[2][1] -= jitterNdc.y;
	}
	return unjittered;
}

void TAAPass::RenderVelocity(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera) {
	if (!sceneGraph || !camera) return;

	rhi::Device& gpu = rhi::Gpu();
	m_velocityFBO->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
	gpu.SetDepthTest(true);
	gpu.Clear(rhi::ClearColor | rhi::ClearDepth);

	gpu.UseProgram(m_velocityShader);

	// Remove exactly the jitter UpdateContext applied (zero when TAA was off that frame).
	glm::mat4 currentVelocityProj = ctx.proj;
	currentVelocityProj[2][0] -= ctx.jitterNdc.x;
	currentVelocityProj[2][1] -= ctx.jitterNdc.y;
	glm::mat4 previousVelocityProj = ctx.prevProj;
	previousVelocityProj[2][0] -= ctx.prevJitterNdc.x;
	previousVelocityProj[2][1] -= ctx.prevJitterNdc.y;

	// Upload current and previous matrices
	if (m_velocityUniforms.view >= 0) gpu.SetUniform(m_velocityUniforms.view, ctx.view);
	if (m_velocityUniforms.projection >= 0) gpu.SetUniform(m_velocityUniforms.projection, currentVelocityProj);

	// Previous matrices (stored in context)
	if (m_velocityUniforms.prevView >= 0) gpu.SetUniform(m_velocityUniforms.prevView, ctx.prevView);
	if (m_velocityUniforms.prevProjection >= 0) gpu.SetUniform(m_velocityUniforms.prevProjection, previousVelocityProj);

	// Render scene for motion vectors
	sceneGraph->RenderVelocity(ctx.view, currentVelocityProj, ctx.prevView, previousVelocityProj, m_velocityShader);

	gpu.BindFramebuffer(PresentTarget::Get());

	// Update context velocity texture each frame
	ctx.velocityTex = m_velocityFBO->GetColorAttachment(0);
}

void TAAPass::ResolveTemporalAntiAliasing(RenderContext& ctx) {
	rhi::Device& gpu = rhi::Gpu();
	m_currentFBO->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
	gpu.Clear(rhi::ClearColor);

	gpu.UseProgram(m_resolveShader);

	// Camera-rotation reprojection for sky pixels (no rasterised velocity there).
	{
		glm::mat4 projU = ctx.proj;
		projU[2][0] -= ctx.jitterNdc.x;
		projU[2][1] -= ctx.jitterNdc.y;
		glm::mat4 prevProjU = ctx.prevProj;
		prevProjU[2][0] -= ctx.prevJitterNdc.x;
		prevProjU[2][1] -= ctx.prevJitterNdc.y;
		const glm::mat4 skyReprojection = prevProjU * glm::mat4(glm::mat3(ctx.prevView)) *
			glm::inverse(glm::mat4(glm::mat3(ctx.view))) * glm::inverse(projU);
		const rhi::UniformLocation loc = gpu.GetUniformLocation(m_resolveShader, "uSkyReprojection");
		if (loc >= 0) gpu.SetUniform(loc, skyReprojection);
	}

	// Bind current frame (HDR output)
	gpu.BindTexture(0, ctx.hdrFBO->GetColorAttachment(0));
	if (m_resolveUniforms.currentFrame >= 0) gpu.SetUniform(m_resolveUniforms.currentFrame, 0);

	// Bind history
	gpu.BindTexture(1, m_historyFBO->GetColorAttachment(0));
	if (m_resolveUniforms.historyFrame >= 0) gpu.SetUniform(m_resolveUniforms.historyFrame, 1);

	// Bind velocity
	gpu.BindTexture(2, m_velocityFBO->GetColorAttachment(0));
	if (m_resolveUniforms.velocityBuffer >= 0) gpu.SetUniform(m_resolveUniforms.velocityBuffer, 2);

	// Bind depth for disocclusion detection
	gpu.BindTexture(3, ctx.gbufferFBO->GetDepthTexture());
	if (m_resolveUniforms.depthBuffer >= 0) gpu.SetUniform(m_resolveUniforms.depthBuffer, 3);

	// Bind normal for rejection
	gpu.BindTexture(4, ctx.gbufferFBO->GetColorAttachment(0));
	if (m_resolveUniforms.normalBuffer >= 0) gpu.SetUniform(m_resolveUniforms.normalBuffer, 4);

	// Bind previous-frame depth and normal for robust disocclusion checks
	gpu.BindTexture(5, m_historyDepthTex);
	if (m_resolveUniforms.historyDepthBuffer >= 0) gpu.SetUniform(m_resolveUniforms.historyDepthBuffer, 5);

	gpu.BindTexture(6, m_historyNormalTex);
	if (m_resolveUniforms.historyNormalBuffer >= 0) gpu.SetUniform(m_resolveUniforms.historyNormalBuffer, 6);

	// Upload TAA parameters
	if (m_resolveUniforms.blendFactor >= 0) gpu.SetUniform(m_resolveUniforms.blendFactor, ctx.taaBlendFactor);
	if (m_resolveUniforms.varianceThreshold >= 0) gpu.SetUniform(m_resolveUniforms.varianceThreshold, ctx.taaVarianceThreshold);
	if (m_resolveUniforms.lumaWeight >= 0) gpu.SetUniform(m_resolveUniforms.lumaWeight, ctx.taaLumaWeight);
	if (m_resolveUniforms.useYCoCg >= 0) gpu.SetUniform(m_resolveUniforms.useYCoCg, ctx.taaUseYCoCg ? 1 : 0);
	if (m_resolveUniforms.historyValid >= 0) gpu.SetUniform(m_resolveUniforms.historyValid, m_historyValid ? 1 : 0);
	if (m_resolveUniforms.screenSize >= 0) gpu.SetUniform(m_resolveUniforms.screenSize,
		glm::vec2(static_cast<float>(ctx.width), static_cast<float>(ctx.height)));

	// Enhanced quality parameters
	if (m_resolveUniforms.depthThreshold >= 0) gpu.SetUniform(m_resolveUniforms.depthThreshold, ctx.taaDepthThreshold);
	if (m_resolveUniforms.normalThreshold >= 0) gpu.SetUniform(m_resolveUniforms.normalThreshold, ctx.taaNormalThreshold);
	if (m_resolveUniforms.edgeThreshold >= 0) gpu.SetUniform(m_resolveUniforms.edgeThreshold, ctx.taaEdgeThreshold);
	if (m_resolveUniforms.reactiveMaskStrength >= 0) gpu.SetUniform(m_resolveUniforms.reactiveMaskStrength, ctx.taaReactiveMaskStrength);

	ctx.screenQuad->Render();

	gpu.BindFramebuffer(PresentTarget::Get());

	rhi::TextureRegion fullRegion;
	fullRegion.extent = glm::ivec3(ctx.width, ctx.height, 1);
	gpu.CopyTexture(m_currentFBO->GetColorAttachment(0), fullRegion,
		m_historyFBO->GetColorAttachment(0), 0, glm::ivec3(0));

	gpu.CopyTexture(ctx.gbufferFBO->GetDepthTexture(), fullRegion,
		m_historyDepthTex, 0, glm::ivec3(0));

	gpu.CopyTexture(ctx.gbufferFBO->GetColorAttachment(0), fullRegion,
		m_historyNormalTex, 0, glm::ivec3(0));

	m_historyValid = true;
	ctx.taaFBO = m_currentFBO.get();
}
