#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include <memory>

class ComputeShader;

/*
* Stochastic screen-space reflections (docs/nfs2015/README.md, M5).
*
* Runs after the G-buffer and velocity passes and before lighting, tracing against last frame's
* resolved colour (as Frostbite does):
*   1. copy last frame's TAA output into a mipmapped texture (cone-filtered fetches)
*   2. trace: half resolution, one GGX-VNDF sampled ray per pixel (shaders/ssr_trace_comp.glsl)
*   3. resolve: full resolution, 4-ray reuse with BRDF/pdf weights (shaders/ssr_resolve_comp.glsl)
*   4. temporal: hit-depth reprojection + neighbourhood clamp (shaders/ssr_temporal_comp.glsl)
*   5. glossy blur: dual-filter Kawase chain over the temporal output, premultiplied by confidence
*      (shaders/ssr_blur_down_comp.glsl, shaders/ssr_blur_up_comp.glsl; Bjorge, SIGGRAPH 2015)
*   6. composite: each pixel samples the chain at the level its roughness and hit distance pick,
*      un-premultiplies, and writes ctx.ssrTexture (shaders/ssr_composite_comp.glsl)
* Output: ctx.ssrTexture (RGBA16F, rgb = reflected radiance at this frame's pre-exposure,
* a = confidence). The lighting pass blends it over the probe/sky specular radiance inside the
* split-sum so energy is not double counted. Requires TAA (its output is the colour history).
* The blur chain only feeds the composite; the temporal history stays sharp, so blur never compounds.
*/
class SSRPass : public RenderPass {
public:
	SSRPass();
	~SSRPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

	void ResetHistory() { m_historyValid = false; }

private:
	void CreateTextures(int width, int height);
	void DestroyTextures();
	void SetCommonUniforms(rhi::ProgramHandle program, const RenderContext& ctx);

	std::unique_ptr<ComputeShader> m_traceShader;
	std::unique_ptr<ComputeShader> m_resolveShader;
	std::unique_ptr<ComputeShader> m_temporalShader;
	std::unique_ptr<ComputeShader> m_blurDownShader;
	std::unique_ptr<ComputeShader> m_blurUpShader;
	std::unique_ptr<ComputeShader> m_compositeShader;
	static constexpr int kBlurLevels = 7;
	rhi::TextureHandle m_traceTex;      // half res RGBA32F
	rhi::TextureHandle m_resolveTex;    // full res RGBA16F
	rhi::TextureHandle m_hitDistTex;    // full res R16F
	rhi::TextureHandle m_historyTex[2];
	rhi::TextureHandle m_blurChainTex;  // half res RGBA16F, kBlurLevels mips: rgb*a, a (premultiplied)
	rhi::TextureHandle m_compositeTex;  // full res RGBA16F: ctx.ssrTexture
	rhi::TextureHandle m_prevColorTex;  // full res RGB16F, mipmapped
	int m_prevColorMips = 1;
	int m_width = 0, m_height = 0;
	int m_historyIndex = 0;
	bool m_historyValid = false;
	unsigned m_frame = 0;
};
