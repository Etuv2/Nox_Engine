#pragma once
#include "../RenderPass.h"
#include "../ReflectionProbe.h"
#include "../rhi/RHI.h"
#include <cstdint>
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
	static constexpr uint32_t kProbeBinding = 44;
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
	// Probe capture is sliced: the probes of one scene are captured a few per frame.
	bool IsCapturing() const { return m_captureActive; }
	static void BindForShading(const RenderContext& ctx);

private:
	// One slice of the capture: probes are captured until budgetMs is spent (budgetMs < 0: all of them).
	void CaptureAll(RenderContext& ctx, const std::shared_ptr<SceneGraph>& sceneGraph, const std::shared_ptr<Skybox>& skybox, double budgetMs);
	static constexpr double kCaptureBudgetMs = 6.0;
	void CaptureProbe(RenderContext& ctx, const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Skybox>& skybox, const ReflectionProbeDesc& probe, int index);
	void PrefilterInto(int index);
	// Uploads the probes whose influence box intersects the view frustum (all when viewProj is
	// null) and returns how many were uploaded. Array layers are preserved in position.w.
	int UploadProbeData(const std::vector<ReflectionProbeDesc>& probes, const glm::mat4* viewProj);

	rhi::ProgramHandle m_forwardShader;
	rhi::ProgramHandle m_prefilterShader;
	rhi::FramebufferHandle m_captureFBO;
	rhi::TextureHandle m_captureDepth;      // face-sized D32F depth target (was a renderbuffer)
	rhi::TextureHandle m_captureCube;       // temporary capture target with mips
	rhi::TextureHandle m_probeArray;        // cube array, RGBA16F
	rhi::BufferHandle m_probeSSBO;
	rhi::VertexInputHandle m_cubeVAO;
	rhi::BufferHandle m_cubeVBO;
	rhi::BufferHandle m_captureExposureSSBO; // pre-exposure 1 during capture
	rhi::BufferHandle m_disabledClusterUBO;  // clustering off during capture
	int m_capacity = 0;
	const void* m_lastScene = nullptr;
	int m_framesUntilCapture = -1;
	int m_probeCount = 0;
	bool m_captureActive = false;   // a capture of m_probeCount probes is in progress
	int m_nextProbe = 0;            // next probe to capture while m_captureActive
	std::shared_ptr<SceneGraph> m_captureScene;   // scene and skybox of the capture in progress
	std::shared_ptr<Skybox> m_captureSkybox;
	rhi::FramebufferHandle m_prevFBO;             // state to restore when the capture finishes
	rhi::Rect m_prevViewport{};
};
