#pragma once
// Scene-driven view camera (Unity Camera.main / Godot Camera3D.current).
//
// A node with a Camera component whose "current" flag is set drives the render camera while the editor
// is in Play mode: the render camera copies the node's world transform and its fov, near and far plane
// every frame. Editor mode keeps the editor camera. Lua switches views by setting "current".

#include <memory>

class Camera;
class SceneGraph;
class SceneNode;

namespace nox::render {

// First node in pre-order traversal whose Camera component has current set; null when none.
SceneNode* FindActiveCameraNode(SceneNode& root);

// Copies the active camera node into the render camera. Returns false (camera untouched) when no node
// is active. The lens is driven as a perspective projection.
bool ApplyActiveCamera(SceneGraph& graph, Camera& camera);

} // namespace nox::render
