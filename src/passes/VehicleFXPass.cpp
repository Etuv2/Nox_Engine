#include "VehicleFXPass.h"
#include "../PresentTarget.h"
#include "../VehicleFX.h"
#include "VolumetricFogPass.h"
#include "ExposurePass.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../Camera.h"
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

namespace {
// Corner attribute for a triangle strip: x across [-1,1], y along [-1,1].
const float kCorners[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
constexpr uint32_t kCornerLocation = 0;
constexpr uint32_t kInstanceLocation = 1; // three consecutive vec4 attributes: 1, 2, 3
static_assert(sizeof(VehicleFX::RenderParticle) == 3 * sizeof(glm::vec4), "instance layout");
}

VehicleFXPass::~VehicleFXPass()
{
	if (!rhi::HasGpu()) return;
	rhi::Device& gpu = rhi::Gpu();
	if (m_shader) gpu.DestroyProgram(m_shader);
	if (m_instanceVbo) gpu.DestroyBuffer(m_instanceVbo);
	if (m_cornerVbo) gpu.DestroyBuffer(m_cornerVbo);
	if (m_vao) gpu.DestroyVertexInput(m_vao);
}

bool VehicleFXPass::Initialize(RenderContext&)
{
	m_shader = CreateShaderProgram("shaders/vehicle_fx_vert.glsl", "shaders/vehicle_fx_frag.glsl");
	if (!m_shader) {
		std::cerr << "[VehicleFXPass] Failed to create vehicle FX shader" << std::endl;
		return false;
	}
	rhi::Device& gpu = rhi::Gpu();

	rhi::BufferDesc cornerDesc;
	cornerDesc.size = sizeof(kCorners);
	cornerDesc.initialData = kCorners;
	cornerDesc.usage = rhi::BufferUsage::StaticDraw;
	m_cornerVbo = gpu.CreateBuffer(cornerDesc);
	// Instance store starts empty; Execute orphans and refills it every frame.
	rhi::BufferDesc instanceDesc;
	instanceDesc.size = 0;
	instanceDesc.usage = rhi::BufferUsage::StreamDraw;
	m_instanceVbo = gpu.CreateBuffer(instanceDesc);

	rhi::VertexInputDesc vaoDesc;
	rhi::VertexBufferBinding cornerBinding;
	cornerBinding.buffer = m_cornerVbo;
	cornerBinding.stride = 2 * sizeof(float);
	vaoDesc.buffers.push_back(cornerBinding);
	rhi::VertexBufferBinding instanceBinding;
	instanceBinding.buffer = m_instanceVbo;
	instanceBinding.stride = sizeof(VehicleFX::RenderParticle);
	instanceBinding.instanceDivisor = 1;
	vaoDesc.buffers.push_back(instanceBinding);

	rhi::VertexAttribute corner;
	corner.location = kCornerLocation;
	corner.bufferIndex = 0;
	corner.components = 2;
	corner.type = rhi::ScalarType::Float;
	corner.offset = 0;
	vaoDesc.attributes.push_back(corner);
	for (uint32_t i = 0; i < 3; ++i) {
		rhi::VertexAttribute row;
		row.location = kInstanceLocation + i;
		row.bufferIndex = 1;
		row.components = 4;
		row.type = rhi::ScalarType::Float;
		row.offset = static_cast<uint32_t>(i * sizeof(glm::vec4));
		vaoDesc.attributes.push_back(row);
	}
	m_vao = gpu.CreateVertexInput(vaoDesc);
	return true;
}

void VehicleFXPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>&,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	VehicleFX::EnsureTestHarnessTick(ctx.deltaTime);
	const std::vector<VehicleFX::RenderParticle>& particles = VehicleFX::GetRenderParticles();
	if (particles.empty() || !camera || !ctx.hdrFBO || !ctx.gbufferFBO) {
		return;
	}

	rhi::Device& gpu = rhi::Gpu();

	// Stream the instance list: orphan the store every frame so the upload never stalls on the GPU.
	const size_t count = particles.size();
	if (count > m_instanceCapacity) {
		m_instanceCapacity = std::max(count, m_instanceCapacity * 2);
	}
	gpu.ReallocateBuffer(m_instanceVbo, m_instanceCapacity * sizeof(VehicleFX::RenderParticle), nullptr, rhi::BufferUsage::StreamDraw);
	gpu.UpdateBuffer(m_instanceVbo, 0, count * sizeof(VehicleFX::RenderParticle), particles.data());

	const glm::vec3 camPos = glm::vec3(glm::inverse(ctx.view)[3]);
	const glm::mat4 viewProj = ctx.proj * ctx.view;

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

	auto set1f = [&](const char* n, float v) { const rhi::UniformLocation l = gpu.GetUniformLocation(m_shader, n); if (l >= 0) gpu.SetUniform(l, v); };
	auto set3f = [&](const char* n, const glm::vec3& v) { const rhi::UniformLocation l = gpu.GetUniformLocation(m_shader, n); if (l >= 0) gpu.SetUniform(l, v); };
	auto setM4 = [&](const char* n, const glm::mat4& m) { const rhi::UniformLocation l = gpu.GetUniformLocation(m_shader, n); if (l >= 0) gpu.SetUniform(l, m); };

	setM4("uViewProj", viewProj);
	setM4("uView", ctx.view);
	setM4("uProj", ctx.proj);
	set3f("uCameraPos", camPos);
	set1f("uStreakTime", std::max(ctx.motionBlurExposureTime, 1.0f / 60.0f));
	set1f("uSoftRange", 0.6f);                         // metres of depth fade at intersections
	set1f("uBrightness", 1.0f);
	set3f("uSmokeTint", glm::vec3(1.0f, 0.84f, 0.66f)); // warm street-light tint on smoke
	set1f("uSmokeGain", 3.0f);
	set3f("uFogAlbedo", ctx.fogAlbedo);
	set3f("uAmbient", ctx.fogAmbientRadiance);
	{
		const rhi::UniformLocation l = gpu.GetUniformLocation(m_shader, "uScreenSize");
		if (l >= 0) gpu.SetUniform(l, glm::vec2(static_cast<float>(ctx.width), static_cast<float>(ctx.height)));
	}

	// Scene depth (G-buffer) for the soft-particle fade.
	gpu.BindTexture(1, ctx.gbufferFBO->GetDepthTexture());
	gpu.SetUniform(gpu.GetUniformLocation(m_shader, "uSceneDepth"), 1);

	// Froxel scattering volume for lamp and headlight light.
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

	gpu.BindVertexInput(m_vao);
	gpu.Draw(rhi::Topology::TriangleStrip, 0, 4, static_cast<uint32_t>(count));
	gpu.BindVertexInput(rhi::VertexInputHandle());

	gpu.BindTexture(0, rhi::TextureHandle());
	gpu.SetBlendEnabled(false);
	gpu.SetDepthWrite(true);
	gpu.SetCullEnabled(true);
	gpu.BindFramebuffer(PresentTarget::Get());
}
