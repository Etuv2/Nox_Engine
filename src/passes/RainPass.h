#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include <glm/glm.hpp>

class VolumetricFogPass;

/*
* Rain streaks (docs/nfs2015/README.md, M6). Instanced streak quads in a box that wraps around the
* camera (shaders/rain_vert.glsl), lit from the froxel fog volume so drops glow inside lamp cones and
* headlight beams (shaders/rain_frag.glsl). Drawn into the HDR target after transparents, depth
* tested against the scene, before the fog is composited.
*/
class RainPass : public RenderPass {
public:
	RainPass() = default;
	~RainPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext&, int, int) override {}
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

	void SetFogPass(const VolumetricFogPass* fog) { m_fog = fog; }

private:
	rhi::ProgramHandle m_shader;
	rhi::VertexInputHandle m_vao;
	rhi::BufferHandle m_vbo;
	const VolumetricFogPass* m_fog = nullptr;
	glm::vec3 m_prevCameraPos{ 0.0f };
	bool m_havePrev = false;
};
