#pragma once
// One Lua VM bound to one scene's ECS: project schemas, ordered systems, events, behaviors, deferred
// native commands and the `nox` API (docs/lua_api.md, API version 1).
//
// Ownership: LuaWorld owns the Runtime (one VM per active project/play world). It never keeps raw
// component pointers; every native access resolves an EntityHandle against the ComponentManager at
// call time. Reads return copies. Native writes are queued as commands and applied after the callback
// that queued them returns normally; a callback that fails has its queued commands discarded.
#include "LuaData.h"
#include "LuaEditor.h"
#include "LuaInput.h"
#include "LuaRuntime.h"
#include "LuaServices.h"
#include "LuaUi.h"
#include "LuaVehicleBindings.h"
#include "../ComponentManager.h"
#include "json.hpp"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

struct lua_State;
class SceneGraph;
class Camera;

namespace NoxLua {

	struct LuaApiBinder;

	// Declared project data types. Native ComponentType is not involved.
	enum class FieldType { Number, Integer, Boolean, String };
	// Stored value keeps the Lua subtype: integers stay integers, floats stay floats.
	using Value = std::variant<int64_t, double, bool, std::string>;
	using FieldMap = std::map<std::string, Value>;
	using SchemaMap = std::map<std::string, FieldMap>;

	struct Schema {
		std::string name;
		std::map<std::string, FieldType> fields;
		FieldMap defaults;
		int version = 1;   // declared schema version; a change needs nox.migrate for stored data
	};

	// Bounded, versioned copy of the declared gameplay state of one world (reload/restore, prompt section 4).
	// Holds only typed values, handles and sets: no Lua objects, so it cannot contain functions, userdata,
	// metatables or cycles. Taken at a frame boundary; imported into a candidate world.
	struct StateSnapshot {
		static constexpr int kFormat = 1;
		static constexpr size_t kMaxValues = 100000;
		int format = kFormat;
		std::map<std::string, int> schemaVersions;          // schema -> version when captured
		std::map<EntityHandle, SchemaMap> data;             // live declared records
		std::map<EntityHandle, std::set<std::string>> attachments;
		std::map<EntityHandle, SchemaMap> authored;         // authored records (Stop restores them)
		std::map<EntityHandle, std::set<std::string>> authoredAttachments;
		std::vector<EntityHandle> spawned;
		std::map<EntityHandle, glm::mat4> touched;          // original transforms of moved entities
		uint64_t frame = 0;
		double playTime = 0.0;
		bool playing = false;
		size_t ValueCount() const;
	};

	// Failure or warning, visible to the editor (phase 2 panel) and tests.
	struct Diagnostic {
		enum class Severity { Warning, Error };
		Severity severity = Severity::Error;
		std::string source;      // "load", "system:<name>", "event:<name>", "behavior:<name>", "authored"
		std::string file;        // virtual file parsed from the message ("projects/x/scripts/main.lua")
		int line = 0;
		std::string message;     // first line of the error
		std::string traceback;   // stack traceback lines, may be empty
		uint64_t frame = 0;      // frame counter when raised (0 during load)
	};

	// Project content handed to LuaWorld (filled by LuaProject).
	struct ProjectContent {
		std::string name;
		std::string entry;                       // logical path of the entry module
		std::vector<std::string> scriptRoots;    // logical directories, searched in order
		std::vector<DeclaredFile> roads;         // project.json "roads" (nox.roads)
		std::vector<DeclaredFile> collision;     // project.json "collision" (nox.collision)
		std::string descriptorPath;              // logical path of project.json ("" when built by hand)
		std::string projectDir;                  // its directory ("" when unknown)
		nlohmann::json descriptor;               // decoded project.json (nox.project)
		std::vector<std::string> authoringRoots; // project.json "authoring_roots" (nox.save_authoring_json), repo-logical
	};

	// The scene's configured documents, copied into each world: the "game" object and the whole scene (nox.game, nox.scene).
	struct SceneDocuments {
		nlohmann::json game;
		nlohmann::json root;
	};

	class LuaWorld {
	public:
		struct Command {
			enum class Kind { Spawn, Destroy, Move, Teleport, SetScale };
			Kind kind = Kind::Move;
			EntityHandle target;
			glm::vec3 value{ 0.0f };
			std::string name;      // spawn name
			int64_t token = 0;     // spawn token
		};

		// `owned` (optional) is destroyed after the VM: a staged candidate keeps its module snapshot alive.
		LuaWorld(const RuntimeLimits& limits, ScriptSource* source, std::unique_ptr<ScriptSource> owned = nullptr);
		~LuaWorld();
		LuaWorld(const LuaWorld&) = delete;
		LuaWorld& operator=(const LuaWorld&) = delete;

