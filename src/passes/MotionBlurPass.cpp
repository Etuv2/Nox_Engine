#include "MotionBlurPass.h"
#include "../ComputeShader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include <algorithm>
#include <iostream>
#include <vector>
#include <glm/gtc/type_ptr.hpp>

MotionBlurPass::MotionBlurPass() = default;

MotionBlurPass::~MotionBlurPass()
{
	if (m_tileMax && rhi::HasGpu()) rhi::Gpu().DestroyTexture(m_tileMax);
	if (m_neighborMax && rhi::HasGpu()) rhi::Gpu().DestroyTexture(m_neighborMax);
}

bool MotionBlurPass::Initialize(RenderContext& context)
{
	m_tileShader = std::make_unique<ComputeShader>();
	m_gatherShader = std::make_unique<ComputeShader>();
	if (!m_tileShader->CreateFromFile("shaders/motion_blur_tile_comp.glsl") ||
		!m_gatherShader->CreateFromFile("shaders/motion_blur_gather_comp.glsl")) {
		std::cerr << "[MotionBlurPass] Failed to compile motion blur shaders" << std::endl;
		return false;
	}
	CreateTargets(context.width, context.height);
	return true;
}

void MotionBlurPass::CreateTargets(int width, int height)
{
	m_output = std::make_unique<FrameBuffer>(std::max(width, 1), std::max(height, 1),
		std::vector<rhi::Format>{ rhi::Format::RGBA16F }, false, false);
	rhi::Device& gpu = rhi::Gpu();
	if (m_tileMax) gpu.DestroyTexture(m_tileMax);
	if (m_neighborMax) gpu.DestroyTexture(m_neighborMax);
	m_tileW = (std::max(width, 1) + kTileSize - 1) / kTileSize;
	m_tileH = (std::max(height, 1) + kTileSize - 1) / kTileSize;
	for (rhi::TextureHandle* tex : { &m_tileMax, &m_neighborMax }) {
		rhi::TextureDesc desc;
		desc.type = rhi::TextureType::Tex2D;
		desc.format = rhi::Format::RG16F;
		desc.width = m_tileW;
		desc.height = m_tileH;
		desc.mipLevels = 1;
		desc.immutable = true;
		*tex = gpu.CreateTexture(desc);
		gpu.SetTextureFilter(*tex, rhi::Filter::Nearest, rhi::Filter::Nearest);
	}
}

void MotionBlurPass::Resize(RenderContext&, int newWidth, int newHeight)
{
	CreateTargets(newWidth, newHeight);
}

void MotionBlurPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>&,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	ctx.postSourceFBO = nullptr;
	FrameBuffer* source = (ctx.enableTAA && ctx.taaFBO) ? ctx.taaFBO : ctx.hdrFBO.get();
	if (!ctx.enableMotionBlur || !source || !ctx.velocityTex || !ctx.gbufferFBO) {
		return;
	}
	if (m_output->GetWidth() != ctx.width || m_output->GetHeight() != ctx.height) {
		CreateTargets(ctx.width, ctx.height);
	}
	// Physical shutter: blur covers exposureTime of motion regardless of the frame rate.
	const float frameTime = std::max(ctx.deltaTime, 1.0f / 1000.0f);
	const float velocityScale = std::clamp(ctx.motionBlurExposureTime / frameTime, 0.0f, 2.0f);
	const float maxBlur = std::max(ctx.motionBlurMaxPixels, 1.0f) * (static_cast<float>(ctx.height) / 1440.0f);
	const glm::vec2 fullSize(static_cast<float>(ctx.width), static_cast<float>(ctx.height));

	rhi::Device& gpu = rhi::Gpu();
	rhi::ProgramHandle p = m_tileShader->GetProgramID();
	gpu.UseProgram(p);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uTileSize"), kTileSize);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uFullSize"), fullSize);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uVelocityScale"), velocityScale);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uMaxBlurPixels"), maxBlur);
	gpu.BindTexture(0, ctx.velocityTex);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uVelocity"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uPass"), 0);
	gpu.BindImage(0, m_tileMax, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RG16F);
	gpu.Dispatch((m_tileW + 7) / 8, (m_tileH + 7) / 8, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);
	gpu.BindTexture(1, m_tileMax);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uTiles"), 1);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uPass"), 1);
	gpu.BindImage(0, m_neighborMax, 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RG16F);
	gpu.Dispatch((m_tileW + 7) / 8, (m_tileH + 7) / 8, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	p = m_gatherShader->GetProgramID();
	gpu.UseProgram(p);
	gpu.BindTexture(0, source->GetColorAttachment(0));
	gpu.BindTexture(1, ctx.velocityTex);
	gpu.BindTexture(2, ctx.gbufferFBO->GetDepthTexture());
	gpu.BindTexture(3, m_neighborMax);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uColor"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uVelocity"), 1);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uDepth"), 2);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uNeighborMax"), 3);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uFullSize"), fullSize);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uTileSize"), kTileSize);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uVelocityScale"), velocityScale);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uMaxBlurPixels"), maxBlur);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uSampleCount"), std::clamp(ctx.motionBlurSamples, 4, 32));
	gpu.SetUniform(gpu.GetUniformLocation(p, "uProj"), ctx.proj);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uFrame"), m_frame++);
	gpu.BindImage(0, m_output->GetColorAttachment(0), 0, false, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	gpu.Dispatch((ctx.width + 7) / 8, (ctx.height + 7) / 8, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess | rhi::BarrierFramebuffer);
	for (uint32_t unit = 0; unit < 4; ++unit) gpu.BindTexture(unit, rhi::TextureHandle{});

	ctx.postSourceFBO = m_output.get();
}
