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
	if (m_tileMax) glDeleteTextures(1, &m_tileMax);
	if (m_neighborMax) glDeleteTextures(1, &m_neighborMax);
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
		std::vector<GLenum>{ GL_RGBA16F }, false, false);
	if (m_tileMax) glDeleteTextures(1, &m_tileMax);
	if (m_neighborMax) glDeleteTextures(1, &m_neighborMax);
	m_tileW = (std::max(width, 1) + kTileSize - 1) / kTileSize;
	m_tileH = (std::max(height, 1) + kTileSize - 1) / kTileSize;
	for (GLuint* tex : { &m_tileMax, &m_neighborMax }) {
		glCreateTextures(GL_TEXTURE_2D, 1, tex);
		glTextureStorage2D(*tex, 1, GL_RG16F, m_tileW, m_tileH);
		glTextureParameteri(*tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTextureParameteri(*tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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

	GLuint p = m_tileShader->GetProgramID();
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "uTileSize"), kTileSize);
	glUniform2fv(glGetUniformLocation(p, "uFullSize"), 1, glm::value_ptr(fullSize));
	glUniform1f(glGetUniformLocation(p, "uVelocityScale"), velocityScale);
	glUniform1f(glGetUniformLocation(p, "uMaxBlurPixels"), maxBlur);
	glBindTextureUnit(0, ctx.velocityTex);
	glUniform1i(glGetUniformLocation(p, "uVelocity"), 0);
	glUniform1i(glGetUniformLocation(p, "uPass"), 0);
	glBindImageTexture(0, m_tileMax, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
	glDispatchCompute((m_tileW + 7) / 8, (m_tileH + 7) / 8, 1);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	glBindTextureUnit(1, m_tileMax);
	glUniform1i(glGetUniformLocation(p, "uTiles"), 1);
	glUniform1i(glGetUniformLocation(p, "uPass"), 1);
	glBindImageTexture(0, m_neighborMax, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
	glDispatchCompute((m_tileW + 7) / 8, (m_tileH + 7) / 8, 1);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	p = m_gatherShader->GetProgramID();
	glUseProgram(p);
	glBindTextureUnit(0, source->GetColorAttachment(0));
	glBindTextureUnit(1, ctx.velocityTex);
	glBindTextureUnit(2, ctx.gbufferFBO->GetDepthTexture());
	glBindTextureUnit(3, m_neighborMax);
	glUniform1i(glGetUniformLocation(p, "uColor"), 0);
	glUniform1i(glGetUniformLocation(p, "uVelocity"), 1);
	glUniform1i(glGetUniformLocation(p, "uDepth"), 2);
	glUniform1i(glGetUniformLocation(p, "uNeighborMax"), 3);
	glUniform2fv(glGetUniformLocation(p, "uFullSize"), 1, glm::value_ptr(fullSize));
	glUniform1i(glGetUniformLocation(p, "uTileSize"), kTileSize);
	glUniform1f(glGetUniformLocation(p, "uVelocityScale"), velocityScale);
	glUniform1f(glGetUniformLocation(p, "uMaxBlurPixels"), maxBlur);
	glUniform1i(glGetUniformLocation(p, "uSampleCount"), std::clamp(ctx.motionBlurSamples, 4, 32));
	glUniformMatrix4fv(glGetUniformLocation(p, "uProj"), 1, GL_FALSE, glm::value_ptr(ctx.proj));
	glUniform1ui(glGetUniformLocation(p, "uFrame"), m_frame++);
	glBindImageTexture(0, m_output->GetColorAttachment(0), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	glDispatchCompute((ctx.width + 7) / 8, (ctx.height + 7) / 8, 1);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
	for (GLuint unit = 0; unit < 4; ++unit) glBindTextureUnit(unit, 0);

	ctx.postSourceFBO = m_output.get();
}
