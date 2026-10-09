#include "ExposurePass.h"
#include "../ComputeShader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
	// Histogram covers 2^-12 .. 2^18 cd/m^2 (starlight to lamp lenses).
	constexpr float kMinLog2Lum = -12.0f;
	constexpr float kLog2LumRange = 30.0f;
	constexpr int kSampleDivisor = 4; // meter a 1/4-resolution grid
}

ExposurePass::ExposurePass() = default;

ExposurePass::~ExposurePass()
{
	if (m_stateSSBO) glDeleteBuffers(1, &m_stateSSBO);
	if (m_histogramSSBO) glDeleteBuffers(1, &m_histogramSSBO);
}

float ExposurePass::ExposureFromEV100(float ev100)
{
	return 1.0f / (1.2f * std::exp2(ev100));
}

bool ExposurePass::Initialize(RenderContext& context)
{
	m_histogramShader = std::make_unique<ComputeShader>();
	m_reduceShader = std::make_unique<ComputeShader>();
	if (!m_histogramShader->CreateFromFile("shaders/auto_exposure_histogram_comp.glsl") ||
		!m_reduceShader->CreateFromFile("shaders/auto_exposure_reduce_comp.glsl")) {
		std::cerr << "[ExposurePass] Failed to compile auto exposure shaders" << std::endl;
		return false;
	}
	// pre, current, EV100, average luminance, previous pre (+ padding)
	const float initialState[8] = { 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
	glGenBuffers(1, &m_stateSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_stateSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(initialState), initialState, GL_DYNAMIC_DRAW);
	const uint32_t zeros[64] = {};
	glGenBuffers(1, &m_histogramSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_histogramSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros), zeros, GL_DYNAMIC_COPY);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	context.exposureSSBO = m_stateSSBO;
	return true;
}

void ExposurePass::Upload(float preExposure, float exposure, float ev100)
{
	const float state[3] = { preExposure, exposure, ev100 };
	const float prevPre = m_lastUploadedPre > 0.0f ? m_lastUploadedPre : preExposure;
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_stateSSBO);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(state), state);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 4 * sizeof(float), sizeof(float), &prevPre);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	m_lastUploadedPre = preExposure;
}

void ExposurePass::Bind(const RenderContext& ctx)
{
	if (ctx.exposureSSBO) {
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kStateBinding, ctx.exposureSSBO);
	}
}

void ExposurePass::BeginFrame(RenderContext& ctx)
{
	ctx.exposureSSBO = m_stateSSBO;
	const int mode = !ctx.physicalCamera ? -1 : (ctx.exposureMode == 1 ? 1 : 0);
	if (mode != m_lastMode) {
		m_resetPending = true;
		m_lastMode = mode;
	}

	if (mode == -1) {
		// Legacy scenes: lighting is not pre-exposed; post multiplies by ctx.exposure.
		Upload(1.0f, 1.0f, 0.0f);
		m_resetPending = false;
	}
	else if (mode == 0) {
		const float ev = ctx.exposureEV100 - ctx.exposureCompensationEV;
		const float e = ExposureFromEV100(ev);
		Upload(e, e, ev);
		m_resetPending = false;
	}
	else if (m_reduceShader && m_reduceShader->IsValid()) {
		const GLuint program = m_reduceShader->GetProgramID();
		glUseProgram(program);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kStateBinding, m_stateSSBO);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kHistogramBinding, m_histogramSSBO);
		if (m_resetPending) {
			// Start from the manual EV, then snap to the metered value on the first measurement.
			glUniform1i(glGetUniformLocation(program, "uMode"), 2);
			const float ev = std::clamp(ctx.exposureEV100 - ctx.exposureCompensationEV,
				ctx.autoExposureMinEV100, ctx.autoExposureMaxEV100);
			glUniform1f(glGetUniformLocation(program, "uResetEV"), ev);
			m_resetPending = false;
			m_snapNext = true;
		}
		else {
			glUniform1i(glGetUniformLocation(program, "uMode"), 1);
		}
		glDispatchCompute(1, 1, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_UNIFORM_BARRIER_BIT);
	}
	Bind(ctx);
}

void ExposurePass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>&,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	if (!ctx.physicalCamera || ctx.exposureMode != 1 || !m_histogramShader || !m_reduceShader) {
		return;
	}
	FrameBuffer* sourceFBO = ctx.postSourceFBO ? ctx.postSourceFBO
		: ((ctx.enableTAA && ctx.taaFBO) ? ctx.taaFBO : ctx.hdrFBO.get());
	const GLuint sourceTex = sourceFBO ? sourceFBO->GetColorAttachment(0) : 0;
	if (!sourceTex) {
		return;
	}
	const int gridX = std::max(ctx.width / kSampleDivisor, 1);
	const int gridY = std::max(ctx.height / kSampleDivisor, 1);

	GLuint program = m_histogramShader->GetProgramID();
	glUseProgram(program);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kStateBinding, m_stateSSBO);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kHistogramBinding, m_histogramSSBO);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, sourceTex);
	glUniform1i(glGetUniformLocation(program, "uColor"), 0);
	glUniform2i(glGetUniformLocation(program, "uSampleGrid"), gridX, gridY);
	glUniform1f(glGetUniformLocation(program, "uMinLog2Lum"), kMinLog2Lum);
	glUniform1f(glGetUniformLocation(program, "uLog2LumRange"), kLog2LumRange);
	glDispatchCompute((gridX + 15) / 16, (gridY + 15) / 16, 1);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

	program = m_reduceShader->GetProgramID();
	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "uMode"), 0);
	glUniform1f(glGetUniformLocation(program, "uMinLog2Lum"), kMinLog2Lum);
	glUniform1f(glGetUniformLocation(program, "uLog2LumRange"), kLog2LumRange);
	glUniform1f(glGetUniformLocation(program, "uLowPercentile"), std::clamp(ctx.autoExposureLowPercentile, 0.0f, 0.99f));
	glUniform1f(glGetUniformLocation(program, "uHighPercentile"), std::clamp(ctx.autoExposureHighPercentile, 0.01f, 1.0f));
	glUniform1f(glGetUniformLocation(program, "uCompensationEV"), ctx.exposureCompensationEV);
	glUniform1f(glGetUniformLocation(program, "uMinEV"), ctx.autoExposureMinEV100);
	glUniform1f(glGetUniformLocation(program, "uMaxEV"), ctx.autoExposureMaxEV100);
	glUniform1f(glGetUniformLocation(program, "uSpeedUp"), ctx.autoExposureSpeedUp);
	glUniform1f(glGetUniformLocation(program, "uSpeedDown"), ctx.autoExposureSpeedDown);
	glUniform1f(glGetUniformLocation(program, "uDeltaTime"), std::clamp(ctx.deltaTime, 0.0f, 0.25f));
	glUniform1i(glGetUniformLocation(program, "uSnap"), m_snapNext ? 1 : 0);
	m_snapNext = false;
	glDispatchCompute(1, 1, 1);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}
