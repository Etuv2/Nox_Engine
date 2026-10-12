#pragma once
// IGameModule adapter for Lua projects (contract C4, docs/lua_api.md). Core selects it through the
// project runtime registry: GameModules::RegisterProjectRuntime("lua", NoxLua::CreateLuaProjectModule).
#include "../GameModule.h"
#include "LuaInput.h"
#include "LuaProject.h"
#include "LuaUi.h"
#include "LuaReload.h"
#include "LuaWorld.h"

#include <memory>
#include <string>
#include <vector>

namespace NoxLua {

	// Shared with editor undo closures: they reach the module's world only through a live link.
	struct LuaEditorLink {
		LuaWorld* world = nullptr;
	};

	class LuaGameModule : public IGameModule {
	public:
		static constexpr double kWatchPollSeconds = 0.25;
		static constexpr double kWatchDebounceSeconds = 0.4;

		// `files` defaults to AssetIO (AssetScriptFiles); tests inject memory files.
		explicit LuaGameModule(ProjectDescriptor descriptor, std::unique_ptr<ScriptFiles> files = nullptr);
		~LuaGameModule() override;

		void SetStandalone(bool standalone) override { m_standalone = standalone; }
		void SetCameraTemplate(const Camera& camera) override;
		void SetServices(const GameModuleServices& services) override;
		// Loads project fonts (ImGui atlas), the icon atlas and the action map once, on the GUI thread.
		void InitializeResources() override;
		// Runs the draw handlers and plays their commands onto the game overlay.
		void DrawOverlay(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, float dt) override;
		bool WantsPointer() const override { return m_world ? m_world->WantsPointer() : true; }

		void Configure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& scene) override;
		// Authoring edits while not playing: a world with nox.on_configure is updated in place (keeps editor undo
		// steps and world state); otherwise the world is rebuilt like Configure.
		void Reconfigure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& scene) override;
		// The project's nox.on_save_game value when it registered one (and it works), else the scene's game object.
		nlohmann::json SaveGameJson() const override;
		bool IsAvailable() const override { return m_available; }

		void BeginPlay() override;
		void EndPlay() override;
		void SetPaused(bool paused) override { m_paused = paused; }
		bool IsPaused() const override { return m_paused; }

		UpdateResult Update(float dt, bool inputEnabled, float aspect) override;
		bool ConsumeTeleport() override;
		std::shared_ptr<Camera> GetCamera() const override { return m_camera; }

		// Transactional reload (docs/lua_api.md "Reload"). RequestReload is applied at the next frame boundary
		// (Update). A failed candidate leaves the running code, declared state and native world unchanged.
		bool SupportsReload() const override { return true; }
		// Queues a reload for the next frame boundary. False (with the reason in *error) when no project world
		// is loaded. Packed distributions reach this only through the editor, which is absent there.
		bool RequestReload(std::string* error = nullptr) override;
		// Loose-file watcher (polls module stamps, debounced). Enabled by default only in Loose mode with
		// ProjectBootstrap reloadEnabled; tests and tools may force it.
		void SetWatchEnabled(bool enabled) { m_watchEnabled = enabled; }
		bool WatchEnabled() const { return m_watchEnabled; }
		void SetRuntimeLimits(const RuntimeLimits& limits) { m_limits = limits; }

		// Diagnostics of the current world (load errors, faulted systems, warnings) for the editor.
		const std::vector<Diagnostic>& Diagnostics() const;
		// Reload outcomes (applied and rejected), oldest first.
		const std::vector<Diagnostic>& ReloadLog() const { return m_reloadLog; }
		int ReloadCount() const { return m_reloadCount; }
		int RejectedReloadCount() const { return m_reloadRejected; }
		// Editor integration (LuaEditor.h). The handles and link are owned by the module; undo closures hold them weakly.
		void DrawEditorPanelById(const std::string& id, EditorHost& host);
		void DrawEditorViewport(EditorHost& host);
		std::weak_ptr<LuaEditorLink> EditorLinkRef() const { return m_link; }
		EditorHandles& Handles() { return *m_handles; }
		const LuaWorld* World() const { return m_world.get(); }
		LuaWorld* MutableWorld() { return m_world.get(); }
		const ProjectDescriptor& Project() const { return m_descriptor; }

	private:
		void RegisterSubsteps();
		void ClearSubsteps();
		void ApplyReload();
		void RecordRejectedReload(const std::string& reason);
		void PollWatcher(float dt);
		void AttachResources(LuaWorld* world);
		void PublishAudio();

		ProjectDescriptor m_descriptor;
		PhysicsEngine* m_physics = nullptr;   // engine service; valid until the next SetServices
		bool m_substepsRegistered = false;
		// Declared before the world: staged candidates read through these files while they live.
		std::unique_ptr<ScriptFiles> m_files;
		RuntimeLimits m_limits;
		// UI and input resources are owned here (before the world, which keeps pointers into them).
		UiFontTable m_fonts;
		UiTextureCache m_textures;                      // ui.texture images; released with the module
		InputMap m_inputs;
		UiRecorder m_recorder;
		std::vector<std::string> m_resourceMessages;   // load problems, replayed into each world as diagnostics
		bool m_resourcesReady = false;
		std::shared_ptr<EditorHandles> m_handles;
		std::shared_ptr<LuaEditorLink> m_link;
		ProceduralVehicleAudio m_audio;                 // engine audio service; published by PublishAudio
		bool m_inputsLoaded = false;                    // the action map parses without GPU/ImGui: loaded at Configure
		std::unique_ptr<LuaWorld> m_world;
		nlohmann::json m_game;
		SceneDocuments m_docs;                 // game object and scene root handed to every staged world
		nlohmann::json m_authoredEntities;
		SceneGraph* m_scene = nullptr;
		std::shared_ptr<Camera> m_camera;
		std::vector<ModuleStamp> m_stamps;     // module set of the running world (watcher baseline)
		bool m_available = false;
		bool m_paused = false;
		bool m_standalone = false;

		bool m_reloadRequested = false;
		int m_reloadCount = 0;
		int m_reloadRejected = 0;
		std::vector<Diagnostic> m_reloadLog;

		bool m_watchEnabled = false;
		double m_pollElapsed = 0.0;
		double m_stableElapsed = 0.0;
		std::vector<ModuleStamp> m_observed;   // last changed set seen by the watcher
	};

	// Factory for GameModules::RegisterProjectRuntime("lua", ...). Returns null and sets error when the
	// descriptor is invalid. Also registers the Lua editor panel (once).
	std::unique_ptr<IGameModule> CreateLuaProjectModule(const std::string& descriptorPath,
		const nlohmann::json& descriptor, std::string& error);
}
