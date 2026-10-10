#include "DecalPass.h"
#include "../PresentTarget.h"
#include "../SceneGraph.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../TextureArrayBuilder.h"
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

namespace {
const float kCorners[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
constexpr uint32_t kCornerLocation = 0;
constexpr uint32_t kInstanceLocation = 1; // five consecutive vec4 attributes: 1..5
constexpr uint32_t kInstanceVec4s = 5;
constexpr int kGBufferTargets = 8;

struct GpuDecal {
	glm::vec4 centerLayer;
	glm::vec4 axisU;
	glm::vec4 axisV;
	glm::vec4 uvRect;
	glm::vec4 tint;
};
static_assert(sizeof(GpuDecal) == kInstanceVec4s * sizeof(glm::vec4), "instance layout");
}

DecalPass::~DecalPass()
{
	if (!rhi::HasGpu()) return;
	rhi::Device& dev = rhi::Gpu();
	if (m_shader) dev.DestroyProgram(m_shader);
	if (m_instanceVbo) dev.DestroyBuffer(m_instanceVbo);
	if (m_cornerVbo) dev.DestroyBuffer(m_cornerVbo);
	if (m_vao) dev.DestroyVertexInput(m_vao);
}

bool DecalPass::Initialize(RenderContext&)
{
	m_shader = CreateShaderProgram("shaders/decal_vert.glsl", "shaders/decal_frag.glsl");
	if (!m_shader) {
		std::cerr << "[DecalPass] Failed to create decal shader" << std::endl;
		return false;
	}
	rhi::Device& dev = rhi::Gpu();

	rhi::BufferDesc cornerDesc;
	cornerDesc.size = sizeof(kCorners);
	cornerDesc.initialData = kCorners;
	cornerDesc.usage = rhi::BufferUsage::StaticDraw;
	m_cornerVbo = dev.CreateBuffer(cornerDesc);
	// Instance store starts empty; Execute uploads the decal list when its revision changes.
	m_instanceVbo = dev.CreateBuffer(rhi::BufferDesc{});

	rhi::VertexInputDesc vaoDesc;
	rhi::VertexBufferBinding cornerBinding;
	cornerBinding.buffer = m_cornerVbo;
	cornerBinding.stride = 2 * sizeof(float);
	vaoDesc.buffers.push_back(cornerBinding);
	rhi::VertexBufferBinding instanceBinding;
	instanceBinding.buffer = m_instanceVbo;
	instanceBinding.stride = sizeof(GpuDecal);
	instanceBinding.instanceDivisor = 1;
	vaoDesc.buffers.push_back(instanceBinding);

	rhi::VertexAttribute corner;
	corner.location = kCornerLocation;
	corner.bufferIndex = 0;
	corner.components = 2;
	corner.type = rhi::ScalarType::Float;
	corner.offset = 0;
	vaoDesc.attributes.push_back(corner);
	for (uint32_t i = 0; i < kInstanceVec4s; ++i) {
		rhi::VertexAttribute row;
		row.location = kInstanceLocation + i;
		row.bufferIndex = 1;
		row.components = 4;
		row.type = rhi::ScalarType::Float;
		row.offset = static_cast<uint32_t>(i * sizeof(glm::vec4));
		vaoDesc.attributes.push_back(row);
	}
	m_vao = dev.CreateVertexInput(vaoDesc);
	return true;
}

void DecalPass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>&,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	if (!sceneGraph || !ctx.gbufferFBO) return;
	const SceneRenderExtras& extras = sceneGraph->renderExtras;
	if (extras.decalSets.empty()) return;

