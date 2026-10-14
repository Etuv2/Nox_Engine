#include "SSRPass.h"
#include "ExposurePass.h"
#include "../ComputeShader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

namespace {
	rhi::TextureHandle MakeTexture(rhi::Format format, int width, int height, int levels = 1) {
		rhi::Device& gpu = rhi::Gpu();
		rhi::TextureDesc desc;
		desc.type = rhi::TextureType::Tex2D;
		desc.format = format;
		desc.width = std::max(width, 1);
		desc.height = std::max(height, 1);
		desc.mipLevels = levels;
		desc.immutable = true;
		const rhi::TextureHandle tex = gpu.CreateTexture(desc);
		gpu.SetTextureMinFilter(tex, levels > 1 ? rhi::Filter::LinearMipLinear : rhi::Filter::Linear);
		gpu.SetTextureMagFilter(tex, rhi::Filter::Linear);
		gpu.SetTextureWrapAxis(tex, 0, rhi::Wrap::ClampToEdge);
		gpu.SetTextureWrapAxis(tex, 1, rhi::Wrap::ClampToEdge);
		return tex;
	}

	// Size of one mip level of a texture whose base level is `base` texels wide.
	int LevelSize(int base, int level) {
		return std::max(1, base >> level);
	}

	// NOX_SSR_DEBUG_VIEW: 1 = raw resolve, 2 = temporal output (the blur chain's input), 3 = composite,
	// 4 = chosen blur level as a colour ramp, 5 = chosen level as grey (L + 2) / 8 (exact, misses = 0).
// Non-zero values replace the lit image with the SSR signal.
	int DebugView() {
		static const int view = [] {
			const char* v = std::getenv("NOX_SSR_DEBUG_VIEW");
			return v ? std::atoi(v) : 0;
		}();
		return view;
	}

	void BindSampler(rhi::ProgramHandle program, const char* name, int unit, rhi::TextureHandle texture) {
		rhi::Device& gpu = rhi::Gpu();
		gpu.BindTexture(static_cast<uint32_t>(unit), texture);
		const rhi::UniformLocation loc = gpu.GetUniformLocation(program, name);
		if (loc >= 0) gpu.SetUniform(loc, unit);
	}
}

SSRPass::SSRPass() = default;

SSRPass::~SSRPass()
{
	DestroyTextures();
}

bool SSRPass::Initialize(RenderContext& context)
{
	m_traceShader = std::make_unique<ComputeShader>();
	m_resolveShader = std::make_unique<ComputeShader>();
	m_temporalShader = std::make_unique<ComputeShader>();
	m_blurDownShader = std::make_unique<ComputeShader>();
	m_blurUpShader = std::make_unique<ComputeShader>();
	m_compositeShader = std::make_unique<ComputeShader>();
	if (!m_traceShader->CreateFromFile("shaders/ssr_trace_comp.glsl") ||
		!m_resolveShader->CreateFromFile("shaders/ssr_resolve_comp.glsl") ||
		!m_temporalShader->CreateFromFile("shaders/ssr_temporal_comp.glsl") ||
		!m_blurDownShader->CreateFromFile("shaders/ssr_blur_down_comp.glsl") ||
		!m_blurUpShader->CreateFromFile("shaders/ssr_blur_up_comp.glsl") ||
		!m_compositeShader->CreateFromFile("shaders/ssr_composite_comp.glsl")) {
		std::cerr << "[SSRPass] Failed to compile SSR shaders" << std::endl;
		return false;
	}
	CreateTextures(context.width, context.height);
	return true;
}

void SSRPass::DestroyTextures()
{
	rhi::TextureHandle textures[] = { m_traceTex, m_resolveTex, m_hitDistTex, m_historyTex[0], m_historyTex[1], m_prevColorTex,
		m_blurChainTex, m_compositeTex };
	for (rhi::TextureHandle& t : textures) {
		if (t && rhi::HasGpu()) rhi::Gpu().DestroyTexture(t);
	}
	m_traceTex = m_resolveTex = m_hitDistTex = m_prevColorTex = rhi::TextureHandle{};
	m_blurChainTex = m_compositeTex = rhi::TextureHandle{};
	m_historyTex[0] = m_historyTex[1] = rhi::TextureHandle{};
}

