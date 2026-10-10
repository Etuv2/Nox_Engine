#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include <memory>

class ComputeShader;
class FrameBuffer;

/*
* Camera/object motion blur (docs/nfs2015/README.md, M7): McGuire et al. 2012 reconstruction filter
* (tile max -> neighbour max -> depth-aware gather) on the TAA-resolved image.
*
* Blur length follows a physical shutter: velocity * (exposureTime / frameTime), clamped to
* ctx.motionBlurMaxPixels, so the look does not change with frame rate. The result is published as
* ctx.postSourceFBO for bloom, auto exposure and tone mapping; TAA/SSR history stays unblurred.
*/
class MotionBlurPass : public RenderPass {
public:
	MotionBlurPass();
	~MotionBlurPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

private:
	void CreateTargets(int width, int height);

	std::unique_ptr<ComputeShader> m_tileShader;
	std::unique_ptr<ComputeShader> m_gatherShader;
	std::unique_ptr<FrameBuffer> m_output;  // RGBA16F, sampled by bloom/post as postSourceFBO
	rhi::TextureHandle m_tileMax;
	rhi::TextureHandle m_neighborMax;
	int m_tileW = 0, m_tileH = 0;
	unsigned m_frame = 0;
	static constexpr int kTileSize = 16;
};
