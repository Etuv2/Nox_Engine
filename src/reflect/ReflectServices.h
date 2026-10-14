#pragma once
// Engine services reachable from reflection descriptors. Descriptors only receive a SceneNode; anything
// that needs an engine system (load a model, talk to the physics world, play audio) goes through here.
// Core fills these in before the first scene loads and clears them on shutdown; every field may be empty
// (headless tests, tools), so descriptors must check before use and fail the set()/invoke() instead.

#include "../json.hpp"

#include <functional>
#include <memory>
#include <string>

class Scene;
class SceneGraph;
class SceneNode;
class PhysicsEngine;
struct RenderContext;

namespace nox::reflect {

struct EngineServices {
	// Loads (or returns the cached) model for a logical asset path; null on failure.
	std::function<std::shared_ptr<Scene>(const std::string& path)> loadModel;
	// Logical asset path a loaded model came from; empty when unknown.
	std::function<std::string(const Scene& model)> modelPath;
	PhysicsEngine* physics = nullptr;
	// The active scene graph (changes on scene switch); null when no scene is loaded.
	std::function<SceneGraph*()> sceneGraph;
	// Call after an edit that changes what is rendered or saved (bounds, BVH, dirty flag).
	std::function<void()> sceneChanged;
	// True while the editor is in Play mode.
	std::function<bool()> isPlaying;
	// Requests a scene switch at the next frame boundary (Lua ctx:load_scene). Logical scene path.
	std::function<void(const std::string& scenePath)> requestSceneSwap;
	// The live renderer settings of the active scene (WorldEnvironment; SceneRenderSettings.h keys). Null
	// when no renderer exists (headless tests may supply a stub).
	std::function<RenderContext*()> renderContext;
	// Instantiates a *.prefab.json under parent (null = root) with placement keys (name, position, rotation, scale,
	// components). Returns the instance root or null with error set. Lua ctx:instantiate.
	std::function<std::shared_ptr<SceneNode>(const std::string& path, const std::shared_ptr<SceneNode>& parent,
		const nlohmann::json& placement, std::string& error)> instantiatePrefab;
	// Removes a scene node subtree during play (Lua ctx:destroy on a node). False when refused.
	std::function<bool(const std::shared_ptr<SceneNode>& node)> destroyNode;
};

EngineServices& Services();

inline void NotifySceneChanged() {
	if (Services().sceneChanged) Services().sceneChanged();
}

} // namespace nox::reflect
