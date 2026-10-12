#include "BloomPass.h"
#include "../PresentTarget.h"
#include "PassLogging.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <iostream>

BloomPass::BloomPass() {}

BloomPass::~BloomPass() {
	if (m_extractShader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_extractShader);
	if (m_kawaseShader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_kawaseShader);
	if (m_upsampleShader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_upsampleShader);
	if (m_dualDownShader && rhi::HasGpu()) rhi::Gpu().DestroyProgram(m_dualDownShader);
}

bool BloomPass::Initialize(RenderContext& context) {
	m_extractShader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
		"shaders/bloom_extract.glsl");
	m_kawaseShader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
		"shaders/kawase_blur.glsl");
	m_upsampleShader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
		"shaders/bloom_upsample.glsl");

	m_dualDownShader = CreateShaderProgram("shaders/fullscreen_vert.glsl",
		"shaders/bloom_downsample.glsl");

	if (!m_extractShader || !m_kawaseShader || !m_upsampleShader || !m_dualDownShader) {
		std::cerr << "[BloomPass] Failed to create shaders.\n";
		return false;
	}

	// Create framebuffers
	m_extractFBO = std::make_unique<FrameBuffer>(
		context.width, context.height,
		std::vector<rhi::Format>{ rhi::Format::RGB16F },
		false, false
	);

	for (int i = 0; i < 4; ++i) {
		int scale = 1 << i; // 1, 2, 4, 8
		m_downsampleFBO[i] = std::make_unique<FrameBuffer>(
			context.width / scale, context.height / scale,
			std::vector<rhi::Format>{ rhi::Format::RGB16F },
			false, false
		);
		m_upsampleFBO[i] = std::make_unique<FrameBuffer>(
			context.width / scale, context.height / scale,
			std::vector<rhi::Format>{ rhi::Format::RGB16F },
			false, false
		);
	}

	std::cout << "[BloomPass] Initialized successfully.\n";
	return true;
}

void BloomPass::Resize(RenderContext& context, int newWidth, int newHeight) {
	if (m_extractFBO) m_extractFBO->Resize(newWidth, newHeight);

	for (int i = 0; i < 4; ++i) {
		int scale = 1 << i;
		if (m_downsampleFBO[i]) m_downsampleFBO[i]->Resize(newWidth / scale, newHeight / scale);
		if (m_upsampleFBO[i]) m_upsampleFBO[i]->Resize(newWidth / scale, newHeight / scale);
	}
}

rhi::TextureHandle BloomPass::GetBloomResult() const {
	if (m_useHalfRes && m_halfUp[0]) {
		return m_halfUp[0]->GetColorAttachment(0);
	}
	return m_upsampleFBO[0] ? m_upsampleFBO[0]->GetColorAttachment(0) : rhi::TextureHandle{};
}

void BloomPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>& dirLight,
	const std::shared_ptr<Skybox>& skybox) {
	// Prefer temporally resolved HDR when TAA is active.
	FrameBuffer* sourceFBO = ctx.postSourceFBO ? ctx.postSourceFBO
		: ((ctx.enableTAA && ctx.taaFBO) ? ctx.taaFBO : ctx.hdrFBO.get());
	rhi::TextureHandle sourceTex = sourceFBO ? sourceFBO->GetColorAttachment(0) : rhi::TextureHandle{};
	if (!sourceTex) {
		PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[BloomPass] Missing source texture, skipping bloom.");
		return;
	}
	PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[BloomPass] Source HDR texture: " << sourceTex);

	m_useHalfRes = ctx.bloomHalfResChain;
	if (m_useHalfRes) {
		ExecuteHalfResChain(ctx, sourceTex);
		return;
	}
	ExtractBrightPixels(ctx, sourceTex);
	Downsample(ctx);
	Upsample(ctx);

	PASS_VERBOSE_LOG(m_runtimeVerboseLogging, "[BloomPass] Bloom result texture: " << GetBloomResult());
}

