#include "ParticlePass.h"
#include "../PresentTarget.h"
#include "../SceneNode.h"
#include "../SceneGraph.h"
#include "../ShaderLoader.h"
#include "../FrameBuffer.h"
#include "../RenderContext.h"
#include "../Camera.h"
#include "../TextureArrayBuilder.h"
#include "ExposurePass.h"
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

using nox::render::ParticleBlend;

namespace {
// Corner attribute for a triangle strip: x across [-1,1], y along [-1,1].
const float kCorners[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
constexpr uint32_t kCornerLocation = 0;
constexpr uint32_t kInstanceLocation = 1; // three consecutive vec4 attributes: 1..3
constexpr uint32_t kInstanceVec4s = 3;
}

ParticlePass::~ParticlePass()
{
	if (!rhi::HasGpu()) return;
	rhi::Device& gpu = rhi::Gpu();
	if (m_shader) gpu.DestroyProgram(m_shader);
	if (m_instanceVbo) gpu.DestroyBuffer(m_instanceVbo);
	if (m_cornerVbo) gpu.DestroyBuffer(m_cornerVbo);
	if (m_vao) gpu.DestroyVertexInput(m_vao);
}

bool ParticlePass::Initialize(RenderContext&)
{
	m_shader = CreateShaderProgram("shaders/particle_vert.glsl", "shaders/particle_frag.glsl");
	if (!m_shader) {
		std::cerr << "[ParticlePass] Failed to create particle shader" << std::endl;
		return false;
	}
	rhi::Device& gpu = rhi::Gpu();

	rhi::BufferDesc cornerDesc;
	cornerDesc.size = sizeof(kCorners);
	cornerDesc.initialData = kCorners;
	cornerDesc.usage = rhi::BufferUsage::StaticDraw;
	m_cornerVbo = gpu.CreateBuffer(cornerDesc);
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
	instanceBinding.stride = sizeof(GpuParticle);
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
	m_vao = gpu.CreateVertexInput(vaoDesc);
	return true;
}

// Appends the live particles of every emitter in the subtree; one batch per emitter.
void ParticlePass::Gather(SceneNode& node)
{
	if (const std::shared_ptr<nox::render::ParticleEmitterComponent> emitter = node.GetParticleEmitter()) {
		const auto& particles = emitter->system.Particles();
		if (!particles.empty()) {
			const glm::mat4 world = node.GetWorldPosition4x4();
			const nox::render::ParticleEmitterSettings& s = emitter->settings;
			Batch batch;
			batch.first = static_cast<uint32_t>(m_instances.size());
			batch.blend = s.blend;
			batch.texture = s.texture;
			for (const nox::render::Particle& p : particles) {
				const float t = p.lifetime > 0.0f ? std::clamp(p.age / p.lifetime, 0.0f, 1.0f) : 1.0f;
				const glm::vec3 position = s.worldSpace ? p.position : glm::vec3(world * glm::vec4(p.position, 1.0f));
				GpuParticle g;
				g.posSize = glm::vec4(position, glm::mix(s.startSize, s.endSize, t));
				g.color = glm::mix(s.startColor, s.endColor, t);
				g.misc = glm::vec4(p.seed * 6.28318530718f, p.seed, t, 0.0f);
				m_instances.push_back(g);
			}
			batch.count = static_cast<uint32_t>(m_instances.size()) - batch.first;
			m_batches.push_back(batch);
		}
	}
	for (const std::shared_ptr<SceneNode>& child : node.children) {
		if (child) Gather(*child);
	}
}

void ParticlePass::Execute(RenderContext& ctx,
	const std::shared_ptr<SceneGraph>& sceneGraph,
	const std::shared_ptr<Camera>& camera,
	const std::shared_ptr<DirectionalLight>&,
	const std::shared_ptr<Skybox>&)
{
	if (!sceneGraph || !camera || !ctx.hdrFBO) return;
	const std::shared_ptr<SceneNode> root = sceneGraph->GetRoot();
	if (!root) return;

	m_instances.clear();
	m_batches.clear();
	Gather(*root);
	if (m_instances.empty()) return;

	rhi::Device& gpu = rhi::Gpu();

	// Stream the instance list: orphan the store every frame so the upload never stalls on the GPU.
	const size_t count = m_instances.size();
	if (count > m_instanceCapacity) {
		m_instanceCapacity = std::max(count, m_instanceCapacity * 2);
	}
	gpu.ReallocateBuffer(m_instanceVbo, m_instanceCapacity * sizeof(GpuParticle), nullptr, rhi::BufferUsage::StreamDraw);
	gpu.UpdateBuffer(m_instanceVbo, 0, count * sizeof(GpuParticle), m_instances.data());

	const glm::mat4 viewProj = ctx.proj * ctx.view;

	ctx.hdrFBO->Bind();
	gpu.SetViewport({ 0, 0, ctx.width, ctx.height });
	gpu.SetDepthTest(true);
	gpu.SetDepthFunc(rhi::CompareOp::Less);
	gpu.SetDepthWrite(false);
	gpu.SetCullEnabled(false);
	gpu.SetBlendEnabled(true);

	gpu.UseProgram(m_shader);
	ExposurePass::Bind(ctx);

	auto setMat = [&](const char* n, const glm::mat4& m) { const rhi::UniformLocation l = gpu.GetUniformLocation(m_shader, n); if (l >= 0) gpu.SetUniform(l, m); };
	setMat("uViewProj", viewProj);
	setMat("uView", ctx.view);
	gpu.SetUniform(gpu.GetUniformLocation(m_shader, "uParticleTex"), 0);

	gpu.BindVertexInput(m_vao);
	for (const Batch& batch : m_batches) {
		if (batch.blend == ParticleBlend::Additive) {
			gpu.SetBlendFunc(rhi::BlendFactor::One, rhi::BlendFactor::One);
		} else {
			gpu.SetBlendFunc(rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrcAlpha); // premultiplied
		}

		int hasTexture = 0;
		if (!batch.texture.empty()) {
			TextureArrayBuilder::Layer layer;
			layer.file = batch.texture;
			TextureArrayBuilder::Options options;
			options.size = 256;
			options.debugName = "particle " + batch.texture;
			const rhi::TextureHandle texture = TextureArrayBuilder::GetOrBuild({ layer }, options);
			if (texture) {
				gpu.BindTexture(0, texture);
				hasTexture = 1;
			}
		}
		gpu.SetUniform(gpu.GetUniformLocation(m_shader, "uHasTexture"), hasTexture);
		gpu.Draw(rhi::Topology::TriangleStrip, 0, 4, batch.count, batch.first);
	}
	gpu.BindVertexInput(rhi::VertexInputHandle());
	gpu.BindTexture(0, rhi::TextureHandle());

	gpu.SetBlendEnabled(false);
	gpu.SetDepthWrite(true);
	gpu.SetCullEnabled(true);
	gpu.BindFramebuffer(PresentTarget::Get());
}
