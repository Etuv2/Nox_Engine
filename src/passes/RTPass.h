#pragma once
#include "../RenderPass.h"
#include "../Texture.h"
#include <memory>
#include <GL/glew.h>

// Forward declarations
class FrameBuffer;
class RenderContext;
class ComputeShader;

/**
 * @brief RTX Pass using compute shaders
 * IMPLEMENTATION: Bidirectional Path Tracing with importance sampling
 * Makes use of Bounding Volume Hierarchy (BVH) for ray-scene intersection acceleration
 * REFERENCES:
 * - www.pbr-book.org. (n.d.). Bounding Volume Hierarchies: https://www.pbr-book.org/3ed-2018/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies
 * - ‌Pbr-book.org. (2025). Bidirectional Path Tracing. : https://www.pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/Bidirectional_Path_Tracing
 */
class RTPass : public RenderPass {
public:
	RTPass() = default;
	~RTPass() override;

	bool Initialize(RenderContext& context) override;
	void Resize(RenderContext& context, int newWidth, int newHeight) override;
	void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) override;


private:

	// Compute shader program for ray tracing
	std::unique_ptr<ComputeShader> m_rtShader;

	// Framebuffer for ray traced output
	std::unique_ptr<FrameBuffer> m_rtFBO;

	// Ray traced output texture
	TexturePtr m_rtTexture;

	// Resolution
	int m_w = 0, m_h = 0;        // Full resolution (G-buffer resolution)
	int m_hw = 0, m_hh = 0;    // Working resolution (half or full based on ssgiHalfRes)
	int m_qw = 0, m_qh = 0;      // Quarter resolution (for efficient denoising)
	float m_renderResolutionScale = 1.0f; // Scale factor for render resolution

	//flag to indicate if BVH needs to be rebuilt
	bool m_bvhDirty = true;

	/*
	* Warmup stage where BVH is built and shaders are prepped
	*/
	void runWarmup(const std::shared_ptr<SceneGraph>& sceneGraph);

	/*
	* Ray tracing execution stage
	*/
	void runRayTracing(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera);

	/*
	* Accumulate frame results for progressive refinement
	*/
	void accumulateFrame(RenderContext& ctx);


};
