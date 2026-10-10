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

	void SetUniform1i(rhi::ProgramHandle p, const char* n, int v) { const rhi::UniformLocation l = rhi::Gpu().GetUniformLocation(p, n); if (l >= 0) rhi::Gpu().SetUniform(l, v); }
	void SetUniform1f(rhi::ProgramHandle p, const char* n, float v) { const rhi::UniformLocation l = rhi::Gpu().GetUniformLocation(p, n); if (l >= 0) rhi::Gpu().SetUniform(l, v); }
}

ReflectionProbePass::ReflectionProbePass() = default;

ReflectionProbePass::~ReflectionProbePass()
{
	if (!rhi::HasGpu()) return;
	rhi::Device& gpu = rhi::Gpu();
	if (m_forwardShader) gpu.DestroyProgram(m_forwardShader);
	if (m_prefilterShader) gpu.DestroyProgram(m_prefilterShader);
	if (m_captureFBO) gpu.DestroyFramebuffer(m_captureFBO);
	if (m_captureDepth) gpu.DestroyTexture(m_captureDepth);
	rhi::TextureHandle textures[] = { m_captureCube, m_probeArray };
	for (rhi::TextureHandle& t : textures) if (t) gpu.DestroyTexture(t);
	rhi::BufferHandle buffers[] = { m_probeSSBO, m_cubeVBO, m_captureExposureSSBO, m_disabledClusterUBO };
	for (rhi::BufferHandle& b : buffers) if (b) gpu.DestroyBuffer(b);
	if (m_cubeVAO) gpu.DestroyVertexInput(m_cubeVAO);
}