	rhi::Device& dev = rhi::Gpu();
	if (extras.revision != m_revision) {
		std::vector<GpuDecal> gpu;
		m_ranges.clear();
		for (const DecalSet& set : extras.decalSets) {
			SetRange range;
			range.firstInstance = static_cast<uint32_t>(gpu.size());
			for (const DecalInstance& d : set.instances) {
				gpu.push_back({ glm::vec4(d.center, static_cast<float>(d.layer)), glm::vec4(d.axisU, 0.0f),
					glm::vec4(d.axisV, 0.0f), d.uvRect, d.tint });
			}
			range.count = static_cast<int>(gpu.size() - range.firstInstance);
			m_ranges.push_back(range);
		}
		dev.ReallocateBuffer(m_instanceVbo, gpu.size() * sizeof(GpuDecal), gpu.data(), rhi::BufferUsage::StaticDraw);
		m_revision = extras.revision;
		std::cout << "[DecalPass] Uploaded " << gpu.size() << " decals in " << m_ranges.size() << " set(s)" << std::endl;
	}

	ctx.gbufferFBO->Bind();
	dev.SetViewport({ 0, 0, ctx.width, ctx.height });

	// Read-only depth test; the negative offset pulls the overlay toward the camera so a decal a few
	// millimetres off its surface never z-fights, even at grazing angles far away.
	dev.SetDepthTest(true);
	dev.SetDepthFunc(rhi::CompareOp::LessEqual);
	dev.SetDepthWrite(false);
	dev.SetPolygonOffsetEnabled(true);
	dev.SetPolygonOffsetFactors(-1.0f, -2.0f);
	dev.SetCullEnabled(false);

	// RT0: roughness (B) only; RT1: albedo (RGB) only; every other target untouched.
	for (int i = 0; i < kGBufferTargets; ++i) dev.SetColorMask(i, false, false, false, false);
	dev.SetColorMask(0, false, false, true, false);
	dev.SetColorMask(1, true, true, true, false);
	for (int i = 0; i < 2; ++i) {
		dev.SetBlendEnabled(i, true);
		dev.SetBlendFunc(i, rhi::BlendFactor::SrcAlpha, rhi::BlendFactor::OneMinusSrcAlpha);
	}

	dev.UseProgram(m_shader);
	dev.SetUniform(dev.GetUniformLocation(m_shader, "uViewProj"), ctx.proj * ctx.view);
	auto set1f = [&](const char* n, float v) { const rhi::UniformLocation l = dev.GetUniformLocation(m_shader, n); if (l >= 0) dev.SetUniform(l, v); };
	set1f("uWeatherWetness", ctx.weatherWetness);
	set1f("uWeatherPuddles", ctx.weatherPuddles);
	set1f("uWeatherRain", ctx.weatherRain);
	set1f("uWeatherPuddleScale", ctx.weatherPuddleScale);
	set1f("uWeatherTime", ctx.timeSeconds);
	dev.SetUniform(dev.GetUniformLocation(m_shader, "uDecals"), 0);
	dev.BindVertexInput(m_vao);

	for (size_t s = 0; s < extras.decalSets.size() && s < m_ranges.size(); ++s) {
		const DecalSet& set = extras.decalSets[s];
		if (m_ranges[s].count == 0) continue;
		const rhi::TextureHandle texture = TextureArrayBuilder::GetOrBuild(set.layers, set.textureOptions);
		if (!texture) continue;
		set1f("uAlbedoScale", set.material.albedoScale);
		set1f("uRoughness", set.material.roughness);
		set1f("uWetnessResponse", set.material.wetnessResponse);
		set1f("uPuddleResponse", set.material.puddleResponse);
		dev.BindTexture(0, texture);
		dev.Draw(rhi::Topology::TriangleStrip, 0, 4, static_cast<uint32_t>(m_ranges[s].count), m_ranges[s].firstInstance);
	}

	dev.BindVertexInput(rhi::VertexInputHandle());
	dev.BindTexture(0, rhi::TextureHandle());
	for (int i = 0; i < 2; ++i) dev.SetBlendEnabled(i, false);
	for (int i = 0; i < kGBufferTargets; ++i) dev.SetColorMask(i, true, true, true, true);
	dev.SetPolygonOffsetEnabled(false);
	dev.SetPolygonOffsetFactors(0.0f, 0.0f);
	dev.SetDepthWrite(true);
	dev.SetDepthFunc(rhi::CompareOp::Less);
	dev.SetCullEnabled(true);
	dev.BindFramebuffer(PresentTarget::Get());
}