void SSRPass::CreateTextures(int width, int height)
{
	DestroyTextures();
	m_width = std::max(width, 1);
	m_height = std::max(height, 1);
	m_prevColorMips = 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(m_width, m_height)))));
	m_prevColorMips = std::min(m_prevColorMips, 7);
	rhi::Device& gpu = rhi::Gpu();
	m_traceTex = MakeTexture(rhi::Format::RGBA32F, (m_width + 1) / 2, (m_height + 1) / 2);
	gpu.SetTextureMinFilter(m_traceTex, rhi::Filter::Nearest);
	gpu.SetTextureMagFilter(m_traceTex, rhi::Filter::Nearest);
	m_resolveTex = MakeTexture(rhi::Format::RGBA16F, m_width, m_height);
	m_hitDistTex = MakeTexture(rhi::Format::R16F, m_width, m_height);
	m_historyTex[0] = MakeTexture(rhi::Format::RGBA16F, m_width, m_height);
	m_historyTex[1] = MakeTexture(rhi::Format::RGBA16F, m_width, m_height);
	m_prevColorTex = MakeTexture(rhi::Format::RGB16F, m_width, m_height, m_prevColorMips);
	m_blurChainTex = MakeTexture(rhi::Format::RGBA16F, (m_width + 1) / 2, (m_height + 1) / 2, kBlurLevels);
	m_compositeTex = MakeTexture(rhi::Format::RGBA16F, m_width, m_height);
	const float zero[4] = { 0, 0, 0, 0 };
	gpu.ClearTexture(m_historyTex[0], 0, rhi::PixelFormat::RGBA, rhi::PixelType::Float, zero);
	gpu.ClearTexture(m_historyTex[1], 0, rhi::PixelFormat::RGBA, rhi::PixelType::Float, zero);
	m_historyValid = false;
}

void SSRPass::Resize(RenderContext& context, int newWidth, int newHeight)
{
	CreateTextures(newWidth, newHeight);
	context.ssrTexture = rhi::TextureHandle{};
}

void SSRPass::SetCommonUniforms(rhi::ProgramHandle program, const RenderContext& ctx)
{
	rhi::Device& gpu = rhi::Gpu();
	const glm::mat4 invProj = glm::inverse(ctx.proj);
	const glm::mat4 invView = glm::inverse(ctx.view);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uProj"), ctx.proj);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uInvProj"), invProj);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uView"), ctx.view);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uInvView"), invView);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uFullSize"), glm::vec2(static_cast<float>(m_width), static_cast<float>(m_height)));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMaxRoughness"), std::clamp(ctx.ssrMaxRoughness, 0.05f, 1.0f));
}

void SSRPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>&,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	ctx.ssrTexture = rhi::TextureHandle{};
	ctx.ssrDebugView = DebugView();
	if (!ctx.enableSSR || !ctx.enableTAA || !ctx.taaFBO || !ctx.gbufferFBO || !ctx.velocityTex) {
		m_historyValid = false;
		return;
	}
	if (ctx.width != m_width || ctx.height != m_height) {
		CreateTextures(ctx.width, ctx.height);
	}
	const rhi::TextureHandle prevSource = ctx.taaFBO->GetColorAttachment(0);
	if (!prevSource || ctx.taaFBO->GetWidth() != m_width || ctx.taaFBO->GetHeight() != m_height) {
		return;
	}
	const rhi::TextureHandle depthTex = ctx.gbufferFBO->GetDepthTexture();
	const rhi::TextureHandle normalTex = ctx.gbufferFBO->GetColorAttachment(0);
	const rhi::TextureHandle clearcoatTex = ctx.gbufferFBO->GetColorAttachment(6);
	ExposurePass::Bind(ctx);

	rhi::Device& gpu = rhi::Gpu();

	// 1. Last frame's resolved colour with a mip chain for cone-filtered (rough) reflections.
	rhi::TextureRegion copyRegion;
	copyRegion.extent = glm::ivec3(m_width, m_height, 1);
	gpu.CopyTexture(prevSource, copyRegion, m_prevColorTex, 0, glm::ivec3(0));
	gpu.GenerateMipmaps(m_prevColorTex);

	const int halfW = (m_width + 1) / 2;
	const int halfH = (m_height + 1) / 2;

	// 2. Trace.
	rhi::ProgramHandle program = m_traceShader->GetProgramID();
	gpu.UseProgram(program);
	SetCommonUniforms(program, ctx);
	BindSampler(program, "uDepth", 0, depthTex);
	BindSampler(program, "uNormalRough", 1, normalTex);
	BindSampler(program, "uClearCoat", 2, clearcoatTex);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uFrame"), m_frame);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMaxDistance"), ctx.ssrMaxDistance);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMaxSteps"), ctx.ssrMaxSteps);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uThickness"), ctx.ssrThickness);
	gpu.BindImage(0, m_traceTex, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA32F);
	gpu.Dispatch((halfW + 7) / 8, (halfH + 7) / 8, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	// 3. Resolve.
	program = m_resolveShader->GetProgramID();
	gpu.UseProgram(program);
	SetCommonUniforms(program, ctx);
	BindSampler(program, "uTrace", 0, m_traceTex);
	BindSampler(program, "uDepth", 1, depthTex);
	BindSampler(program, "uNormalRough", 2, normalTex);
	BindSampler(program, "uClearCoat", 3, clearcoatTex);
	BindSampler(program, "uPrevColor", 4, m_prevColorTex);
	BindSampler(program, "uVelocity", 5, ctx.velocityTex);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uPrevColorMaxMip"), static_cast<float>(m_prevColorMips - 1));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uResolveSamples"), ctx.ssrResolveSamples);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uEdgeFade"), std::clamp(ctx.ssrEdgeFade, 0.0001f, 0.5f));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uFrame"), m_frame);
	gpu.BindImage(0, m_resolveTex, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	gpu.BindImage(1, m_hitDistTex, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::R16F);
	gpu.Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	// 4. Temporal.
	const int previous = m_historyIndex;
	const int current = 1 - m_historyIndex;
	glm::mat4 prevProjU = ctx.prevProj;
	prevProjU[2][0] -= ctx.prevJitterNdc.x;
	prevProjU[2][1] -= ctx.prevJitterNdc.y;
	const glm::mat4 prevViewProj = prevProjU * ctx.prevView;
	program = m_temporalShader->GetProgramID();
	gpu.UseProgram(program);
	SetCommonUniforms(program, ctx);
	BindSampler(program, "uCurrent", 0, m_resolveTex);
	BindSampler(program, "uHitDist", 1, m_hitDistTex);
	BindSampler(program, "uHistory", 2, m_historyTex[previous]);
	BindSampler(program, "uDepth", 3, depthTex);
	BindSampler(program, "uVelocity", 4, ctx.velocityTex);
	BindSampler(program, "uNormalRough", 5, normalTex);
	BindSampler(program, "uClearCoat", 6, clearcoatTex);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uPrevViewProj"), prevViewProj);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uHistoryValid"), m_historyValid ? 1 : 0);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uBlend"), std::clamp(ctx.ssrTemporalBlend, 0.02f, 1.0f));
	gpu.BindImage(0, m_historyTex[current], 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	gpu.Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	// 5. Glossy blur chain (dual-filter Kawase). Level 0 is the half-resolution premultiplied
	// history (rgb*a, a); each level below is a 5-tap downsample of the one above. The up pass then
	// walks back up with 8-tap filters, one level per dispatch (barrier between dispatches).
	const int halfWidth = (m_width + 1) / 2;
	const int halfHeight = (m_height + 1) / 2;
	program = m_blurDownShader->GetProgramID();
	gpu.UseProgram(program);
	for (int level = 0; level < kBlurLevels; ++level) {
		const bool fromHistory = level == 0;
		BindSampler(program, "uSource", 0, fromHistory ? m_historyTex[current] : m_blurChainTex);
		gpu.SetUniform(gpu.GetUniformLocation(program, "uSourceLod"), static_cast<float>(fromHistory ? 0 : level - 1));
		gpu.SetUniform(gpu.GetUniformLocation(program, "uPremultiply"), fromHistory ? 1 : 0);
		gpu.BindImage(0, m_blurChainTex, level, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		gpu.Dispatch((LevelSize(halfWidth, level) + 7) / 8, (LevelSize(halfHeight, level) + 7) / 8, 1);
		gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);
	}
	program = m_blurUpShader->GetProgramID();
	gpu.UseProgram(program);
	for (int level = kBlurLevels - 2; level >= 0; --level) {
		BindSampler(program, "uChain", 0, m_blurChainTex);
		gpu.SetUniform(gpu.GetUniformLocation(program, "uUpperLod"), static_cast<float>(level + 1));
		gpu.SetUniform(gpu.GetUniformLocation(program, "uOwnLod"), static_cast<float>(level));
		gpu.BindImage(0, m_blurChainTex, level, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
		gpu.Dispatch((LevelSize(halfWidth, level) + 7) / 8, (LevelSize(halfHeight, level) + 7) / 8, 1);
		gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);
	}

	// 6. Composite: pick the blur level per pixel from roughness and hit distance, un-premultiply.
	program = m_compositeShader->GetProgramID();
	gpu.UseProgram(program);
	SetCommonUniforms(program, ctx);
	BindSampler(program, "uHistory", 0, m_historyTex[current]);
	BindSampler(program, "uChain", 1, m_blurChainTex);
	BindSampler(program, "uHitDist", 2, m_hitDistTex);
	BindSampler(program, "uDepth", 3, depthTex);
	BindSampler(program, "uNormalRough", 4, normalTex);
	BindSampler(program, "uClearCoat", 5, clearcoatTex);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uBlurStrength"), std::clamp(ctx.ssrBlurStrength, 0.0f, 4.0f));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMaxBlurLevel"),
		static_cast<float>(std::clamp(ctx.ssrMaxBlurLevel, 0, kBlurLevels - 1)));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uDebugLevel"), ctx.ssrDebugView == 4 ? 1 : ctx.ssrDebugView == 5 ? 2 : 0);
	gpu.BindImage(0, m_compositeTex, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	gpu.Dispatch((m_width + 7) / 8, (m_height + 7) / 8, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	for (uint32_t unit = 0; unit < 7; ++unit) gpu.BindTexture(unit, rhi::TextureHandle{});
	m_historyIndex = current;
	m_historyValid = true;
	++m_frame;
	switch (ctx.ssrDebugView) {
	case 1: ctx.ssrTexture = m_resolveTex; break;
	case 2: ctx.ssrTexture = m_historyTex[current]; break;
	default: ctx.ssrTexture = m_compositeTex; break;
	}
}
