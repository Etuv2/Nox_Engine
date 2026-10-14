#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include "../render/ParticleSystem.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

/*
* Generic particles (ParticleEmitter component, src/render/ParticleSystem.h). Sibling of VehicleFXPass:
* the particles of every emitter in the scene are gathered into one instance buffer and drawn as
* camera-facing billboards, one draw per emitter, after the vehicle FX in the HDR target. Emitters
* blend alpha (premultiplied) or additive; an optional texture is one layer of a texture array,
* otherwise a soft disc. Simulation runs once per frame in Core::Update (UpdateSceneEmitters); this
* pass only reads the particles. Depth tested read-only against the scene.
*/
class ParticlePass : public RenderPass {
public:
	ParticlePass() = default;
	~ParticlePass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext&, int, int) override {}
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

private:
	// Instance layout read by shaders/particle_vert.glsl: three vec4 attributes.
	struct GpuParticle {
		glm::vec4 posSize;   // xyz world position, w radius
		glm::vec4 color;     // rgb, a opacity (straight alpha)
		glm::vec4 misc;      // x roll, y seed, z normalised age, w unused
	};
	struct Batch {
		uint32_t first = 0;
		uint32_t count = 0;
		nox::render::ParticleBlend blend = nox::render::ParticleBlend::Alpha;
		std::string texture;
	};

	void Gather(SceneNode& node);
	static_assert(sizeof(GpuParticle) == 3 * sizeof(glm::vec4), "instance layout");

	rhi::ProgramHandle m_shader;
	rhi::VertexInputHandle m_vao;
	rhi::BufferHandle m_cornerVbo;
	rhi::BufferHandle m_instanceVbo;
	size_t m_instanceCapacity = 0;
	std::vector<GpuParticle> m_instances;
	std::vector<Batch> m_batches;
};
