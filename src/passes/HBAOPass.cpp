#include "HBAOPass.h"
#include "../ComputeShader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
	constexpr int kLayerCount = 16;               // 4x4 deinterleaving
	constexpr float kMaxRadiusPixels = 256.0f;    // clamp of the projected radius (full-res pixels)
	constexpr int kLocalSize = 8;

	int DivUp(int value, int divisor) {
		return (value + divisor - 1) / divisor;
	}

	float ClampResolutionScale(float scale) {
		return std::clamp(scale, 0.25f, 1.0f);
	}

	rhi::TextureHandle MakeTexture(rhi::TextureType type, rhi::Format format, int width, int height, int layers,
		rhi::Filter filter) {
		rhi::Device& gpu = rhi::Gpu();
		rhi::TextureDesc desc;
		desc.type = type;
		desc.format = format;
		desc.width = std::max(width, 1);
		desc.height = std::max(height, 1);
		desc.depth = layers;
		desc.mipLevels = 1;
		desc.immutable = true;
		const rhi::TextureHandle tex = gpu.CreateTexture(desc);
		gpu.SetTextureFilter(tex, filter, filter);
		gpu.SetTextureWrapAxis(tex, 0, rhi::Wrap::ClampToEdge);
		gpu.SetTextureWrapAxis(tex, 1, rhi::Wrap::ClampToEdge);
		return tex;
	}

	void DestroyIfValid(rhi::TextureHandle& tex) {
		if (tex && rhi::HasGpu()) rhi::Gpu().DestroyTexture(tex);
		tex = rhi::TextureHandle{};
	}
}

HBAOPass::HBAOPass() = default;

HBAOPass::~HBAOPass() {
	DestroyTextures();
}

bool HBAOPass::Initialize(RenderContext& context) {
	m_deinterleaveShader = std::make_unique<ComputeShader>();
	m_hbaoShader = std::make_unique<ComputeShader>();
	m_reinterleaveShader = std::make_unique<ComputeShader>();
	m_blurShader = std::make_unique<ComputeShader>();
	if (!m_deinterleaveShader->CreateFromFile("shaders/hbao_deinterleave_comp.glsl") ||
		!m_hbaoShader->CreateFromFile("shaders/hbao_compute_comp.glsl") ||
		!m_reinterleaveShader->CreateFromFile("shaders/hbao_reinterleave_comp.glsl") ||
		!m_blurShader->CreateFromFile("shaders/hbao_blur_comp.glsl")) {
		std::cerr << "[HBAOPass] Failed to compile HBAO shaders." << std::endl;
		return false;
	}

	CreateTextures(context.width, context.height, ClampResolutionScale(context.ssaoResolutionScale));
	return true;
}

void HBAOPass::DestroyTextures() {
	DestroyIfValid(m_linearDepthArray);
	DestroyIfValid(m_aoArray);
	DestroyIfValid(m_packedTex);
	DestroyIfValid(m_blurTex);
	DestroyIfValid(m_outputTex);
}

void HBAOPass::CreateTextures(int width, int height, float resolutionScale) {
	DestroyTextures();
	m_fullWidth = std::max(width, 1);
	m_fullHeight = std::max(height, 1);
	m_resolutionScale = ClampResolutionScale(resolutionScale);
	m_outWidth = std::max(1, static_cast<int>(std::lround(m_fullWidth * m_resolutionScale)));
	m_outHeight = std::max(1, static_cast<int>(std::lround(m_fullHeight * m_resolutionScale)));
	m_quarterWidth = DivUp(m_fullWidth, 4);
	m_quarterHeight = DivUp(m_fullHeight, 4);

	m_linearDepthArray = MakeTexture(rhi::TextureType::Tex2DArray, rhi::Format::R32F,
		m_quarterWidth, m_quarterHeight, kLayerCount, rhi::Filter::Nearest);
	m_aoArray = MakeTexture(rhi::TextureType::Tex2DArray, rhi::Format::R16F,
		m_quarterWidth, m_quarterHeight, kLayerCount, rhi::Filter::Nearest);
	m_packedTex = MakeTexture(rhi::TextureType::Tex2D, rhi::Format::RG16F,
		m_outWidth, m_outHeight, 1, rhi::Filter::Nearest);
	m_blurTex = MakeTexture(rhi::TextureType::Tex2D, rhi::Format::RG16F,
		m_outWidth, m_outHeight, 1, rhi::Filter::Nearest);
	m_outputTex = MakeTexture(rhi::TextureType::Tex2D, rhi::Format::RG16F,
		m_outWidth, m_outHeight, 1, rhi::Filter::Linear);
}

void HBAOPass::Resize(RenderContext& context, int newWidth, int newHeight) {
	CreateTextures(newWidth, newHeight, ClampResolutionScale(context.ssaoResolutionScale));
}

void HBAOPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>&,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&) {
	ctx.ssaoTexture = rhi::TextureHandle{};
	if (!ctx.enableSSAO || !ctx.gbufferFBO || ctx.width <= 0 || ctx.height <= 0) {
		return;
	}

	const float scale = ClampResolutionScale(ctx.ssaoResolutionScale);
	if (ctx.width != m_fullWidth || ctx.height != m_fullHeight || scale != m_resolutionScale) {
		CreateTextures(ctx.width, ctx.height, scale);
	}

	rhi::Device& gpu = rhi::Gpu();
	const rhi::TextureHandle depthTex = ctx.gbufferFBO->GetDepthTexture();
	const rhi::TextureHandle normalTex = ctx.gbufferFBO->GetColorAttachment(0);
	if (!depthTex || !normalTex) {
		return;
	}
	// Depth is read with texelFetch only; the compare state is the default (none).
	gpu.SetTextureCompare(depthTex, false, rhi::CompareOp::LessEqual);

	const int groupsQX = DivUp(m_quarterWidth, kLocalSize);
	const int groupsQY = DivUp(m_quarterHeight, kLocalSize);
	const glm::vec2 fullSize(static_cast<float>(m_fullWidth), static_cast<float>(m_fullHeight));
	const float focalPixels = ctx.proj[1][1] * static_cast<float>(m_fullHeight) * 0.5f;

	// 1. Linearize and deinterleave the depth into 16 quarter-res layers.
	rhi::ProgramHandle p = m_deinterleaveShader->GetProgramID();
	gpu.UseProgram(p);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uProj"), ctx.proj);
	gpu.BindTexture(0, depthTex);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uDepth"), 0);
	gpu.BindImage(0, m_linearDepthArray, 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::R32F);
	gpu.Dispatch(groupsQX, groupsQY, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	// 2. Horizon search per layer (z of the dispatch = layer).
	p = m_hbaoShader->GetProgramID();
	gpu.UseProgram(p);
	gpu.BindTexture(0, m_linearDepthArray);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uLinearDepth"), 0);
	gpu.BindTexture(1, normalTex);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uNormals"), 1);
	gpu.BindImage(0, m_aoArray, 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::R16F);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uProj"), ctx.proj);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uView"), ctx.view);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uRadius"), std::max(ctx.ssaoRadius, 1e-3f));
	gpu.SetUniform(gpu.GetUniformLocation(p, "uAngleBias"), std::clamp(ctx.ssaoBias, 0.0f, 1.2f));
	gpu.SetUniform(gpu.GetUniformLocation(p, "uStrength"), std::max(ctx.ssaoStrength, 0.0f));
	gpu.SetUniform(gpu.GetUniformLocation(p, "uPower"), std::max(ctx.ssaoPower, 0.1f));
	gpu.SetUniform(gpu.GetUniformLocation(p, "uFocalPixels"), focalPixels);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uMaxRadiusPixels"), kMaxRadiusPixels);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uDirections"), std::clamp(ctx.ssaoDirections, 4, 16));
	gpu.SetUniform(gpu.GetUniformLocation(p, "uSteps"), std::clamp(ctx.ssaoSteps, 2, 8));
	gpu.Dispatch(groupsQX, groupsQY, kLayerCount);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	// 3. Reinterleave to the output resolution, packing view depth beside the AO.
	const int groupsOX = DivUp(m_outWidth, kLocalSize);
	const int groupsOY = DivUp(m_outHeight, kLocalSize);
	p = m_reinterleaveShader->GetProgramID();
	gpu.UseProgram(p);
	gpu.BindTexture(0, m_aoArray);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uAO"), 0);
	gpu.BindTexture(1, m_linearDepthArray);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uLinearDepth"), 1);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uFullSize"), fullSize);
	gpu.BindImage(0, m_packedTex, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RG16F);
	gpu.Dispatch(groupsOX, groupsOY, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	// 4. Depth-aware separable blur: horizontal into m_blurTex, vertical into m_outputTex.
	p = m_blurShader->GetProgramID();
	gpu.UseProgram(p);
	const int blurRadius = std::clamp(ctx.ssaoBlurRadius, 1, 8);
	const float blurSharpness = std::max(ctx.ssaoBlurSharpness, 0.0f);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uRadius"), blurRadius);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uSharpness"), blurSharpness);

	gpu.BindTexture(0, m_packedTex);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uInput"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uAxis"), glm::ivec2(1, 0));
	gpu.BindImage(0, m_blurTex, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RG16F);
	gpu.Dispatch(groupsOX, groupsOY, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	gpu.BindTexture(0, m_blurTex);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uAxis"), glm::ivec2(0, 1));
	gpu.BindImage(0, m_outputTex, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RG16F);
	gpu.Dispatch(groupsOX, groupsOY, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	ctx.ssaoTexture = m_outputTex;
}