void BloomPass::ExtractBrightPixels(RenderContext& ctx, rhi::TextureHandle sourceTex) {
	rhi::Device& gpu = rhi::Gpu();
	m_extractFBO->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });

	//Disable depth test and blending for fullscreen quad
	gpu.SetDepthTest(false);
	gpu.SetBlendEnabled(false);

	gpu.Clear(rhi::ClearColor);

	gpu.UseProgram(m_extractShader);

	gpu.BindTexture(0, sourceTex);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "hdrBuffer"), 0);

	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "threshold"), ctx.bloomThreshold);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "knee"), ctx.bloomKnee);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uExposure"), ctx.exposure);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uBloomMode"), ctx.bloomMode);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uMaxValue"), ctx.bloomMode == 1 ? 256.0f : 64.0f);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uSourceTexel"), glm::vec2(0.0f));
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uScatterThreshold"), ctx.bloomScatterThreshold);

	ctx.screenQuad->Render();

	gpu.BindFramebuffer(PresentTarget::Get());
}

void BloomPass::Downsample(RenderContext& ctx) {
	rhi::Device& gpu = rhi::Gpu();
	//Disable depth test and blending for fullscreen quad
	gpu.SetDepthTest(false);
	gpu.SetBlendEnabled(false);

	rhi::TextureHandle srcTex = m_extractFBO->GetColorAttachment(0);

	for (int i = 0; i < 4; ++i) {
		m_downsampleFBO[i]->Bind();
		int w = m_downsampleFBO[i]->GetWidth();
		int h = m_downsampleFBO[i]->GetHeight();
		gpu.SetViewport({ 0, 0, w, h });
		gpu.Clear(rhi::ClearColor);

		gpu.UseProgram(m_kawaseShader);
		gpu.SetUniform(gpu.GetUniformLocation(m_kawaseShader, "pass"), i);

		gpu.BindTexture(0, srcTex);
		gpu.SetUniform(gpu.GetUniformLocation(m_kawaseShader, "image"), 0);

		glm::vec2 texelSize(1.0f / w, 1.0f / h);
		gpu.SetUniform(gpu.GetUniformLocation(m_kawaseShader, "texelSize"), texelSize);

		ctx.screenQuad->Render();

		srcTex = m_downsampleFBO[i]->GetColorAttachment(0);
	}

	gpu.BindFramebuffer(PresentTarget::Get());
}

void BloomPass::Upsample(RenderContext& ctx) {
	rhi::Device& gpu = rhi::Gpu();
	//Disable depth test and blending for fullscreen quad
	gpu.SetDepthTest(false);
	gpu.SetBlendEnabled(false);

	rhi::TextureHandle srcTex = m_downsampleFBO[3]->GetColorAttachment(0); // Smallest

	for (int i = 2; i >= 0; --i) {
		m_upsampleFBO[i]->Bind();
		int w = m_upsampleFBO[i]->GetWidth();
		int h = m_upsampleFBO[i]->GetHeight();
		gpu.SetViewport({ 0, 0, w, h });
		gpu.Clear(rhi::ClearColor);

		gpu.UseProgram(m_upsampleShader);
		gpu.SetUniform(gpu.GetUniformLocation(m_upsampleShader, "uNormalize"), ctx.bloomMode == 1 ? 1 : 0);

		// Low-res input (previous upsample)
		gpu.BindTexture(0, srcTex);
		gpu.SetUniform(gpu.GetUniformLocation(m_upsampleShader, "lowResTex"), 0);

		// High-res input (current downsample level)
		gpu.BindTexture(1, m_downsampleFBO[i]->GetColorAttachment(0));
		gpu.SetUniform(gpu.GetUniformLocation(m_upsampleShader, "highResTex"), 1);

		ctx.screenQuad->Render();

		srcTex = m_upsampleFBO[i]->GetColorAttachment(0);
	}

	gpu.BindFramebuffer(PresentTarget::Get());
}

