#pragma once
#include "../RenderPass.h"
#include "../ReflectionProbe.h"
#include <GL/glew.h>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

/*
* Static local reflection probes (docs/nfs2015/README.md, M5).
*
* Two frames after a scene loads (so shadow maps exist), every probe in SceneGraph::reflectionProbes
* is captured: six 90-degree faces rendered with the forward shading path (all lights, emissive,
* wetness, sky; dynamic models flagged excludeFromReflectionCapture are skipped) at pre-exposure 1,
* then GGX-prefiltered into one mip chain per probe in a cubemap array (TextureUnits::REFLECTION_PROBES).
* Probe boxes go to SSBO 44 for the lighting pass, which parallax-corrects and cross-fades them under
* SSR (shaders/includes/reflection_probes.glsl).
*/
class ReflectionProbePass : public RenderPass {
public:
	static constexpr GLuint kProbeBinding = 44;
	static constexpr int kMaxProbes = 48;
	static constexpr int kFaceSize = 256;
	static constexpr int kMipLevels = 7; // 256 .. 4

	ReflectionProbePass();
	~ReflectionProbePass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext&, int, int) override {}
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

	void RequestRecapture() { m_framesUntilCapture = 2; }
	static void BindForShading(const RenderContext& ctx);

private:
	void CaptureAll(RenderContext& ctx, const std::shared_ptr<SceneGraph>& sceneGraph, const std::shared_ptr<Skybox>& skybox);
	void CaptureProbe(RenderContext& ctx, const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Skybox>& skybox, const ReflectionProbeDesc& probe, int index);
	void PrefilterInto(int index);
	// Uploads the probes whose influence box intersects the view frustum (all when viewProj is
	// null) and returns how many were uploaded. Array layers are preserved in position.w.
	int UploadProbeData(const std::vector<ReflectionProbeDesc>& probes, const glm::mat4* viewProj);

	GLuint m_forwardShader = 0;
	GLuint m_prefilterShader = 0;
	GLuint m_captureFBO = 0;
	GLuint m_captureDepth = 0;
	GLuint m_captureCube = 0;     // temporary capture target with mips
	GLuint m_probeArray = 0;      // GL_TEXTURE_CUBE_MAP_ARRAY, RGBA16F
	GLuint m_probeSSBO = 0;
	GLuint m_cubeVAO = 0;
	GLuint m_cubeVBO = 0;
	GLuint m_captureExposureSSBO = 0; // pre-exposure 1 during capture
	GLuint m_disabledClusterUBO = 0;  // clustering off during capture
	int m_capacity = 0;
	const void* m_lastScene = nullptr;
	int m_framesUntilCapture = -1;
	int m_probeCount = 0;
};
