#include "VolumetricFogPass.h"
#include "../PresentTarget.h"
#include "LightClusterPass.h"
#include "ExposurePass.h"
#include "../ComputeShader.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../ScreenQuad.h"
#include "../RenderContext.h"
#include "../LightManager.h"
#include "../Camera.h"
#include "../TextureUnits.h"
#include <algorithm>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

namespace {
	rhi::TextureHandle MakeVolume(int x, int y, int z) {
		rhi::Device& gpu = rhi::Gpu();
		rhi::TextureDesc desc;
		desc.type = rhi::TextureType::Tex3D;
		desc.format = rhi::Format::RGBA16F;
		desc.width = std::max(x, 1);
		desc.height = std::max(y, 1);
		desc.depth = std::max(z, 1);
		desc.mipLevels = 1;
		desc.immutable = true;
		const rhi::TextureHandle tex = gpu.CreateTexture(desc);
		gpu.SetTextureFilter(tex, rhi::Filter::Linear, rhi::Filter::Linear);
		gpu.SetTextureWrap(tex, rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge);
		return tex;
	}

	void Set1f(rhi::ProgramHandle p, const char* n, float v) { const rhi::UniformLocation l = rhi::Gpu().GetUniformLocation(p, n); if (l >= 0) rhi::Gpu().SetUniform(l, v); }
	void Set1i(rhi::ProgramHandle p, const char* n, int v) { const rhi::UniformLocation l = rhi::Gpu().GetUniformLocation(p, n); if (l >= 0) rhi::Gpu().SetUniform(l, v); }
	void Set3f(rhi::ProgramHandle p, const char* n, const glm::vec3& v) { const rhi::UniformLocation l = rhi::Gpu().GetUniformLocation(p, n); if (l >= 0) rhi::Gpu().SetUniform(l, v); }
	void SetM4(rhi::ProgramHandle p, const char* n, const glm::mat4& m) { const rhi::UniformLocation l = rhi::Gpu().GetUniformLocation(p, n); if (l >= 0) rhi::Gpu().SetUniform(l, m); }
}

VolumetricFogPass::VolumetricFogPass() = default;

VolumetricFogPass::~VolumetricFogPass()
{
	if (!rhi::HasGpu()) return;
	DestroyVolumes();
	if (m_applyShader) rhi::Gpu().DestroyProgram(m_applyShader);
}

bool VolumetricFogPass::Initialize(RenderContext& context)
{
	m_injectShader = std::make_unique<ComputeShader>();
	m_integrateShader = std::make_unique<ComputeShader>();
	if (!m_injectShader->CreateFromFile("shaders/volumetric_inject_comp.glsl") ||
		!m_integrateShader->CreateFromFile("shaders/volumetric_integrate_comp.glsl")) {
		std::cerr << "[VolumetricFogPass] Failed to compile volumetric compute shaders" << std::endl;
		return false;
	}
	m_applyShader = CreateShaderProgram("shaders/fullscreen_vert.glsl", "shaders/volumetric_apply_frag.glsl");
	if (!m_applyShader) {
		std::cerr << "[VolumetricFogPass] Failed to create apply shader" << std::endl;
		return false;
	}
	CreateVolumes(context.width, context.height, context.volumetricTileSize, context.volumetricSlices);
	return true;
}

void VolumetricFogPass::DestroyVolumes()
{
	rhi::Device& gpu = rhi::Gpu();
	rhi::TextureHandle t[] = { m_scattering[0], m_scattering[1], m_integrated };
	for (rhi::TextureHandle& tex : t) if (tex) gpu.DestroyTexture(tex);
	m_scattering[0] = m_scattering[1] = m_integrated = {};
}

void VolumetricFogPass::CreateVolumes(int width, int height, int tileSize, int slices)
{
	DestroyVolumes();
	m_tileSize = std::clamp(tileSize, 4, 32);
	m_sizeX = (std::max(width, 1) + m_tileSize - 1) / m_tileSize;
	m_sizeY = (std::max(height, 1) + m_tileSize - 1) / m_tileSize;
	m_sizeZ = std::clamp(slices, 16, 128);
	m_scattering[0] = MakeVolume(m_sizeX, m_sizeY, m_sizeZ);
	m_scattering[1] = MakeVolume(m_sizeX, m_sizeY, m_sizeZ);
	m_integrated = MakeVolume(m_sizeX, m_sizeY, m_sizeZ);
	m_historyValid = false;
}

