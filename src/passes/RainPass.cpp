#include "RainPass.h"
#include "../PresentTarget.h"
#include "VolumetricFogPass.h"
#include "ExposurePass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../Camera.h"
#include <algorithm>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

RainPass::~RainPass()
{
	if (!rhi::HasGpu()) return;
	rhi::Device& gpu = rhi::Gpu();
	if (m_shader) gpu.DestroyProgram(m_shader);
	if (m_vbo) gpu.DestroyBuffer(m_vbo);
	if (m_vao) gpu.DestroyVertexInput(m_vao);
}

bool RainPass::Initialize(RenderContext&)
{
	m_shader = CreateShaderProgram("shaders/rain_vert.glsl", "shaders/rain_frag.glsl");
	if (!m_shader) {
		std::cerr << "[RainPass] Failed to create rain shader" << std::endl;
		return false;
	}
	const float corners[] = { -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 1.0f, 1.0f, 1.0f };
	rhi::Device& gpu = rhi::Gpu();
	rhi::BufferDesc vboDesc;
	vboDesc.size = sizeof(corners);
	vboDesc.initialData = corners;
	vboDesc.usage = rhi::BufferUsage::StaticDraw;
	m_vbo = gpu.CreateBuffer(vboDesc);
	rhi::VertexInputDesc vaoDesc;
	rhi::VertexBufferBinding binding;
	binding.buffer = m_vbo;
	binding.stride = 2 * sizeof(float);
	vaoDesc.buffers.push_back(binding);
	rhi::VertexAttribute corner;
	corner.location = 0;
	corner.bufferIndex = 0;
	corner.components = 2;
	corner.type = rhi::ScalarType::Float;
	corner.offset = 0;
	vaoDesc.attributes.push_back(corner);
	m_vao = gpu.CreateVertexInput(vaoDesc);
	return true;
}

void RainPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	if (ctx.weatherRain <= 0.0f || ctx.rainDropCount <= 0 || !camera || !ctx.hdrFBO) {
		m_havePrev = false;
		return;
	}
	const glm::vec3 camPos = glm::vec3(glm::inverse(ctx.view)[3]);
	const float dt = std::max(ctx.deltaTime, 1e-4f);
	const glm::vec3 camVelocity = m_havePrev ? (camPos - m_prevCameraPos) / dt : glm::vec3(0.0f);
	m_prevCameraPos = camPos;
	m_havePrev = true;

	rhi::Device& gpu = rhi::Gpu();
	ctx.hdrFBO->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
	gpu.SetDepthTest(true);
	gpu.SetDepthFunc(rhi::CompareOp::Less);
	gpu.SetDepthWrite(false);
	gpu.SetBlendEnabled(true);
	gpu.SetBlendFunc(rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrcAlpha); // premultiplied
	gpu.SetCullEnabled(false);

	gpu.UseProgram(m_shader);
	ExposurePass::Bind(ctx);
	const glm::mat4 viewProj = ctx.proj * ctx.view;
	auto set1f = [&](const char* n, float v) { const rhi::UniformLocation l = gpu.GetUniformLocation(m_shader, n); if (l >= 0) gpu.SetUniform(l, v); };
	auto set3f = [&](const char* n, const glm::vec3& v) { const rhi::UniformLocation l = gpu.GetUniformLocation(m_shader, n); if (l >= 0) gpu.SetUniform(l, v); };
	gpu.SetUniform(gpu.GetUniformLocation(m_shader, "uViewProj"), viewProj);
	gpu.SetUniform(gpu.GetUniformLocation(m_shader, "uView"), ctx.view);
	set3f("uCameraPos", camPos);
	set3f("uBoxSize", glm::vec3(36.0f, 22.0f, 36.0f));
	set1f("uTime", ctx.timeSeconds);
	set3f("uFallVelocity", glm::vec3(ctx.fogWind.x, -9.0f, ctx.fogWind.z));
	set1f("uStreakTime", std::max(ctx.motionBlurExposureTime, 1.0f / 60.0f));
	set1f("uDropWidth", 0.006f);
	set3f("uCameraVelocity", camVelocity);
	set3f("uFogAlbedo", ctx.fogAlbedo);
	set3f("uAmbient", ctx.fogAmbientRadiance);
	set1f("uOpacity", std::clamp(ctx.weatherRain, 0.0f, 1.0f));
	set1f("uBrightness", ctx.rainBrightness);

	const bool hasVolume = m_fog && ctx.enableVolumetricFog && m_fog->GetScatteringVolume().IsValid();
	gpu.SetUniform(gpu.GetUniformLocation(m_shader, "uHasVolume"), hasVolume ? 1 : 0);
	if (hasVolume) {
		gpu.BindTexture(0, m_fog->GetScatteringVolume());
		gpu.SetUniform(gpu.GetUniformLocation(m_shader, "uScattering"), 0);
		const glm::ivec3 size = m_fog->GetVolumeSize();
		gpu.SetUniform(gpu.GetUniformLocation(m_shader, "uVolumeSize"), size);
		set1f("uVolumeNear", m_fog->GetVolumeNear());
		set1f("uVolumeFar", m_fog->GetVolumeFar());
	}

	const int drops = static_cast<int>(static_cast<float>(ctx.rainDropCount) * std::clamp(ctx.weatherRain, 0.0f, 1.0f));
	gpu.BindVertexInput(m_vao);
	gpu.Draw(rhi::Topology::TriangleStrip, 0, 4, static_cast<uint32_t>(std::max(drops, 0)));
	gpu.BindVertexInput(rhi::VertexInputHandle());

	gpu.SetBlendEnabled(false);
	gpu.SetDepthWrite(true);
	gpu.SetCullEnabled(true);
	gpu.BindFramebuffer(PresentTarget::Get());
}