bool ReflectionProbePass::Initialize(RenderContext&)
{
	m_forwardShader = CreateShaderProgram("shaders/forward_transparent_vert.glsl", "shaders/forward_transparent_frag.glsl");
	m_prefilterShader = CreateShaderProgram("shaders/prefilter_vert.glsl", "shaders/prefilter_frag.glsl");
	if (!m_forwardShader || !m_prefilterShader) {
		std::cerr << "[ReflectionProbePass] Failed to create capture/prefilter shaders" << std::endl;
		return false;
	}

	rhi::Device& gpu = rhi::Gpu();
	m_captureFBO = gpu.CreateFramebuffer(rhi::FramebufferDesc{});
	// The capture FBO renders to colour attachment 0 (the default draw/read buffer of a new framebuffer).
	const int captureDrawBuffers[1] = { 0 };
	gpu.SetDrawBuffers(m_captureFBO, captureDrawBuffers, 1);
	gpu.SetReadBuffer(m_captureFBO, 0);

	rhi::TextureDesc depthDesc;
	depthDesc.type = rhi::TextureType::Tex2D;
	depthDesc.format = rhi::Format::D32F;
	depthDesc.width = kFaceSize;
	depthDesc.height = kFaceSize;
	depthDesc.immutable = true;
	m_captureDepth = gpu.CreateTexture(depthDesc);

	rhi::TextureDesc cubeDesc;
	cubeDesc.type = rhi::TextureType::Cube;
	cubeDesc.format = rhi::Format::RGBA16F;
	cubeDesc.width = kFaceSize;
	cubeDesc.height = kFaceSize;
	cubeDesc.mipLevels = kMipLevels + 2;
	cubeDesc.immutable = true;
	m_captureCube = gpu.CreateTexture(cubeDesc);
	gpu.SetTextureFilter(m_captureCube, rhi::Filter::LinearMipLinear, rhi::Filter::Linear);
	gpu.SetTextureWrap(m_captureCube, rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge, rhi::Wrap::ClampToEdge);

	rhi::BufferDesc probeDesc;
	probeDesc.size = sizeof(ProbeGpu) * kMaxProbes;
	probeDesc.usage = rhi::BufferUsage::DynamicDraw;
	m_probeSSBO = gpu.CreateBuffer(probeDesc);

	// Capture-time state: pre-exposure 1 (probes store scene-referred radiance) and no clustering
	// (the cluster lists are built for the main camera).
	const float captureExposure[8] = { 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
	rhi::BufferDesc exposureDesc;
	exposureDesc.size = sizeof(captureExposure);
	exposureDesc.initialData = captureExposure;
	exposureDesc.usage = rhi::BufferUsage::StaticDraw;
	m_captureExposureSSBO = gpu.CreateBuffer(exposureDesc);
	const uint32_t disabledCluster[16] = {};
	rhi::BufferDesc clusterDesc;
	clusterDesc.size = sizeof(disabledCluster);
	clusterDesc.initialData = disabledCluster;
	clusterDesc.usage = rhi::BufferUsage::StaticDraw;
	m_disabledClusterUBO = gpu.CreateBuffer(clusterDesc);

	const float v[] = {
		-1,-1,-1,  1, 1,-1,  1,-1,-1,   1, 1,-1, -1,-1,-1, -1, 1,-1,
		-1,-1, 1,  1,-1, 1,  1, 1, 1,   1, 1, 1, -1, 1, 1, -1,-1, 1,
		-1, 1, 1, -1, 1,-1, -1,-1,-1,  -1,-1,-1, -1,-1, 1, -1, 1, 1,
		 1, 1, 1,  1,-1,-1,  1, 1,-1,   1,-1,-1,  1, 1, 1,  1,-1, 1,
		-1,-1,-1,  1,-1,-1,  1,-1, 1,   1,-1, 1, -1,-1, 1, -1,-1,-1,
		-1, 1,-1,  1, 1, 1,  1, 1,-1,   1, 1, 1, -1, 1,-1, -1, 1, 1 };
	rhi::BufferDesc vboDesc;
	vboDesc.size = sizeof(v);
	vboDesc.initialData = v;
	vboDesc.usage = rhi::BufferUsage::StaticDraw;
	m_cubeVBO = gpu.CreateBuffer(vboDesc);
	rhi::VertexInputDesc vaoDesc;
	rhi::VertexBufferBinding binding;
	binding.buffer = m_cubeVBO;
	binding.stride = 3 * sizeof(float);
	vaoDesc.buffers.push_back(binding);
	rhi::VertexAttribute position;
	position.location = 0;
	position.bufferIndex = 0;
	position.components = 3;
	position.type = rhi::ScalarType::Float;
	position.offset = 0;
	vaoDesc.attributes.push_back(position);
	m_cubeVAO = gpu.CreateVertexInput(vaoDesc);
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
	if (!data.empty()) {
		rhi::Gpu().UpdateBuffer(m_probeSSBO, 0, sizeof(ProbeGpu) * data.size(), data.data());
	}
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
	ctx.reflectionProbeArray = (m_probeCount > 0 && ctx.enableReflectionProbes) ? m_probeArray : rhi::TextureHandle();
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

	rhi::Device& gpu = rhi::Gpu();
	if (m_capacity < m_probeCount) {
		if (m_probeArray) gpu.DestroyTexture(m_probeArray);
		rhi::TextureDesc arrayDesc;
		arrayDesc.type = rhi::TextureType::CubeArray;
		arrayDesc.format = rhi::Format::RGBA16F;
		arrayDesc.width = kFaceSize;
		arrayDesc.height = kFaceSize;
		arrayDesc.depth = 6 * m_probeCount;
		arrayDesc.mipLevels = kMipLevels;
		arrayDesc.immutable = true;
		m_probeArray = gpu.CreateTexture(arrayDesc);
		gpu.SetTextureFilter(m_probeArray, rhi::Filter::LinearMipLinear, rhi::Filter::Linear);
		gpu.SetTextureWrapAxis(m_probeArray, 0, rhi::Wrap::ClampToEdge);
		gpu.SetTextureWrapAxis(m_probeArray, 1, rhi::Wrap::ClampToEdge);
		m_capacity = m_probeCount;
	}

	const rhi::FramebufferHandle prevFBO = gpu.CurrentDrawFramebuffer();
	const rhi::Rect prevViewport = gpu.GetViewport();

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
	gpu.BindFramebuffer(prevFBO);
	gpu.SetViewport(prevViewport);
	LightClusterPass::BindForShading(ctx);
	ExposurePass::Bind(ctx);
	gpu.SetDepthTest(true);
	gpu.SetDepthFunc(rhi::CompareOp::Less);
	gpu.SetDepthWrite(true);
	gpu.SetBlendEnabled(false);
	gpu.SetCullEnabled(true);
	gpu.SetCullFace(rhi::CullFace::Back);

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

	rhi::Device& gpu = rhi::Gpu();
	gpu.BindFramebuffer(m_captureFBO);
	gpu.SetAttachment(m_captureFBO, rhi::AttachmentPoint::Depth, { m_captureDepth });
	gpu.SetViewport({ 0, 0, kFaceSize, kFaceSize });

	// Capture-time bindings shared by every face.
	gpu.BindStorageBuffer(ExposurePass::kStateBinding, m_captureExposureSSBO);
	gpu.BindUniformBuffer(LightClusterPass::kParamsBinding, m_disabledClusterUBO);

	for (int face = 0; face < 6; ++face) {
		gpu.SetAttachment(m_captureFBO, rhi::AttachmentPoint::Color0, { m_captureCube, 0, face });
		gpu.SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		gpu.Clear(rhi::ClearColor | rhi::ClearDepth);
		gpu.SetDepthTest(true);
		gpu.SetDepthFunc(rhi::CompareOp::Less);
		gpu.SetBlendEnabled(false);
		const glm::mat4 view = glm::lookAt(probe.position, probe.position + kFaceDirs[face], kFaceUps[face]);

		const rhi::ProgramHandle p = m_forwardShader;
		gpu.UseProgram(p);
		gpu.SetUniform(gpu.GetUniformLocation(p, "view"), view);
		gpu.SetUniform(gpu.GetUniformLocation(p, "projection"), projection);
		gpu.SetUniform(gpu.GetUniformLocation(p, "viewPos"), probe.position);
		if (skybox && skybox->ValidateIBLTextures()) {
			gpu.BindTexture(TextureUnits::IRRADIANCE_MAP, skybox->GetIrradianceMap());
			gpu.BindTexture(TextureUnits::PREFILTERED_ENV_MAP, skybox->GetPrefilteredMap());
			gpu.BindTexture(TextureUnits::BRDF_LUT, skybox->GetBRDFLUT());
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
			gpu.BindStorageBuffer(0, ctx.lightManager->GetLightDataSSBO());
			gpu.BindStorageBuffer(1, ctx.lightManager->GetShadowMatricesSSBO());
			SetUniform1i(p, "numLights", ctx.lightManager->GetActiveLightCount());
			const rhi::TextureHandle shadowArray = ctx.lightManager->GetShadowArrayTexture();
			if (shadowArray.IsValid() && gpu.IsTexture(shadowArray)) {
				gpu.BindTexture(TextureUnits::SHADOW_MAP_ARRAY, shadowArray);
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
			gpu.SetDepthFunc(rhi::CompareOp::LessEqual);
			skybox->Draw(view, projection);
			gpu.SetDepthFunc(rhi::CompareOp::Less);
		}
	}
	gpu.GenerateMipmaps(m_captureCube);
}

void ReflectionProbePass::PrefilterInto(int index)
{
	const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
	rhi::Device& gpu = rhi::Gpu();
	gpu.BindFramebuffer(m_captureFBO);
	gpu.SetAttachment(m_captureFBO, rhi::AttachmentPoint::Depth, rhi::Attachment());
	gpu.SetDepthTest(false);
	gpu.SetCullEnabled(false);
	gpu.UseProgram(m_prefilterShader);
	gpu.BindTexture(0, m_captureCube);
	SetUniform1i(m_prefilterShader, "environmentMap", 0);
	SetUniform1f(m_prefilterShader, "resolution", static_cast<float>(kFaceSize));
	gpu.SetUniform(gpu.GetUniformLocation(m_prefilterShader, "projection"), projection);
	gpu.BindVertexInput(m_cubeVAO);
	for (int mip = 0; mip < kMipLevels; ++mip) {
		const int size = std::max(kFaceSize >> mip, 1);
		gpu.SetViewport({ 0, 0, size, size });
		// Same perceptual-roughness ladder as the global environment (Skybox::GeneratePrefilteredMap).
		SetUniform1f(m_prefilterShader, "roughness", static_cast<float>(mip) / static_cast<float>(kMipLevels - 1));
		for (int face = 0; face < 6; ++face) {
			const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), kFaceDirs[face], kFaceUps[face]);
			gpu.SetUniform(gpu.GetUniformLocation(m_prefilterShader, "view"), view);
			gpu.SetAttachment(m_captureFBO, rhi::AttachmentPoint::Color0, { m_probeArray, mip, index * 6 + face });
			gpu.Draw(rhi::Topology::Triangles, 0, 36);
		}
	}
	gpu.BindVertexInput(rhi::VertexInputHandle());
	gpu.SetDepthTest(true);
}

void ReflectionProbePass::BindForShading(const RenderContext& ctx)
{
	rhi::Device& gpu = rhi::Gpu();
	gpu.BindTexture(TextureUnits::REFLECTION_PROBES, ctx.reflectionProbeArray);
	if (ctx.reflectionProbeSSBO) {
		gpu.BindStorageBuffer(kProbeBinding, ctx.reflectionProbeSSBO);
	}
}
