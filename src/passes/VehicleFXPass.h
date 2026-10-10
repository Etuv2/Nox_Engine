#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
#include <glm/glm.hpp>

class VolumetricFogPass;

/*
* Vehicle smoke and wet-road spray (src/VehicleFX.*). Each frame the pass uploads the VehicleFX render
* list into an instance buffer and draws camera-facing billboards into the HDR target after fog
* compositing and rain, before TAA. Depth tested read-only against the scene with a soft-particle fade
* from the G-buffer depth, premultiplied alpha. Lighting comes from the froxel fog volume (the same
* lamp/headlight field rain streaks use) plus the scene ambient radiance.
*/
class VehicleFXPass : public RenderPass {
public:
	VehicleFXPass() = default;
	~VehicleFXPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext&, int, int) override {}
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

	void SetFogPass(const VolumetricFogPass* fog) { m_fog = fog; }

private:
	GLuint m_shader = 0;
	GLuint m_vao = 0;
	GLuint m_cornerVbo = 0;
	GLuint m_instanceVbo = 0;
	size_t m_instanceCapacity = 0; // particles the instance buffer can hold
	const VolumetricFogPass* m_fog = nullptr;
};
