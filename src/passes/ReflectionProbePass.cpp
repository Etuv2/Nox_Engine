#include "ReflectionProbePass.h"
#include "LightClusterPass.h"
#include "ExposurePass.h"
#include "../ShaderLoader.h"
#include "../RenderContext.h"
#include "../SceneGraph.h"
#include "../RenderSystem.h"
#include "../Skybox.h"
#include "../LightManager.h"
#include "../TextureUnits.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>

namespace {
	struct ProbeGpu {
		glm::vec4 boxMin;   // w = blend distance
		glm::vec4 boxMax;   // w = intensity
		glm::vec4 position; // w = array layer (probe index)
	};

	const glm::vec3 kFaceDirs[6] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
	const glm::vec3 kFaceUps[6] = { {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0} };

	void SetUniform1i(GLuint p, const char* n, int v) { const GLint l = glGetUniformLocation(p, n); if (l >= 0) glUniform1i(l, v); }
	void SetUniform1f(GLuint p, const char* n, float v) { const GLint l = glGetUniformLocation(p, n); if (l >= 0) glUniform1f(l, v); }
}

ReflectionProbePass::ReflectionProbePass() = default;

ReflectionProbePass::~ReflectionProbePass()
{
	if (m_forwardShader) glDeleteProgram(m_forwardShader);
	if (m_prefilterShader) glDeleteProgram(m_prefilterShader);
	if (m_captureFBO) glDeleteFramebuffers(1, &m_captureFBO);
	if (m_captureDepth) glDeleteRenderbuffers(1, &m_captureDepth);
	GLuint textures[] = { m_captureCube, m_probeArray };
	for (GLuint t : textures) if (t) glDeleteTextures(1, &t);
	GLuint buffers[] = { m_probeSSBO, m_cubeVBO, m_captureExposureSSBO, m_disabledClusterUBO };
	for (GLuint b : buffers) if (b) glDeleteBuffers(1, &b);
	if (m_cubeVAO) glDeleteVertexArrays(1, &m_cubeVAO);
}

