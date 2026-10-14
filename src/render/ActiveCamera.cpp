#include "ActiveCamera.h"

#include "../Camera.h"
#include "../ComponentTypes.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"

namespace nox::render {

namespace {

SceneNode* FindRecursive(SceneNode& node) {
	const CameraComponent* camera = node.GetCameraComponent();
	if (camera && camera->current) return &node;
	for (const std::shared_ptr<SceneNode>& child : node.children) {
		if (!child) continue;
		if (SceneNode* found = FindRecursive(*child)) return found;
	}
	return nullptr;
}

} // namespace

SceneNode* FindActiveCameraNode(SceneNode& root) {
	return FindRecursive(root);
}

bool ApplyActiveCamera(SceneGraph& graph, Camera& camera) {
	const std::shared_ptr<SceneNode> root = graph.GetRoot();
	if (!root) return false;
	SceneNode* node = FindActiveCameraNode(*root);
	if (!node) return false;
	const CameraComponent* lens = node->GetCameraComponent();
	if (!lens) return false;

	camera.SetWorldTransform(node->GetWorldPosition4x4());
	camera.SetPerspective(lens->fov, camera.GetAspect(), lens->nearPlane, lens->farPlane);
	return true;
}

} // namespace nox::render
