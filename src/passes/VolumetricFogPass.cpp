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
	GLuint MakeVolume(int x, int y, int z) {
		GLuint tex = 0;
		glCreateTextures(GL_TEXTURE_3D, 1, &tex);
		glTextureStorage3D(tex, 1, GL_RGBA16F, std::max(x, 1), std::max(y, 1), std::max(z, 1));
		glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTextureParameteri(tex, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		return tex;
	}

	void Set1f(GLuint p, const char* n, float v) { const GLint l = glGetUniformLocation(p, n); if (l >= 0) glUniform1f(l, v); }
	void Set1i(GLuint p, const char* n, int v) { const GLint l = glGetUniformLocation(p, n); if (l >= 0) glUniform1i(l, v); }
	void Set3f(GLuint p, const char* n, const glm::vec3& v) { const GLint l = glGetUniformLocation(p, n); if (l >= 0) glUniform3fv(l, 1, glm::value_ptr(v)); }
	void SetM4(GLuint p, const char* n, const glm::mat4& m) { const GLint l = glGetUniformLocation(p, n); if (l >= 0) glUniformMatrix4fv(l, 1, GL_FALSE, glm::value_ptr(m)); }
}

VolumetricFogPass::VolumetricFogPass() = default;

VolumetricFogPass::~VolumetricFogPass()
{
	DestroyVolumes();
	if (m_applyShader) glDeleteProgram(m_applyShader);
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
	GLuint t[] = { m_scattering[0], m_scattering[1], m_integrated };
	for (GLuint tex : t) if (tex) glDeleteTextures(1, &tex);
	m_scattering[0] = m_scattering[1] = m_integrated = 0;
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
	GLuint p = m_injectShader->GetProgramID();
	glUseProgram(p);
	LightClusterPass::BindForShading(ctx);
	ExposurePass::Bind(ctx);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx.lightManager->GetLightDataSSBO());
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ctx.lightManager->GetShadowMatricesSSBO());
	Set1i(p, "numLights", ctx.lightManager->GetActiveLightCount());
	const GLuint shadowArray = ctx.lightManager->GetShadowArrayTexture();
	const bool shadows = ctx.enableShadows && shadowArray > 0 && glIsTexture(shadowArray);
	if (shadows) {
		glActiveTexture(GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY);
		glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArray);
	}
	Set1i(p, "multiLightShadowArray", TextureUnits::SHADOW_MAP_ARRAY);
	Set1i(p, "enableShadows", shadows ? 1 : 0);
	Set1f(p, "shadowBias", ctx.shadowBias);
	{
		const float nearPlane = std::max(ctx.shadowNear, camera->GetCameraNearPlane());
		const float farPlane = std::max(nearPlane + 1.0f, std::min(ctx.shadowFar, camera->GetCameraFarPlane()));
		const glm::vec4 splits = ctx.lightManager->ComputeCascadeSplitVector(nearPlane, farPlane);
		const GLint loc = glGetUniformLocation(p, "cascadeSplits");
		if (loc >= 0) glUniform4fv(loc, 1, glm::value_ptr(splits));
	}
	SetM4(p, "view", ctx.view);
	SetM4(p, "uInvViewProj", invViewProj);
	SetM4(p, "uPrevViewProj", prevViewProj);
	Set3f(p, "uCameraPos", camera->GetCameraPosition());
	glUniform2f(glGetUniformLocation(p, "uScreenSize"), static_cast<float>(ctx.width), static_cast<float>(ctx.height));
	glUniform1ui(glGetUniformLocation(p, "uFrame"), m_frame);
	glUniform3i(glGetUniformLocation(p, "uVolumeSize"), m_sizeX, m_sizeY, m_sizeZ);
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
	glBindTextureUnit(0, m_scattering[previous]);
	Set1i(p, "uHistoryVolume", 0);
	glBindImageTexture(0, m_scattering[current], 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	glDispatchCompute((m_sizeX + 7) / 8, (m_sizeY + 7) / 8, m_sizeZ);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	// 2. Integrate.
	p = m_integrateShader->GetProgramID();
	glUseProgram(p);
	glUniform3i(glGetUniformLocation(p, "uVolumeSize"), m_sizeX, m_sizeY, m_sizeZ);
	Set1f(p, "uVolumeNear", volumeNear);
	Set1f(p, "uVolumeFar", volumeFar);
	glBindTextureUnit(0, m_scattering[current]);
	Set1i(p, "uScattering", 0);
	glBindImageTexture(0, m_integrated, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	glDispatchCompute((m_sizeX + 7) / 8, (m_sizeY + 7) / 8, 1);
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	glBindTextureUnit(0, 0);

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
	ctx.hdrFBO->Bind();
	glViewport(0, 0, ctx.width, ctx.height);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_SRC_ALPHA); // dst' = inScatter + dst * transmittance
	glUseProgram(m_applyShader);
	ExposurePass::Bind(ctx);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_3D, m_integrated);
	Set1i(m_applyShader, "uIntegrated", 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, ctx.gbufferFBO->GetDepthTexture());
	Set1i(m_applyShader, "uDepth", 1);
	SetM4(m_applyShader, "uProj", ctx.proj);
	glUniform3i(glGetUniformLocation(m_applyShader, "uVolumeSize"), m_sizeX, m_sizeY, m_sizeZ);
	Set1f(m_applyShader, "uVolumeNear", m_volumeNear);
	Set1f(m_applyShader, "uVolumeFar", m_volumeFar);
	Set1f(m_applyShader, "uDistantExtinction", ctx.fogDistantDensity);
	// Distant haze glows with the ambient sky light (the shader applies the pre-exposure).
	Set3f(m_applyShader, "uDistantInScatter", ctx.fogAmbientRadiance * ctx.fogAlbedo * ctx.fogDistantGlow);
	ctx.screenQuad->Render();
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, PresentTarget::Get());
}