void BloomPass::EnsureHalfResTargets(int width, int height) {
	if (m_halfLevel[0] && m_halfWidth == width && m_halfHeight == height) {
		return;
	}
	m_halfWidth = width;
	m_halfHeight = height;
	for (int i = 0; i < 3; ++i) {
		const int scale = 2 << i; // 2, 4, 8
		m_halfLevel[i] = std::make_unique<FrameBuffer>(std::max(width / scale, 1), std::max(height / scale, 1),
			std::vector<rhi::Format>{ rhi::Format::RGB16F }, false, false);
	}
	for (int i = 0; i < 2; ++i) {
		const int scale = 2 << i; // 2, 4
		m_halfUp[i] = std::make_unique<FrameBuffer>(std::max(width / scale, 1), std::max(height / scale, 1),
			std::vector<rhi::Format>{ rhi::Format::RGB16F }, false, false);
	}
}

void BloomPass::ExecuteHalfResChain(RenderContext& ctx, rhi::TextureHandle sourceTex) {
	EnsureHalfResTargets(ctx.width, ctx.height);
	rhi::Device& gpu = rhi::Gpu();
	gpu.SetDepthTest(false);
	gpu.SetBlendEnabled(false);
	auto bindTarget = [&](FrameBuffer& fb) {
		fb.Bind();
		gpu.SetViewport({ 0, 0, fb.GetWidth(), fb.GetHeight() });
	};

	// 1. Extract at 1/2 with a 13-tap prefilter of the full-resolution source.
	bindTarget(*m_halfLevel[0]);
	gpu.UseProgram(m_extractShader);
	gpu.BindTexture(0, sourceTex);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "hdrBuffer"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "threshold"), ctx.bloomThreshold);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "knee"), ctx.bloomKnee);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uExposure"), ctx.exposure);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uBloomMode"), ctx.bloomMode);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uMaxValue"), ctx.bloomMode == 1 ? 256.0f : 64.0f);
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uSourceTexel"),
		glm::vec2(1.0f / static_cast<float>(std::max(ctx.width, 1)), 1.0f / static_cast<float>(std::max(ctx.height, 1))));
	gpu.SetUniform(gpu.GetUniformLocation(m_extractShader, "uScatterThreshold"), ctx.bloomScatterThreshold);
	ctx.screenQuad->Render();

	// 2. Dual-filter downsample 1/2 -> 1/4 -> 1/8.
	gpu.UseProgram(m_dualDownShader);
	gpu.SetUniform(gpu.GetUniformLocation(m_dualDownShader, "image"), 0);
	for (int i = 1; i < 3; ++i) {
		bindTarget(*m_halfLevel[i]);
		gpu.BindTexture(0, m_halfLevel[i - 1]->GetColorAttachment(0));
		ctx.screenQuad->Render();
	}

	// 3. Tent upsample 1/8 -> 1/4 -> 1/2, adding each level (same weights as the full chain).
	gpu.UseProgram(m_upsampleShader);
	gpu.SetUniform(gpu.GetUniformLocation(m_upsampleShader, "uNormalize"), ctx.bloomMode == 1 ? 1 : 0);
	gpu.SetUniform(gpu.GetUniformLocation(m_upsampleShader, "lowResTex"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(m_upsampleShader, "highResTex"), 1);
	rhi::TextureHandle low = m_halfLevel[2]->GetColorAttachment(0);
	for (int i = 1; i >= 0; --i) {
		bindTarget(*m_halfUp[i]);
		gpu.BindTexture(0, low);
		gpu.BindTexture(1, m_halfLevel[i]->GetColorAttachment(0));
		ctx.screenQuad->Render();
		low = m_halfUp[i]->GetColorAttachment(0);
	}
	gpu.BindTexture(1, rhi::TextureHandle{});
	gpu.BindFramebuffer(PresentTarget::Get());
}