void VolumetricFogPass::Resize(RenderContext& context, int newWidth, int newHeight)
{
	CreateVolumes(newWidth, newHeight, context.volumetricTileSize, context.volumetricSlices);
}

void VolumetricFogPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	m_executedThisFrame = false;
	if (!ctx.enableVolumetricFog || !camera || !ctx.lightManager) {
		m_historyValid = false;
		return;
	}
	if (ctx.volumetricTileSize != m_tileSize || ctx.volumetricSlices != m_sizeZ ||
		m_sizeX != (ctx.width + m_tileSize - 1) / m_tileSize || m_sizeY != (ctx.height + m_tileSize - 1) / m_tileSize) {
		CreateVolumes(ctx.width, ctx.height, ctx.volumetricTileSize, ctx.volumetricSlices);
	}

	rhi::Device& gpu = rhi::Gpu();
	glm::mat4 projU = ctx.proj;
	projU[2][0] -= ctx.jitterNdc.x;
	projU[2][1] -= ctx.jitterNdc.y;
	glm::mat4 prevProjU = ctx.prevProj;
	prevProjU[2][0] -= ctx.prevJitterNdc.x;
	prevProjU[2][1] -= ctx.prevJitterNdc.y;
	const glm::mat4 invViewProj = glm::inverse(projU * ctx.view);
	const glm::mat4 prevViewProj = prevProjU * ctx.prevView;
	const float volumeNear = std::max(camera->GetCameraNearPlane(), 0.05f);
	const float volumeFar = std::max(ctx.volumetricFar, volumeNear * 4.0f);
	m_volumeNear = volumeNear;
	m_volumeFar = volumeFar;

	const int previous = m_current;
	const int current = 1 - m_current;

	// 1. Inject.
	rhi::ProgramHandle p = m_injectShader->GetProgramID();
	gpu.UseProgram(p);
	LightClusterPass::BindForShading(ctx);
	ExposurePass::Bind(ctx);
	gpu.BindStorageBuffer(0, ctx.lightManager->GetLightDataSSBO());
	gpu.BindStorageBuffer(1, ctx.lightManager->GetShadowMatricesSSBO());
	Set1i(p, "numLights", ctx.lightManager->GetActiveLightCount());
	const rhi::TextureHandle shadowArray = ctx.lightManager->GetShadowArrayTexture();
	const bool shadows = ctx.enableShadows && shadowArray.IsValid() && gpu.IsTexture(shadowArray);
	if (shadows) {
		gpu.BindTexture(TextureUnits::SHADOW_MAP_ARRAY, shadowArray);
	}
	Set1i(p, "multiLightShadowArray", TextureUnits::SHADOW_MAP_ARRAY);
	Set1i(p, "enableShadows", shadows ? 1 : 0);
	Set1f(p, "shadowBias", ctx.shadowBias);
	{
		const float nearPlane = std::max(ctx.shadowNear, camera->GetCameraNearPlane());
		const float farPlane = std::max(nearPlane + 1.0f, std::min(ctx.shadowFar, camera->GetCameraFarPlane()));
		const glm::vec4 splits = ctx.lightManager->ComputeCascadeSplitVector(nearPlane, farPlane);
		const rhi::UniformLocation loc = gpu.GetUniformLocation(p, "cascadeSplits");
		if (loc >= 0) gpu.SetUniform(loc, splits);
	}
	SetM4(p, "view", ctx.view);
	SetM4(p, "uInvViewProj", invViewProj);
	SetM4(p, "uPrevViewProj", prevViewProj);
	Set3f(p, "uCameraPos", camera->GetCameraPosition());
	gpu.SetUniform(gpu.GetUniformLocation(p, "uScreenSize"), glm::vec2(static_cast<float>(ctx.width), static_cast<float>(ctx.height)));
	gpu.SetUniform(gpu.GetUniformLocation(p, "uFrame"), m_frame);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uVolumeSize"), glm::ivec3(m_sizeX, m_sizeY, m_sizeZ));
	Set1f(p, "uVolumeNear", volumeNear);
	Set1f(p, "uVolumeFar", volumeFar);
	Set1f(p, "uFogDensity", ctx.fogDensity);
	Set1f(p, "uFogBaseHeight", ctx.fogBaseHeight);
	Set1f(p, "uFogHeightFalloff", ctx.fogHeightFalloff);
	Set3f(p, "uFogAlbedo", ctx.fogAlbedo);
	Set1f(p, "uFogAnisotropy", std::clamp(ctx.fogAnisotropy, -0.95f, 0.95f));
	Set1f(p, "uFogNoiseStrength", ctx.fogNoiseStrength);
	Set1f(p, "uFogNoiseScale", ctx.fogNoiseScale);
	Set3f(p, "uFogWind", ctx.fogWind);
	Set1f(p, "uTime", ctx.timeSeconds);
	Set3f(p, "uAmbientRadiance", ctx.fogAmbientRadiance);
	Set1f(p, "uTemporalBlend", std::clamp(ctx.fogTemporalBlend, 0.02f, 1.0f));
	Set1i(p, "uHistoryValid", m_historyValid ? 1 : 0);
	gpu.BindTexture(0, m_scattering[previous]);
	Set1i(p, "uHistoryVolume", 0);
	gpu.BindImage(0, m_scattering[current], 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	gpu.Dispatch((m_sizeX + 7) / 8, (m_sizeY + 7) / 8, m_sizeZ);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);

	// 2. Integrate.
	p = m_integrateShader->GetProgramID();
	gpu.UseProgram(p);
	gpu.SetUniform(gpu.GetUniformLocation(p, "uVolumeSize"), glm::ivec3(m_sizeX, m_sizeY, m_sizeZ));
	Set1f(p, "uVolumeNear", volumeNear);
	Set1f(p, "uVolumeFar", volumeFar);
	gpu.BindTexture(0, m_scattering[current]);
	Set1i(p, "uScattering", 0);
	gpu.BindImage(0, m_integrated, 0, true, 0, rhi::ImageAccess::WriteOnly, rhi::Format::RGBA16F);
	gpu.Dispatch((m_sizeX + 7) / 8, (m_sizeY + 7) / 8, 1);
	gpu.PipelineBarrier(rhi::BarrierTextureFetch | rhi::BarrierImageAccess);
	gpu.BindTexture(0, rhi::TextureHandle());

	m_current = current;
	m_historyValid = true;
	m_executedThisFrame = true;
	++m_frame;
}

