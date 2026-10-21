#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include <cstdint>
#include <memory>

class ComputeShader;
class FrameBuffer;

/*
* Lens flares on visible bright sources (SceneRenderExtras::lensFlares).
*
* Runs after TAA, motion blur, auto exposure and bloom, before tone mapping:
*   1. Visibility (compute): per source, a 13-tap depth disc around the projected source gives soft
*      partial occlusion; the result is eased over time in an SSBO (no pops, no CPU readback).
*   2. Sprites: up to four camera-facing layers per source as described by its style (texture layer,
*      gain, width, tint, glare dependence), screen-space quads with min/max pixel sizes and distance
*      and size-based energy fades, added (ONE, ONE blend, premultiplied) to the linear pre-exposed HDR
*      image. No depth test or writes: occlusion comes from step 1, so rays are never clipped by
*      nearby silhouettes.
* Drawing after TAA avoids history ghosting on moving sprites; drawing after bloom keeps glare from
* being counted twice. Flares go into the pass's own copy of the post source, so TAA history and
* SSR's previous-frame colour never contain camera glare.
*/
class LensFlarePass : public RenderPass {
public:
	LensFlarePass();
	~LensFlarePass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	// Prepare phase (after the scene commit, inside a load slice): builds the flare sprite array of the scene.
	void PrepareScene(const std::shared_ptr<SceneGraph>& sceneGraph);
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

private:
	void CreateTarget(int width, int height);

	rhi::ProgramHandle m_drawShader;
	std::unique_ptr<ComputeShader> m_visibilityShader;
	std::unique_ptr<FrameBuffer> m_output;   // RGBA16F copy of the post source with flares added
	rhi::VertexInputHandle m_vao;
	rhi::BufferHandle m_sourceSSBO;
	rhi::BufferHandle m_visibilitySSBO;
	rhi::BufferHandle m_styleSSBO;
	int m_count = 0;
	int m_styleCount = 0;
	uint64_t m_revision = 0;
	bool m_resetVisibility = true;
};
