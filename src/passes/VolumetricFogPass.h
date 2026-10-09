#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
#include <memory>
#include <glm/glm.hpp>

class ComputeShader;

/*
* Froxel volumetric fog (docs/nfs2015/README.md, M6), after Hillaire 2015 (Frostbite).
*
*   Execute: inject media + clustered lights + shadows per froxel with temporal reprojection
*            (shaders/volumetric_inject_comp.glsl), then integrate front to back
*            (shaders/volumetric_integrate_comp.glsl).
*   Apply:   composite over the HDR image before TAA (shaders/volumetric_apply_frag.glsl).
*
* Resources: two RGBA16F scattering/extinction volumes (ping-pong history) and one integrated
* volume, sized (width / tile) x (height / tile) x slices.
*/
class VolumetricFogPass : public RenderPass {
public:
	VolumetricFogPass();
	~VolumetricFogPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;
	void Apply(RenderContext& ctx);

	void ResetHistory() { m_historyValid = false; }

	// This frame's scattering/extinction volume (0 when the pass did not run), used to light rain.
	GLuint GetScatteringVolume() const { return m_executedThisFrame ? m_scattering[m_current] : 0; }
	glm::ivec3 GetVolumeSize() const { return glm::ivec3(m_sizeX, m_sizeY, m_sizeZ); }
	float GetVolumeNear() const { return m_volumeNear; }
	float GetVolumeFar() const { return m_volumeFar; }

private:
	void CreateVolumes(int width, int height, int tileSize, int slices);
	void DestroyVolumes();

	std::unique_ptr<ComputeShader> m_injectShader;
	std::unique_ptr<ComputeShader> m_integrateShader;
	GLuint m_applyShader = 0;
	GLuint m_scattering[2] = { 0, 0 };
	GLuint m_integrated = 0;
	int m_sizeX = 0, m_sizeY = 0, m_sizeZ = 0;
	int m_tileSize = 0;
	int m_current = 0;
	bool m_historyValid = false;
	bool m_executedThisFrame = false;
	unsigned m_frame = 0;
	float m_volumeNear = 0.1f;
	float m_volumeFar = 200.0f;
};