void VolumetricFogPass::Apply(RenderContext& ctx)
{
	if (!m_executedThisFrame || !ctx.hdrFBO || !ctx.gbufferFBO || !ctx.screenQuad) {
		return;
	}
	rhi::Device& gpu = rhi::Gpu();
	ctx.hdrFBO->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
	gpu.SetDepthTest(false);
	gpu.SetDepthWrite(false);
	gpu.SetBlendEnabled(true);
	gpu.SetBlendFunc(rhi::BlendFactor::One, rhi::BlendFactor::SrcAlpha); // dst' = inScatter + dst * transmittance
	gpu.UseProgram(m_applyShader);
	ExposurePass::Bind(ctx);
	gpu.BindTexture(0, m_integrated);
	Set1i(m_applyShader, "uIntegrated", 0);
	gpu.BindTexture(1, ctx.gbufferFBO->GetDepthTexture());
	Set1i(m_applyShader, "uDepth", 1);
	SetM4(m_applyShader, "uProj", ctx.proj);
	gpu.SetUniform(gpu.GetUniformLocation(m_applyShader, "uVolumeSize"), glm::ivec3(m_sizeX, m_sizeY, m_sizeZ));
	Set1f(m_applyShader, "uVolumeNear", m_volumeNear);
	Set1f(m_applyShader, "uVolumeFar", m_volumeFar);
	Set1f(m_applyShader, "uDistantExtinction", ctx.fogDistantDensity);
	// Distant haze glows with the ambient sky light (the shader applies the pre-exposure).
	Set3f(m_applyShader, "uDistantInScatter", ctx.fogAmbientRadiance * ctx.fogAlbedo * ctx.fogDistantGlow);
	ctx.screenQuad->Render();
	gpu.SetBlendEnabled(false);
	gpu.SetDepthWrite(true);
	gpu.SetDepthTest(true);
	gpu.BindFramebuffer(PresentTarget::Get());
}