		// Builds the VM and runs the entry module (schemas, systems, events, behaviors are registered there),
		// then applies authored data. Returns false when the entry failed (see Diagnostics). The scene must
		// outlive this world (LuaGameModule reconfigures on every scene load).
		bool Load(const ProjectContent& project, SceneGraph& scene, const nlohmann::json& authoredEntities);
		bool IsLoaded() const { return m_loaded; }

		// Lifecycle (driven by LuaGameModule). Update runs systems, events and behaviors once.
		void BeginPlay();
		void EndPlay();
		void Update(float dt, bool inputEnabled);
		// Physics substep boundary (LuaGameModule registers it with PhysicsEngine::SetSubstepCallback while
		// playing). Runs the systems declared with fixed = true, in order, once per fixed step.
		void FixedUpdate(float dt, int index);
		bool ConsumeTeleport();

		// Draw handlers (nox.on_draw, docs/lua_api.md "UI"). Runs every handler with (ctx, ui, dt) into `out`
		// (cleared first). Draw handlers may read the world; world writes they queue are discarded with a warning.
		// `pointerOverlay` is the pointer relative to the overlay origin. Faulted handlers are skipped until reload.
		void DrawFrame(float dt, const glm::vec2& viewport, const glm::vec2& pointerOverlay, UiRecorder& out);
		// Project resources owned by the module (not by this world): fonts for ui.text, the action map for ctx:action_*.
		void SetUiFonts(const UiFontTable* fonts) { m_ui.fonts = fonts; }
		void SetInputs(const InputMap* inputs) { m_inputs = inputs; }
		// Pointer policy the module reports through IGameModule::WantsPointer (set from Lua with ctx:set_wants_pointer).
		bool WantsPointer() const { return m_wantsPointer; }
		void SetWantsPointer(bool wants) { m_wantsPointer = wants; }
		// Module camera for ctx:camera_* (owned by the module) and the aspect ratio of the current frame.
		void SetCamera(Camera* camera) { m_services.camera = camera; }
		void SetAspect(float aspect) { m_services.aspect = aspect; }
		// Game overlay rectangle of the frame being drawn (ctx:overlay_rect), in window coordinates.
		void SetOverlayRect(const glm::vec2& origin, const glm::vec2& size) {
			m_services.overlayOrigin = origin;
			m_services.overlaySize = size;
		}
		// Image cache for ui.texture, owned by the module (null disables ui.texture).
		void SetTextures(UiTextureCache* textures) { m_ui.textures = textures; }
		// Scene documents for nox.game / nox.scene and the on_configure hook. Call before Load.
		void SetSceneDocuments(const nlohmann::json& game, const nlohmann::json& root);
		// nox.on_configure hook: runs once per load of this world with (game, scene). False when it fails.
		bool RunConfigureHook(std::string& error);
		bool HasConfigureHook() const { return m_configureRef != 0; }
		// nox.on_save_game hook: the project's authoring JSON for SaveGameJson. False (with reason) when there is no hook,
		// it failed, or the value is not representable as JSON.
		bool HasSaveGameHook() const { return m_saveGameRef != 0; }
		bool SaveGameJson(nlohmann::json& out, std::string& error);
		// ctx:request_exit() was called since this world was created.
		bool ExitRequested() const { return m_dataApi.exitRequested; }
		// Reload staging (LuaReload): authoring writes are refused in a candidate world.
		void SetStaging(bool staging) { m_dataApi.staging = staging; }
		// Editor panels and the viewport overlay declared by the project (nox.editor_panel, nox.editor_viewport).
		// The panel draws with `ed`; ed.handle needs the viewport. Both run on the GUI thread through the module.
		struct EditorPanelDecl {
			std::string id;
			std::string title;
			int ref = 0;
			bool faulted = false;
		};
		const std::vector<EditorPanelDecl>& EditorPanels() const { return m_editorPanels; }
		bool HasEditorViewport() const { return m_editorViewportRef != 0; }
		bool DrawEditorPanel(const std::string& id, EditorHost* host, EditorHandles* handles);
		bool DrawEditorViewport(EditorHost* host, EditorHandles* handles);
		// Runs one ed.undo step (undo or redo function reference) on the editor's stack. Never faults the world.
		void RunEditorUndo(int ref, const std::string& label);
		// Declared handles of a schema (inspector).
		std::vector<EntityHandle> DeclaredHandles(const std::string& schema) const { return DataHandlesWithSchema(schema); }
		LuaServices& Services() { return m_services; }
		VehicleHost& Vehicles() { return *m_vehicles; }
		const LuaServices& Services() const { return m_services; }