bool ReflectionProbePass::Initialize(RenderContext&)
{
	m_forwardShader = CreateShaderProgram("shaders/forward_transparent_vert.glsl", "shaders/forward_transparent_frag.glsl");
	m_prefilterShader = CreateShaderProgram("shaders/prefilter_vert.glsl", "shaders/prefilter_frag.glsl");
	if (!m_forwardShader || !m_prefilterShader) {
		std::cerr << "[ReflectionProbePass] Failed to create capture/prefilter shaders" << std::endl;
		return false;
	}

	glGenFramebuffers(1, &m_captureFBO);
	glGenRenderbuffers(1, &m_captureDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_captureDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, kFaceSize, kFaceSize);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_captureCube);
	glTextureStorage2D(m_captureCube, kMipLevels + 2, GL_RGBA16F, kFaceSize, kFaceSize);
	glTextureParameteri(m_captureCube, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTextureParameteri(m_captureCube, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTextureParameteri(m_captureCube, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTextureParameteri(m_captureCube, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTextureParameteri(m_captureCube, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	glGenBuffers(1, &m_probeSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_probeSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(ProbeGpu) * kMaxProbes, nullptr, GL_DYNAMIC_DRAW);

	// Capture-time state: pre-exposure 1 (probes store scene-referred radiance) and no clustering
	// (the cluster lists are built for the main camera).
	const float captureExposure[8] = { 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
	glGenBuffers(1, &m_captureExposureSSBO);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_captureExposureSSBO);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(captureExposure), captureExposure, GL_STATIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	const uint32_t disabledCluster[16] = {};
	glGenBuffers(1, &m_disabledClusterUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_disabledClusterUBO);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(disabledCluster), disabledCluster, GL_STATIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	const float v[] = {
		-1,-1,-1,  1, 1,-1,  1,-1,-1,   1, 1,-1, -1,-1,-1, -1, 1,-1,
		-1,-1, 1,  1,-1, 1,  1, 1, 1,   1, 1, 1, -1, 1, 1, -1,-1, 1,
		-1, 1, 1, -1, 1,-1, -1,-1,-1,  -1,-1,-1, -1,-1, 1, -1, 1, 1,
		 1, 1, 1,  1,-1,-1,  1, 1,-1,   1,-1,-1,  1, 1, 1,  1,-1, 1,
		-1,-1,-1,  1,-1,-1,  1,-1, 1,   1,-1, 1, -1,-1, 1, -1,-1,-1,
		-1, 1,-1,  1, 1, 1,  1, 1,-1,   1, 1, 1, -1, 1,-1, -1, 1, 1 };
	glGenVertexArrays(1, &m_cubeVAO);
	glGenBuffers(1, &m_cubeVBO);
	glBindVertexArray(m_cubeVAO);
	glBindBuffer(GL_ARRAY_BUFFER, m_cubeVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
	glBindVertexArray(0);
	return true;
}

int ReflectionProbePass::UploadProbeData(const std::vector<ReflectionProbeDesc>& probes, const glm::mat4* viewProj)
{
	// Frustum planes (Gribb-Hartmann) for per-frame culling: pixels can only sample probes whose
	// influence box contains them, and every visible pixel lies inside the frustum.
	glm::vec4 planes[6];
	if (viewProj) {
		const glm::mat4& m = *viewProj;
		const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
		const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
		const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
		const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
		planes[0] = r3 + r0; planes[1] = r3 - r0; planes[2] = r3 + r1;
		planes[3] = r3 - r1; planes[4] = r3 + r2; planes[5] = r3 - r2;
	}
	auto boxVisible = [&](const glm::vec3& mn, const glm::vec3& mx) {
		for (const glm::vec4& pl : planes) {
			const glm::vec3 positive(pl.x >= 0.0f ? mx.x : mn.x, pl.y >= 0.0f ? mx.y : mn.y, pl.z >= 0.0f ? mx.z : mn.z);
			if (glm::dot(glm::vec3(pl), positive) + pl.w < 0.0f) return false;
		}
		return true;
	};
	std::vector<ProbeGpu> data;
	for (int i = 0; i < m_probeCount; ++i) {
		const ReflectionProbeDesc& p = probes[static_cast<size_t>(i)];
		if (viewProj && !boxVisible(p.boxMin, p.boxMax)) continue;
		data.push_back({ glm::vec4(p.boxMin, std::max(p.blendDistance, 0.01f)),
			glm::vec4(p.boxMax, p.intensity), glm::vec4(p.position, static_cast<float>(i)) });
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_probeSSBO);
	if (!data.empty()) {
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(ProbeGpu) * data.size(), data.data());
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	return static_cast<int>(data.size());
}

void ReflectionProbePass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>&,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>& skybox)
{
	if (!sceneGraph) return;
	if (sceneGraph.get() != m_lastScene) {
		m_lastScene = sceneGraph.get();
		m_probeCount = 0;
		m_framesUntilCapture = sceneGraph->reflectionProbes.empty() ? -1 : 2;
	}
	if (m_framesUntilCapture > 0) {
		--m_framesUntilCapture;
	}
	else if (m_framesUntilCapture == 0) {
		CaptureAll(ctx, sceneGraph, skybox);
		m_framesUntilCapture = -1;
	}
	ctx.reflectionProbeArray = (m_probeCount > 0 && ctx.enableReflectionProbes) ? m_probeArray : 0;
	ctx.reflectionProbeSSBO = m_probeSSBO;
	int visibleProbes = 0;
	if (m_probeCount > 0 && ctx.enableReflectionProbes) {
		const glm::mat4 viewProj = ctx.proj * ctx.view;
		visibleProbes = UploadProbeData(sceneGraph->reflectionProbes, &viewProj);
	}
	ctx.reflectionProbeCount = visibleProbes;
	ctx.reflectionProbeMaxLOD = static_cast<float>(kMipLevels - 1);
}

void ReflectionProbePass::CaptureAll(RenderContext& ctx, const std::shared_ptr<SceneGraph>& sceneGraph, const std::shared_ptr<Skybox>& skybox)
{
	const auto start = std::chrono::steady_clock::now();
	const std::vector<ReflectionProbeDesc>& probes = sceneGraph->reflectionProbes;
	m_probeCount = std::min(static_cast<int>(probes.size()), kMaxProbes);
	if (m_probeCount <= 0) return;

	if (m_capacity < m_probeCount) {
		if (m_probeArray) glDeleteTextures(1, &m_probeArray);
		glCreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &m_probeArray);
		glTextureStorage3D(m_probeArray, kMipLevels, GL_RGBA16F, kFaceSize, kFaceSize, 6 * m_probeCount);
		glTextureParameteri(m_probeArray, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTextureParameteri(m_probeArray, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTextureParameteri(m_probeArray, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(m_probeArray, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		m_capacity = m_probeCount;
	}

	GLint prevFBO = 0;
	GLint prevViewport[4] = {};
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
	glGetIntegerv(GL_VIEWPORT, prevViewport);

	// Probe faces need every static item, not the main camera's visible set.
	if (auto* renderSystem = sceneGraph->GetRenderSystem()) {
		renderSystem->SetReflectionCaptureActive(true);
		renderSystem->ClearFrustum();
	}
	for (int i = 0; i < m_probeCount; ++i) {
		CaptureProbe(ctx, sceneGraph, skybox, probes[static_cast<size_t>(i)], i);
		PrefilterInto(i);
	}
	if (auto* renderSystem = sceneGraph->GetRenderSystem()) {
		renderSystem->SetReflectionCaptureActive(false);
		if (ctx.enableFrustumCulling) {
			renderSystem->SetFrustumPlanes(ctx.proj * ctx.view);
		}
	}
	UploadProbeData(probes, nullptr);

	// Restore shared state for the rest of the frame.
	glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFBO));
	glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
	LightClusterPass::BindForShading(ctx);
	ExposurePass::Bind(ctx);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);

	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	std::cout << "[ReflectionProbePass] Captured " << m_probeCount << " probes (" << kFaceSize << "^2) in "
		<< ms << " ms (CPU submit)" << std::endl;
}

void ReflectionProbePass::CaptureProbe(RenderContext& ctx, const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Skybox>& skybox, const ReflectionProbeDesc& probe, int)
{
	RenderSystem* renderSystem = sceneGraph->GetRenderSystem();
	if (!renderSystem) return;
	const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, 600.0f);

	glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_captureDepth);
	glViewport(0, 0, kFaceSize, kFaceSize);

	// Capture-time bindings shared by every face.
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, ExposurePass::kStateBinding, m_captureExposureSSBO);
	glBindBufferBase(GL_UNIFORM_BUFFER, LightClusterPass::kParamsBinding, m_disabledClusterUBO);

	for (int face = 0; face < 6; ++face) {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, m_captureCube, 0);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDisable(GL_BLEND);
		const glm::mat4 view = glm::lookAt(probe.position, probe.position + kFaceDirs[face], kFaceUps[face]);

		const GLuint p = m_forwardShader;
		glUseProgram(p);
		glUniformMatrix4fv(glGetUniformLocation(p, "view"), 1, GL_FALSE, glm::value_ptr(view));
		glUniformMatrix4fv(glGetUniformLocation(p, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
		glUniform3fv(glGetUniformLocation(p, "viewPos"), 1, glm::value_ptr(probe.position));
		if (skybox && skybox->ValidateIBLTextures()) {
			glActiveTexture(GL_TEXTURE0 + TextureUnits::IRRADIANCE_MAP);
			glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetIrradianceMap());
			glActiveTexture(GL_TEXTURE0 + TextureUnits::PREFILTERED_ENV_MAP);
			glBindTexture(GL_TEXTURE_CUBE_MAP, skybox->GetPrefilteredMap());
			glActiveTexture(GL_TEXTURE0 + TextureUnits::BRDF_LUT);
			glBindTexture(GL_TEXTURE_2D, skybox->GetBRDFLUT());
			SetUniform1i(p, "irradianceMap", TextureUnits::IRRADIANCE_MAP);
			SetUniform1i(p, "prefilteredMap", TextureUnits::PREFILTERED_ENV_MAP);
			SetUniform1i(p, "brdfLUT", TextureUnits::BRDF_LUT);
			SetUniform1f(p, "prefilteredMaxLOD", skybox->GetPrefilteredMaxLOD());
			SetUniform1f(p, "iblIntensity", ctx.iblIntensity);
			SetUniform1f(p, "diffuseIBLScale", ctx.diffuseIBLScale);
			SetUniform1f(p, "specularIBLScale", ctx.specularIBLScale);
		}
		SetUniform1i(p, "multiLightShadowArray", TextureUnits::SHADOW_MAP_ARRAY);
		int shadowsAvailable = 0;
		if (ctx.lightManager) {
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx.lightManager->GetLightDataSSBO());
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ctx.lightManager->GetShadowMatricesSSBO());
			SetUniform1i(p, "numLights", ctx.lightManager->GetActiveLightCount());
			const GLuint shadowArray = ctx.lightManager->GetShadowArrayTexture();
			if (shadowArray > 0 && glIsTexture(shadowArray)) {
				glActiveTexture(GL_TEXTURE0 + TextureUnits::SHADOW_MAP_ARRAY);
				glBindTexture(GL_TEXTURE_2D_ARRAY, shadowArray);
				shadowsAvailable = 1;
			}
			SetUniform1f(p, "shadowBias", ctx.shadowBias);
		}
		SetUniform1i(p, "enableShadows", (ctx.enableShadows && shadowsAvailable) ? 1 : 0);
		SetUniform1f(p, "uWeatherWetness", ctx.weatherWetness);
		SetUniform1f(p, "uWeatherPuddles", ctx.weatherPuddles);
		SetUniform1f(p, "uWeatherRain", 0.0f); // no animated ripples baked into static probes
		SetUniform1f(p, "uWeatherPuddleScale", ctx.weatherPuddleScale);
		SetUniform1f(p, "uWeatherTime", 0.0f);

		renderSystem->RenderForward(view, projection, m_forwardShader);

		if (skybox) {
			glDepthFunc(GL_LEQUAL);
			skybox->Draw(view, projection);
			glDepthFunc(GL_LESS);
		}
	}
	glGenerateTextureMipmap(m_captureCube);
}

