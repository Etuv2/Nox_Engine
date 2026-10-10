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
	if (m_stateSSBO && rhi::HasGpu()) rhi::Gpu().DestroyBuffer(m_stateSSBO);
	if (m_histogramSSBO && rhi::HasGpu()) rhi::Gpu().DestroyBuffer(m_histogramSSBO);
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
	rhi::Device& gpu = rhi::Gpu();
	// pre, current, EV100, average luminance, previous pre (+ padding)
	const float initialState[8] = { 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
	rhi::BufferDesc stateDesc;
	stateDesc.size = sizeof(initialState);
	stateDesc.initialData = initialState;
	stateDesc.usage = rhi::BufferUsage::DynamicDraw;
	m_stateSSBO = gpu.CreateBuffer(stateDesc);
	const uint32_t zeros[64] = {};
	rhi::BufferDesc histogramDesc;
	histogramDesc.size = sizeof(zeros);
	histogramDesc.initialData = zeros;
	histogramDesc.usage = rhi::BufferUsage::DynamicCopy;
	m_histogramSSBO = gpu.CreateBuffer(histogramDesc);
	context.exposureSSBO = m_stateSSBO;
	return true;
}

void ExposurePass::Upload(float preExposure, float exposure, float ev100)
{
	rhi::Device& gpu = rhi::Gpu();
	const float state[3] = { preExposure, exposure, ev100 };
	const float prevPre = m_lastUploadedPre > 0.0f ? m_lastUploadedPre : preExposure;
	gpu.UpdateBuffer(m_stateSSBO, 0, sizeof(state), state);
	gpu.UpdateBuffer(m_stateSSBO, 4 * sizeof(float), sizeof(float), &prevPre);
	m_lastUploadedPre = preExposure;
}

void ExposurePass::Bind(const RenderContext& ctx)
{
	if (ctx.exposureSSBO) {
		rhi::Gpu().BindStorageBuffer(kStateBinding, ctx.exposureSSBO);
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
		rhi::Device& gpu = rhi::Gpu();
		const rhi::ProgramHandle program = m_reduceShader->GetProgramID();
		gpu.UseProgram(program);
		gpu.BindStorageBuffer(kStateBinding, m_stateSSBO);
		gpu.BindStorageBuffer(kHistogramBinding, m_histogramSSBO);
		if (m_resetPending) {
			// Start from the manual EV, then snap to the metered value on the first measurement.
			gpu.SetUniform(gpu.GetUniformLocation(program, "uMode"), 2);
			const float ev = std::clamp(ctx.exposureEV100 - ctx.exposureCompensationEV,
				ctx.autoExposureMinEV100, ctx.autoExposureMaxEV100);
			gpu.SetUniform(gpu.GetUniformLocation(program, "uResetEV"), ev);
			m_resetPending = false;
			m_snapNext = true;
		}
		else {
			gpu.SetUniform(gpu.GetUniformLocation(program, "uMode"), 1);
		}
		gpu.Dispatch(1, 1, 1);
		gpu.PipelineBarrier(rhi::BarrierStorage | rhi::BarrierUniform);
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
	const rhi::TextureHandle sourceTex = sourceFBO ? sourceFBO->GetColorAttachment(0) : rhi::TextureHandle{};
	if (!sourceTex) {
		return;
	}
	const int gridX = std::max(ctx.width / kSampleDivisor, 1);
	const int gridY = std::max(ctx.height / kSampleDivisor, 1);

	rhi::Device& gpu = rhi::Gpu();
	rhi::ProgramHandle program = m_histogramShader->GetProgramID();
	gpu.UseProgram(program);
	gpu.BindStorageBuffer(kStateBinding, m_stateSSBO);
	gpu.BindStorageBuffer(kHistogramBinding, m_histogramSSBO);
	gpu.BindTexture(0, sourceTex);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uColor"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uSampleGrid"), glm::ivec2(gridX, gridY));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMinLog2Lum"), kMinLog2Lum);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uLog2LumRange"), kLog2LumRange);
	gpu.Dispatch((gridX + 15) / 16, (gridY + 15) / 16, 1);
	gpu.PipelineBarrier(rhi::BarrierStorage);

	program = m_reduceShader->GetProgramID();
	gpu.UseProgram(program);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMode"), 0);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMinLog2Lum"), kMinLog2Lum);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uLog2LumRange"), kLog2LumRange);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uLowPercentile"), std::clamp(ctx.autoExposureLowPercentile, 0.0f, 0.99f));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uHighPercentile"), std::clamp(ctx.autoExposureHighPercentile, 0.01f, 1.0f));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uCompensationEV"), ctx.exposureCompensationEV);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMinEV"), ctx.autoExposureMinEV100);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uMaxEV"), ctx.autoExposureMaxEV100);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uSpeedUp"), ctx.autoExposureSpeedUp);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uSpeedDown"), ctx.autoExposureSpeedDown);
	gpu.SetUniform(gpu.GetUniformLocation(program, "uDeltaTime"), std::clamp(ctx.deltaTime, 0.0f, 0.25f));
	gpu.SetUniform(gpu.GetUniformLocation(program, "uSnap"), m_snapNext ? 1 : 0);
	m_snapNext = false;
	gpu.Dispatch(1, 1, 1);
	gpu.PipelineBarrier(rhi::BarrierStorage);
}
