#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
#include <memory>

class ComputeShader;

/*
* Physical camera exposure (docs/nfs2015/README.md, M3).
*
* Owns the exposure state SSBO (binding 42, shaders/includes/exposure_common.glsl):
*   preExposure  - multiplier lighting shaders apply to the radiance they write this frame
*   exposure     - final exposure; post-processing applies exposure / preExposure
*   EV100, average luminance (debug)
*
* BeginFrame runs before any lighting: legacy scenes get {1, 1}, manual mode gets the value from
* EV100, auto mode commits last frame's adapted exposure as this frame's pre-exposure (GPU only).
* Execute (after TAA) meters the pre-exposed image in auto mode: a centre-weighted log-luminance
* histogram (SSBO 43), percentile-trimmed average, EV100 target with compensation and clamps, and
* exponential adaptation. Reset() snaps adaptation (scene load / camera cut).
*/
class ExposurePass : public RenderPass {
public:
	static constexpr GLuint kStateBinding = 42;
	static constexpr GLuint kHistogramBinding = 43;

	ExposurePass();
	~ExposurePass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext&, int, int) override {}
	void BeginFrame(RenderContext& ctx);
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

	void Reset() { m_resetPending = true; }
	static void Bind(const RenderContext& ctx);

	// Frostbite PBR 3.0: exposure = 1 / (1.2 * 2^EV100)
	static float ExposureFromEV100(float ev100);

private:
	void Upload(float preExposure, float exposure, float ev100);

	std::unique_ptr<ComputeShader> m_histogramShader;
	std::unique_ptr<ComputeShader> m_reduceShader;
	GLuint m_stateSSBO = 0;
	GLuint m_histogramSSBO = 0;
	bool m_resetPending = true;
	bool m_snapNext = true;
	int m_lastMode = -1; // -1 legacy, 0 manual, 1 auto
	float m_lastUploadedPre = -1.0f;
};
