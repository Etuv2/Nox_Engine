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
 * @brief Ray Tracing Pass using compute shaders
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
	std::unique_ptr<ComputeShader> m_rtShader;
	std::unique_ptr<FrameBuffer> m_rtFBO;
	TexturePtr m_rtTexture;

};