		// Diagnostics and faults. A failed system/behavior/handler is disabled until RetryFaulted().
		const std::vector<Diagnostic>& Diagnostics() const { return m_diagnostics; }
		std::vector<std::string> FaultedNames() const;
		void RetryFaulted();
		std::vector<std::string> SystemNames() const;

		// State inspection (tests, editor).
		const std::map<std::string, Schema>& Schemas() const { return m_schemas; }
		uint64_t Frame() const { return m_frame; }
		double PlayTime() const { return m_playTime; }
		size_t SpawnedCount() const { return m_spawned.size(); }
		size_t PendingCommandCount() const { return m_commands.size(); }
		size_t LastEventCount() const { return m_lastEventCount; }
		uint64_t FixedStepCount() const { return m_fixedSteps; }
		size_t DeclaredRecordCount() const { return m_data.size(); }
		Runtime& GetRuntime() { return *m_runtime; }
		const Runtime& GetRuntime() const { return *m_runtime; }

		// Reload support (LuaReload.cpp). TakeSnapshot is const and bounded (throws nothing; `error` set when
		// over the limit). ImportSnapshot runs on a freshly loaded candidate: it migrates, validates and copies
		// the state; on failure the candidate must be discarded. Handles are kept only when still valid.
		bool TakeSnapshot(StateSnapshot& out, std::string& error) const;
		bool ImportSnapshot(const StateSnapshot& snapshot, std::string& reason);
		void AddDiagnosticNote(Diagnostic::Severity severity, const std::string& source, const std::string& message);
		bool InCallback() const { return m_inCallback; }
		bool Loading() const { return m_phase == Phase::Loading; }
		bool InputEnabled() const { return m_inputEnabled; }
		ComponentManager* Manager() const;
		EntityHandle FindByName(const std::string& name) const;

		// Declared data (same rules as the Lua API; usable from host code and tests).
		bool SetDeclaredField(const EntityHandle& handle, const std::string& schema, const std::string& field,
			const Value& value, std::string& error);
		bool GetDeclaredField(const EntityHandle& handle, const std::string& schema, const std::string& field, Value& out) const;
		bool HasDeclared(const EntityHandle& handle, const std::string& schema) const;

		static constexpr size_t kMaxDiagnostics = 256;
		static constexpr size_t kMaxSpawnedPerPlay = 4096;
		static constexpr size_t kMaxEventsPerFrame = 10000;

	private:
		friend struct LuaApiBinder;   // C entry points of the nox API (LuaWorld.cpp)

		enum class Phase { Idle, Loading, Running };
		struct SystemEntry {
			std::string name;
			int order = 0;
			size_t sequence = 0;
			int ref = 0;
			bool fixed = false;
			bool faulted = false;
		};
		struct BehaviorEntry {
			std::string name;
			int ref = 0;
			bool faulted = false;
		};
		struct HandlerEntry {
			int ref = 0;
			bool faulted = false;
		};
		struct PendingEvent {
			std::string name;
			int payloadRef = 0;
		};

		// Used by the binding layer.
		void QueueCommand(Command command);
		int64_t NextSpawnToken() { return ++m_spawnTokenCounter; }
		bool ResolveSpawn(int64_t token, EntityHandle& out) const;
		bool QueueEvent(const std::string& name, int payloadRef);
		void SetDeclaredRecord(const EntityHandle& handle, const std::string& schema, const FieldMap& values);
		void RemoveDeclaredRecord(const EntityHandle& handle, const std::string& schema);
		const FieldMap* FindDeclaredRecord(const EntityHandle& handle, const std::string& schema) const;
		std::vector<EntityHandle> DataHandlesWithSchema(const std::string& schema) const;
		bool AttachBehavior(const EntityHandle& handle, const std::string& behavior, std::string& error);
		bool DetachBehavior(const EntityHandle& handle, const std::string& behavior);
		bool HasBehavior(const std::string& name) const;
		void AddSystem(const std::string& name, int order, int ref, bool fixed);
		void AddBehavior(const std::string& name, int ref);
		void AddHandler(const std::string& eventName, int ref);
		void AddDrawHandler(int ref);
		void AddConfigureHook(int ref) { m_configureRef = ref; }
		void AddSaveGameHook(int ref) { m_saveGameRef = ref; }
		void AddEditorPanel(const std::string& id, const std::string& title, int ref);
		void SetEditorViewport(int ref);
		bool RunEditorCallback(int ref, const std::string& label, bool* faultFlag, EditorHost* host, EditorHandles* handles, bool inViewport);
		bool AddSchema(Schema schema, std::string& error);
		bool AddMigration(const std::string& schema, int ref, std::string& error);
		// Migrates one stored record to the candidate's schema (runs the nox.migrate function when needed).
		bool MigrateRecord(const std::string& schema, int fromVersion, const FieldMap& in, FieldMap& out, std::string& error);
		const Schema* FindSchema(const std::string& name) const;
		void Log(const std::string& line);
		void AddDiagnostic(Diagnostic diagnostic);

