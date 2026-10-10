#pragma once
#include "../RenderPass.h"
#include "../rhi/RHI.h"
#include <cstdint>
#include <vector>

/*
* Surface decals (SceneRenderExtras::decalSets). Runs right after the G-buffer pass: instanced quads,
* depth tested read-only against the scene with a small polygon offset (no z-fighting; geometry in
* front of the decal occludes it), alpha coverage blended into the G-buffer albedo (RT1.rgb) and
* roughness (RT0.b) only, with the set's surface response (roughness, wetness). Everything downstream
* (lighting, SSR, probes, GI, TAA) sees the decal as part of the underlying non-emissive surface.
* One draw per set; each set has its own texture array.
*/
class DecalPass : public RenderPass {
public:
	DecalPass() = default;
	~DecalPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext&, int, int) override {}
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;

private:
	struct SetRange { uint32_t firstInstance = 0; int count = 0; };

	rhi::ProgramHandle m_shader;
	rhi::VertexInputHandle m_vao;
	rhi::BufferHandle m_cornerVbo;
	rhi::BufferHandle m_instanceVbo;
	std::vector<SetRange> m_ranges;   // per decal set, into the shared instance buffer
	uint64_t m_revision = 0;
};
