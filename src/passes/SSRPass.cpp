#include "SSRPass.h"
#include "ExposurePass.h"
#include "../ComputeShader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include <algorithm>
#include <cmath>
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
	if (!m_traceShader->CreateFromFile("shaders/ssr_trace_comp.glsl") ||
		!m_resolveShader->CreateFromFile("shaders/ssr_resolve_comp.glsl") ||
		!m_temporalShader->CreateFromFile("shaders/ssr_temporal_comp.glsl")) {
		std::cerr << "[SSRPass] Failed to compile SSR shaders" << std::endl;
		return false;
	}
	CreateTextures(context.width, context.height);
	return true;
}

void SSRPass::DestroyTextures()
{
	rhi::TextureHandle textures[] = { m_traceTex, m_resolveTex, m_hitDistTex, m_historyTex[0], m_historyTex[1], m_prevColorTex };
	for (rhi::TextureHandle& t : textures) {
		if (t && rhi::HasGpu()) rhi::Gpu().DestroyTexture(t);
	}
	m_traceTex = m_resolveTex = m_hitDistTex = m_prevColorTex = rhi::TextureHandle{};
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

	for (uint32_t unit = 0; unit < 7; ++unit) gpu.BindTexture(unit, rhi::TextureHandle{});
	m_historyIndex = current;
	m_historyValid = true;
	++m_frame;
	ctx.ssrTexture = m_historyTex[current];
}
