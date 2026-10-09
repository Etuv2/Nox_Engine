#pragma once
#include "../RenderPass.h"
#include <GL/glew.h>
#include <memory>

class ComputeShader;

/*
* Clustered light culling (docs/nfs2015/README.md, M2).
*
* Splits the view frustum into screen tiles x exponential depth slices and builds a list of
* point/spot lights per cluster (shaders/light_cluster_build_comp.glsl). Consumers include
* shaders/includes/clustered_lighting.glsl and call BindForShading(ctx) before drawing.
*
* Resources: UBO binding 8 (ClusterParams), SSBO 40 (per-cluster counts),
* SSBO 41 (per-cluster light indices, fixed NOX_CLUSTER_MAX_LIGHTS slots per cluster).
*/
class LightClusterPass : public RenderPass {
public:
	static constexpr GLuint kParamsBinding = 8;
	static constexpr GLuint kCountBinding = 40;
	static constexpr GLuint kIndexBinding = 41;
	static constexpr unsigned kMaxLightsPerCluster = 128u; // matches NOX_CLUSTER_MAX_LIGHTS
	static constexpr unsigned kTileSize = 64u;
	static constexpr unsigned kSlices = 32u;

	LightClusterPass();
	~LightClusterPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

	// Binds the cluster UBO/SSBOs recorded in the context (no-op when clustering is off, in which
	// case the UBO reports enabled = 0 and shaders fall back to looping every light).
	static void BindForShading(const RenderContext& ctx);

private:
	void AllocateBuffers(int width, int height);

	std::unique_ptr<ComputeShader> m_buildShader;
	GLuint m_paramsUBO = 0;
	GLuint m_countSSBO = 0;
	GLuint m_indexSSBO = 0;
	unsigned m_gridX = 0, m_gridY = 0;
};