		bool InstallApi(std::string& error);
		bool RunEntry(const ProjectContent& project, std::string& error);
		void ApplyAuthored(SceneGraph& scene, const nlohmann::json& entities);
		bool RunCallback(int ref, const std::string& label, const std::function<int(lua_State*)>& pushArgs,
			bool* faultFlag);
		void DispatchEvents(const std::vector<PendingEvent>& events);
		void ApplyCommands();
		void DeliverEvents();
		void RunBehaviors(float dt);
		void RunSystems(bool fixed, float dt);
		void SweepDead();
		void DiagnoseRunError(const std::string& source, const RunError& error, Diagnostic::Severity severity);
		void TouchTransform(const EntityHandle& handle);
		void RestoreTouched();
		void DestroySpawned();

		std::unique_ptr<ScriptSource> m_ownedSource;   // declared before the runtime: destroyed after it
		std::unique_ptr<Runtime> m_runtime;
		// Vehicle instances and their Lua controllers (ctx.vehicle); declared after the runtime so it is
		// destroyed first and releases its controller references while the VM is still open.
		std::unique_ptr<VehicleHost> m_vehicles;
		std::map<std::string, int> m_migrationRefs;
		SceneGraph* m_scene = nullptr;
		Phase m_phase = Phase::Idle;
		bool m_loaded = false;
		bool m_inCallback = false;
		bool m_inputEnabled = true;
		bool m_playing = false;
		bool m_teleportPending = false;
		uint64_t m_frame = 0;
		double m_playTime = 0.0;
		int m_contextRef = 0;
		int64_t m_spawnTokenCounter = 0;
		int m_substepIndex = 0;
		uint64_t m_fixedSteps = 0;
		std::map<int64_t, EntityHandle> m_resolvedSpawns;
		size_t m_lastEventCount = 0;

		std::map<std::string, Schema> m_schemas;
		std::vector<SystemEntry> m_systems;
		std::vector<BehaviorEntry> m_behaviors;
		std::map<std::string, std::vector<HandlerEntry>> m_handlers;
		std::vector<PendingEvent> m_events;
		size_t m_sequence = 0;

		// Declared data and behavior attachments keyed by checked identity (ordered by id).
		std::map<EntityHandle, SchemaMap> m_data;
		std::map<EntityHandle, std::set<std::string>> m_attachments;
		// Authored state captured at Load (Stop restores it).
		std::map<EntityHandle, SchemaMap> m_authoredData;
		std::map<EntityHandle, std::set<std::string>> m_authoredAttachments;

		std::vector<Command> m_commands;
		std::vector<EntityHandle> m_spawned;
		// Original local transform of every entity a script moved, captured before its first write.
		std::map<EntityHandle, glm::mat4> m_touched;

		std::vector<Diagnostic> m_diagnostics;

		// UI and input (LuaUi.cpp, LuaInput.cpp).
		struct DrawEntry {
			int ref = 0;
			size_t index = 0;   // registration order, used in diagnostics
			bool faulted = false;
			bool warnedCommands = false;
		};
		UiState m_ui;
		int m_uiRef = 0;
		DataState m_dataApi;               // read-only data, scene documents, authoring policy (LuaData.cpp)
		int m_configureRef = 0;            // nox.on_configure
		int m_saveGameRef = 0;             // nox.on_save_game
		bool m_saveGameFaulted = false;
		bool m_configureFaulted = false;
		LuaServices m_services;
		EditorBinding m_edBinding;
		std::vector<EditorPanelDecl> m_editorPanels;
		int m_editorViewportRef = 0;
		bool m_editorViewportFaulted = false;
		std::vector<DrawEntry> m_draws;
		bool m_drawDropReported = false;
		const InputMap* m_inputs = nullptr;
		bool m_wantsPointer = true;
	};
}
