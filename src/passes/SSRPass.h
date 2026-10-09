#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
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
* Output: ctx.ssrTexture (RGBA16F, rgb = reflected radiance at this frame's pre-exposure,
* a = confidence). The lighting pass blends it over the probe/sky specular radiance inside the
* split-sum so energy is not double counted. Requires TAA (its output is the colour history).
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
	void SetCommonUniforms(GLuint program, const RenderContext& ctx);

	std::unique_ptr<ComputeShader> m_traceShader;
	std::unique_ptr<ComputeShader> m_resolveShader;
	std::unique_ptr<ComputeShader> m_temporalShader;
	GLuint m_traceTex = 0;      // half res RGBA32F
	GLuint m_resolveTex = 0;    // full res RGBA16F
	GLuint m_hitDistTex = 0;    // full res R16F
	GLuint m_historyTex[2] = { 0, 0 };
	GLuint m_prevColorTex = 0;  // full res RGB16F, mipmapped
	int m_prevColorMips = 1;
	int m_width = 0, m_height = 0;
	int m_historyIndex = 0;
	bool m_historyValid = false;
	unsigned m_frame = 0;
};
