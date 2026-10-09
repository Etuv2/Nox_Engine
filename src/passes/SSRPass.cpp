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
	GLuint MakeTexture(GLenum internalFormat, int width, int height, int levels = 1) {
		GLuint tex = 0;
		glCreateTextures(GL_TEXTURE_2D, 1, &tex);
		glTextureStorage2D(tex, levels, internalFormat, std::max(width, 1), std::max(height, 1));
		glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		return tex;
	}

	void BindSampler(GLuint program, const char* name, int unit, GLuint texture) {
		glBindTextureUnit(static_cast<GLuint>(unit), texture);
		const GLint loc = glGetUniformLocation(program, name);
		if (loc >= 0) glUniform1i(loc, unit);
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
	GLuint textures[] = { m_traceTex, m_resolveTex, m_hitDistTex, m_historyTex[0], m_historyTex[1], m_prevColorTex };
	for (GLuint& t : textures) {
		if (t) glDeleteTextures(1, &t);
	}
	m_traceTex = m_resolveTex = m_hitDistTex = m_prevColorTex = 0;
	m_historyTex[0] = m_historyTex[1] = 0;
}

void SSRPass::CreateTextures(int width, int height)
{
	DestroyTextures();
	m_width = std::max(width, 1);
	m_height = std::max(height, 1);
	m_prevColorMips = 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(m_width, m_height)))));
	m_prevColorMips = std::min(m_prevColorMips, 7);
	m_traceTex = MakeTexture(GL_RGBA32F, (m_width + 1) / 2, (m_height + 1) / 2);
	glTextureParameteri(m_traceTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTextureParameteri(m_traceTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	m_resolveTex = MakeTexture(GL_RGBA16F, m_width, m_height);
	m_hitDistTex = MakeTexture(GL_R16F, m_width, m_height);
	m_historyTex[0] = MakeTexture(GL_RGBA16F, m_width, m_height);
	m_historyTex[1] = MakeTexture(GL_RGBA16F, m_width, m_height);
	m_prevColorTex = MakeTexture(GL_RGB16F, m_width, m_height, m_prevColorMips);
	const float zero[4] = { 0, 0, 0, 0 };
	glClearTexImage(m_historyTex[0], 0, GL_RGBA, GL_FLOAT, zero);
	glClearTexImage(m_historyTex[1], 0, GL_RGBA, GL_FLOAT, zero);
	m_historyValid = false;
}

void SSRPass::Resize(RenderContext& context, int newWidth, int newHeight)
{
	CreateTextures(newWidth, newHeight);
	context.ssrTexture = 0;
}

void SSRPass::SetCommonUniforms(GLuint program, const RenderContext& ctx)
{
	const glm::mat4 invProj = glm::inverse(ctx.proj);
	const glm::mat4 invView = glm::inverse(ctx.view);
	glUniformMatrix4fv(glGetUniformLocation(program, "uProj"), 1, GL_FALSE, glm::value_ptr(ctx.proj));
	glUniformMatrix4fv(glGetUniformLocation(program, "uInvProj"), 1, GL_FALSE, glm::value_ptr(invProj));
	glUniformMatrix4fv(glGetUniformLocation(program, "uView"), 1, GL_FALSE, glm::value_ptr(ctx.view));
	glUniformMatrix4fv(glGetUniformLocation(program, "uInvView"), 1, GL_FALSE, glm::value_ptr(invView));
	glUniform2f(glGetUniformLocation(program, "uFullSize"), static_cast<float>(m_width), static_cast<float>(m_height));
	glUniform1f(glGetUniformLocation(program, "uMaxRoughness"), std::clamp(ctx.ssrMaxRoughness, 0.05f, 1.0f));
}

void SSRPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>&,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	ctx.ssrTexture = 0;
	if (!ctx.enableSSR || !ctx.enableTAA || !ctx.taaFBO || !ctx.gbufferFBO || !ctx.velocityTex) {
		m_historyValid = false;
		return;
	}
	if (ctx.width != m_width || ctx.height != m_height) {
		CreateTextures(ctx.width, ctx.height);
	}
	const GLuint prevSource = ctx.taaFBO->GetColorAttachment(0);
	if (!prevSource || ctx.taaFBO->GetWidth() != m_width || ctx.taaFBO->GetHeight() != m_height) {
		return;
	}
	const GLuint depthTex = ctx.gbufferFBO->GetDepthTexture();
	const GLuint normalTex = ctx.gbufferFBO->GetColorAttachment(0);
	const GLuint clearcoatTex = ctx.gbufferFBO->GetColorAttachment(6);
	ExposurePass::Bind(ctx);

	// 1. Last frame's resolved colour with a mip chain for cone-filtered (rough) reflections.
	glCopyImageSubData(prevSource, GL_TEXTURE_2D, 0, 0, 0, 0, m_prevColorTex, GL_TEXTURE_2D, 0, 0, 0, 0, m_width, m_height, 1);
	glGenerateTextureMipmap(m_prevColorTex);

	const int halfW = (m_width + 1) / 2;
	const int halfH = (m_height + 1) / 2;

	// 2. Trace.
	GLuint program = m_traceShader->GetProgramID();
	glUseProgram(program);
	SetCommonUniforms(program, ctx);
	BindSampler(program, "uDepth", 0, depthTex);
	BindSampler(program, "uNormalRough", 1, normalTex);
	BindSampler(program, "uClearCoat", 2, clearcoatTex);
	glUniform1ui(glGetUniformLocation(program, "uFrame"), m_frame);
	glUniform1f(glGetUniformLocation(program, "uMaxDistance"), ctx.ssrMaxDistance);
	glUniform1i(glGetUniformLocation(program, "uMaxSteps"), ctx.ssrMaxSteps);
	glUniform1f(glGetUniformLocation(program, "uThickness"), ctx.ssrThickness);
	glBindImageTexture(0, m_traceTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
	glDispatchCompute((halfW + 7) / 8, (halfH + 7) / 8, 1);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	// 3. Resolve.
	program = m_resolveShader->GetProgramID();
	glUseProgram(program);
	SetCommonUniforms(program, ctx);
	BindSampler(program, "uTrace", 0, m_traceTex);
	BindSampler(program, "uDepth", 1, depthTex);
	BindSampler(program, "uNormalRough", 2, normalTex);
	BindSampler(program, "uClearCoat", 3, clearcoatTex);
	BindSampler(program, "uPrevColor", 4, m_prevColorTex);
	BindSampler(program, "uVelocity", 5, ctx.velocityTex);
	glUniform1f(glGetUniformLocation(program, "uPrevColorMaxMip"), static_cast<float>(m_prevColorMips - 1));
	glUniform1i(glGetUniformLocation(program, "uResolveSamples"), ctx.ssrResolveSamples);
	glUniform1ui(glGetUniformLocation(program, "uFrame"), m_frame);
	glBindImageTexture(0, m_resolveTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	glBindImageTexture(1, m_hitDistTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R16F);
	glDispatchCompute((m_width + 7) / 8, (m_height + 7) / 8, 1);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	// 4. Temporal.
	const int previous = m_historyIndex;
	const int current = 1 - m_historyIndex;
	glm::mat4 prevProjU = ctx.prevProj;
	prevProjU[2][0] -= ctx.prevJitterNdc.x;
	prevProjU[2][1] -= ctx.prevJitterNdc.y;
	const glm::mat4 prevViewProj = prevProjU * ctx.prevView;
	program = m_temporalShader->GetProgramID();
	glUseProgram(program);
	SetCommonUniforms(program, ctx);
	BindSampler(program, "uCurrent", 0, m_resolveTex);
	BindSampler(program, "uHitDist", 1, m_hitDistTex);
	BindSampler(program, "uHistory", 2, m_historyTex[previous]);
	BindSampler(program, "uDepth", 3, depthTex);
	BindSampler(program, "uVelocity", 4, ctx.velocityTex);
	BindSampler(program, "uNormalRough", 5, normalTex);
	BindSampler(program, "uClearCoat", 6, clearcoatTex);
	glUniformMatrix4fv(glGetUniformLocation(program, "uPrevViewProj"), 1, GL_FALSE, glm::value_ptr(prevViewProj));
	glUniform1i(glGetUniformLocation(program, "uHistoryValid"), m_historyValid ? 1 : 0);
	glUniform1f(glGetUniformLocation(program, "uBlend"), std::clamp(ctx.ssrTemporalBlend, 0.02f, 1.0f));
	glBindImageTexture(0, m_historyTex[current], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	glDispatchCompute((m_width + 7) / 8, (m_height + 7) / 8, 1);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	for (GLuint unit = 0; unit < 7; ++unit) glBindTextureUnit(unit, 0);
	m_historyIndex = current;
	m_historyValid = true;
	++m_frame;
	ctx.ssrTexture = m_historyTex[current];
}
