#pragma once
// Engine-side contract for an optional game layer (gameplay, its camera and its overlay UI).
//
// Core owns at most one module, created through the factory the application registers in main()
// before the window opens. Without a factory the engine runs scenes as a plain viewer/editor.
// The engine never includes a concrete game header; everything it needs goes through this
// interface, and game data stays in the scene's "game" JSON object, which the engine passes
// through without interpreting it.
#include <functional>
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include "json.hpp"

class Camera;
class SceneGraph;
class PhysicsEngine;
struct ImDrawList;

// Engine services a module may use. Pointers stay valid until the next SetServices call or the
// module's destruction; a module must drop any callbacks it registered with them in its destructor.
struct GameModuleServices {
	PhysicsEngine* physics = nullptr;
};

class IGameModule {
public:
	enum class UpdateResult { Continue, ExitApplication };

	virtual ~IGameModule() = default;

	// Once, after the GPU device and ImGui exist (fonts, UI atlases, audio).
	virtual void InitializeResources() {}
	// Standalone = full-window game launch (no editor).
	virtual void SetStandalone(bool standalone) { (void)standalone; }
	// Called after creation (and again if a service is replaced), before Configure.
	virtual void SetServices(const GameModuleServices& services) { (void)services; }
	// Projection and clip settings for the game camera, copied from the engine camera.
	virtual void SetCameraTemplate(const Camera& camera) { (void)camera; }

	// (Re)build game state from the scene's "game" object; sceneRoot is the whole scene file.
	virtual void Configure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& scene) = 0;
	// Re-apply edited authoring data while editing; keeps editor-side selection by default.
	virtual void Reconfigure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& scene) {
		Configure(game, sceneRoot, scene);
	}
	virtual nlohmann::json SaveGameJson() const = 0;
	// False when the loaded scene has nothing playable; Core then ignores the module.
	virtual bool IsAvailable() const = 0;
	// Start playing right after load even in the editor (e.g. a module-specific test switch).
	virtual bool WantsAutoPlay() const { return false; }

	virtual void BeginPlay() {}
	virtual void EndPlay() {}
	virtual void SetPaused(bool paused) { (void)paused; }
	virtual bool IsPaused() const { return false; }

	// inputEnabled is false while another UI owns the keyboard/pointer (unfocused editor viewport).
	virtual UpdateResult Update(float dt, bool inputEnabled, float aspect) = 0;
	// True once after a discontinuous camera/object move (temporal history must be reset).
	virtual bool ConsumeTeleport() { return false; }
	virtual std::shared_ptr<Camera> GetCamera() const = 0;
	// Optional: the player's world position (used by the editor smoke test). False when none.
	virtual bool GetPlayerPose(glm::vec3& pos) const { (void)pos; return false; }
	// Overlay drawn on top of the game view (origin/size in ImGui display coordinates).
	virtual void DrawOverlay(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, float dt) {
		(void)drawList; (void)origin; (void)size; (void)dt;
	}
	// Code reload for modules whose gameplay is content (project scripts). Returns false when the
	// module has no reloadable code; failures keep the running code and state (reason in error).
	virtual bool SupportsReload() const { return false; }
	virtual bool RequestReload(std::string* error = nullptr) { if (error) *error = "module has no reloadable code"; return false; }
	// The module wants an absolute, visible pointer (menus, maps); otherwise the host may capture it.
	virtual bool WantsPointer() const { return true; }
};

using GameModuleFactory = std::function<std::unique_ptr<IGameModule>()>;
// Creates the module for a project descriptor (logical path, parsed JSON). Registered per "runtime".
using ProjectModuleFactory = std::function<std::unique_ptr<IGameModule>(const std::string& descriptorPath,
	const nlohmann::json& descriptor, std::string& error)>;

namespace GameModules {
	// Application-native module used for scenes that name no project (transitional, optional).
	void SetFactory(GameModuleFactory factory);
	// Null when no factory was registered.
	std::unique_ptr<IGameModule> Create();

	// Project runtimes (e.g. "lua"): a scene's "project" key or --project selects a descriptor
	// (projects/<name>/project.json, docs/asset_io.md); its "runtime" picks the factory.
	void RegisterProjectRuntime(const std::string& runtime, ProjectModuleFactory factory);
	// Reads the descriptor through AssetIO. Null with a reason when it is missing/invalid/unknown.
	std::unique_ptr<IGameModule> CreateForProject(const std::string& descriptorPath, std::string& error);
}