void ReflectionProbePass::PrefilterInto(int index)
{
	const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
	glBindFramebuffer(GL_FRAMEBUFFER, m_captureFBO);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glUseProgram(m_prefilterShader);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_CUBE_MAP, m_captureCube);
	SetUniform1i(m_prefilterShader, "environmentMap", 0);
	SetUniform1f(m_prefilterShader, "resolution", static_cast<float>(kFaceSize));
	glUniformMatrix4fv(glGetUniformLocation(m_prefilterShader, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
	glBindVertexArray(m_cubeVAO);
	for (int mip = 0; mip < kMipLevels; ++mip) {
		const int size = std::max(kFaceSize >> mip, 1);
		glViewport(0, 0, size, size);
		// Same perceptual-roughness ladder as the global environment (Skybox::GeneratePrefilteredMap).
		SetUniform1f(m_prefilterShader, "roughness", static_cast<float>(mip) / static_cast<float>(kMipLevels - 1));
		for (int face = 0; face < 6; ++face) {
			const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), kFaceDirs[face], kFaceUps[face]);
			glUniformMatrix4fv(glGetUniformLocation(m_prefilterShader, "view"), 1, GL_FALSE, glm::value_ptr(view));
			glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_probeArray, mip, index * 6 + face);
			glDrawArrays(GL_TRIANGLES, 0, 36);
		}
	}
	glBindVertexArray(0);
	glEnable(GL_DEPTH_TEST);
}

void ReflectionProbePass::BindForShading(const RenderContext& ctx)
{
	glActiveTexture(GL_TEXTURE0 + TextureUnits::REFLECTION_PROBES);
	glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, ctx.reflectionProbeArray);
	if (ctx.reflectionProbeSSBO) {
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kProbeBinding, ctx.reflectionProbeSSBO);
	}
}
