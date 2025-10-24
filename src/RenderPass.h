#pragma once
#include <memory>



// Forward declarations
class SceneGraph;
class Camera;
class DirectionalLight;
class Skybox;


struct RenderContext;


class RenderPass {
public:
	virtual ~RenderPass() = default;
	virtual bool Initialize(RenderContext& context) = 0;
	virtual void Resize(RenderContext& context, int newWidth, int newHeight) = 0;
	virtual void Execute(RenderContext& ctx,
		const std::shared_ptr<SceneGraph>& sceneGraph,
		const std::shared_ptr<Camera>& camera,
		const std::shared_ptr<DirectionalLight>& dirLight,
		const std::shared_ptr<Skybox>& skybox) = 0;
};