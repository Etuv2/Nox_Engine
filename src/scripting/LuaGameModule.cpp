#include "../audio/AudioMixer.h"
#include "LuaGameModule.h"

#include "LuaEditorPanel.h"
#include "LuaEditorSupport.h"
#include "LuaScene.h"
#include "../Camera.h"
#include "../EditorExtensions.h"
#include "../PhysicsEngine.h"
#include "../SceneGraph.h"
#include "../assets/AssetIO.h"
#include "../app/ProjectBootstrap.h"
#include "../UiIconAtlas.h"
#include "../audio/ProceduralVehicleAudio.h"
#include "LuaEditorPanel.h"
#include "../VehicleFX.h"
#include "../ui/Pointer.h"
#include "../GuiNode.h"
#include "LuaUiFocus.h"

#include <IMGUI/imgui.h>

#include <algorithm>
#include <iostream>

namespace NoxLua {

	namespace {
		constexpr int kGuiPointerZ = 50;   // under the Lua widgets (100), above other engine regions

		// Every GUI node of the scene (the GUI pass collects the same set when it renders).
		void CollectGuiNodes(SceneNode& node, std::vector<GuiNode*>& out) {
			if (node.GetNodeType() == SceneNode::GUI) {
				if (GuiNode* gui = dynamic_cast<GuiNode*>(&node)) out.push_back(gui);
			}
			for (const std::shared_ptr<SceneNode>& child : node.children) {
				if (child) CollectGuiNodes(*child, out);
			}
		}

		const std::vector<Diagnostic>& NoDiagnostics() {
			static const std::vector<Diagnostic> empty;
			return empty;
		}

		bool SameStamps(const std::vector<ModuleStamp>& a, const std::vector<ModuleStamp>& b) {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i) {
				if (a[i].path != b[i].path || a[i].stamp != b[i].stamp) return false;
			}
			return true;
		}

		bool LooseWatchAllowed() {
			return AssetIO::IsInitialized() && AssetIO::GetMode() == AssetIO::Mode::Loose &&
				ProjectBootstrap::Info().reloadEnabled;
		}
	}

	LuaGameModule::LuaGameModule(ProjectDescriptor descriptor, std::unique_ptr<ScriptFiles> files)
		: m_descriptor(std::move(descriptor))
		, m_files(files ? std::move(files) : std::make_unique<AssetScriptFiles>())
		, m_watchEnabled(LooseWatchAllowed())
		, m_handles(std::make_shared<EditorHandles>())
		, m_link(std::make_shared<LuaEditorLink>()) {
	}

	LuaGameModule::~LuaGameModule() {
		// The callback captures this module: drop it before the module disappears.
		ClearSubsteps();
		// Undo closures that outlive the module see no world.
		if (m_link) m_link->world = nullptr;
	}

	void LuaGameModule::DrawEditorPanelById(const std::string& id, EditorHost& host) {
		if (!m_world) return;
		m_handles->SetUndoSink([&host](const std::string& label, std::function<void()> undo, std::function<void()> redo) {
			host.PushUndo(label, std::move(undo), std::move(redo));
		});
		m_world->DrawEditorPanel(id, &host, m_handles.get());
		m_handles->SetUndoSink(nullptr);
	}

	void LuaGameModule::DrawEditorViewport(EditorHost& host) {
		if (!m_world) return;
		m_handles->SetUndoSink([&host](const std::string& label, std::function<void()> undo, std::function<void()> redo) {
			host.PushUndo(label, std::move(undo), std::move(redo));
		});
		m_world->DrawEditorViewport(&host, m_handles.get());
		m_handles->SetUndoSink(nullptr);
	}

	void LuaGameModule::SetServices(const GameModuleServices& services) {
		ClearSubsteps();
		m_physics = services.physics;
	}

	void LuaGameModule::RegisterSubsteps() {
		if (!m_physics || m_substepsRegistered) return;
		// fixed = true systems run at each physics substep boundary (PhysicsEngine::SetSubstepCallback).
		m_physics->SetSubstepCallback([this](float fixedDt, int index) {
			if (m_world && m_available && !m_paused) m_world->FixedUpdate(fixedDt, index);
		});
		m_substepsRegistered = true;
	}

	void LuaGameModule::ClearSubsteps() {
		if (m_physics && m_substepsRegistered) m_physics->SetSubstepCallback(nullptr);
		m_substepsRegistered = false;
	}

	void LuaGameModule::InitializeResources() {
		if (m_resourcesReady) return;
		m_resourcesReady = true;
		std::vector<std::string> messages;
		LoadUiFonts(m_descriptor.fonts, m_fonts, messages);
		m_audio.Init();   // no device: audio calls become no-ops (the log says why)
		if (!m_descriptor.iconManifest.empty() &&
			!UiIconAtlas::Get().Load(m_descriptor.iconManifest, m_descriptor.iconSize)) {
			messages.push_back("icon atlas '" + m_descriptor.iconManifest + "' could not be loaded; ui.image draws nothing");
		}
		for (const std::string& m : messages) m_resourceMessages.push_back(m);
		if (m_world) AttachResources(m_world.get());
	}

	nlohmann::json LuaGameModule::SaveGameJson() const {
		if (!m_world || !m_world->HasSaveGameHook()) return m_game;
		nlohmann::json value;
		std::string error;
		if (m_world->SaveGameJson(value, error)) return value;
		// The failure is a diagnostic of the world (shown in the editor); the scene's object is kept.
		return m_game;
	}

	void LuaGameModule::AttachResources(LuaWorld* world) {
		m_link->world = world;
		if (!world) return;
		world->SetUiFonts(&m_fonts);
		world->SetTextures(&m_textures);
		world->SetInputs(&m_inputs);
		world->SetCamera(m_camera.get());
		RegisterLuaProjectPanels(*world, m_descriptor.descriptorPath);
		for (const std::string& message : m_resourceMessages) {
			world->AddDiagnosticNote(Diagnostic::Severity::Warning, "resources", message);
		}
	}

	// Gameplay thread, once per frame: the state scripts published (ctx:audio_set) and queued cues (ctx:audio_cue).
	// The SDL callback reads the copy made here; it never calls Lua.
	void LuaGameModule::PublishAudio() {
		if (!m_world) return;
		LuaServices& services = m_world->Services();
		if (services.audioUsed) {
			VehicleAudioState state = services.audio;
			state.paused = state.paused || m_paused;
			m_audio.Update(state);
		}
		for (AudioCue cue : services.audioCues) m_audio.Play(cue);
		services.audioCues.clear();
	}

	void LuaGameModule::DrawOverlay(ImDrawList* drawList, const glm::vec2& origin, const glm::vec2& size, float dt) {
		if (!m_world || !m_available) return;
		const glm::vec2 pointer = ui::Pointer::Get().Position() - origin;
		m_world->SetOverlayRect(origin, size);
		m_world->DrawFrame(dt, size, pointer, m_recorder);
		PlaybackUi(m_recorder, &m_fonts, drawList, origin, &m_textures);
		InteractGui(drawList);
	}

	void LuaGameModule::InteractGui(ImDrawList* drawList) {
		if (!m_scene || !m_world) return;
		std::vector<GuiNode*> nodes;
		if (std::shared_ptr<SceneNode> root = m_scene->GetRoot()) CollectGuiNodes(*root, nodes);
		ui::Pointer& pointer = ui::Pointer::Get();
		const UiNavigator& nav = m_world->Navigator();
		std::map<uint32_t, std::string> hoveredNow;
		for (size_t n = 0; n < nodes.size(); ++n) {
			const std::vector<GuiNode::GuiElement>& elements = nodes[n]->GetElements();
			for (size_t i = 0; i < elements.size(); ++i) {
				const GuiNode::GuiElement& e = elements[i];
				if (!e.interactive || !e.visible) continue;
				// Elements without an id are named by node and index: stable while the scene is not edited.
				const std::string name = e.id.empty() ? "gui" + std::to_string(n) + "." + std::to_string(i) : e.id;
				const uint32_t id = ui::HashId(name.c_str());
				const glm::vec2 min(e.x, e.y), max(e.x + e.width, e.y + e.height);
				pointer.AddRect(id, min, max, kGuiPointerZ, ui::RegionAll);
				m_world->AddNavigationWidget(id, min, max);
				if (pointer.IsHovered(id)) hoveredNow[id] = name;
				if (pointer.IsClicked(id) || nav.Accept(id)) m_world->QueueUiEvent("ui.click", name, e.onClick, -1);
				if (drawList && nav.IsFocused(id)) {
					drawList->AddRect(ImVec2(min.x - 3.0f, min.y - 3.0f), ImVec2(max.x + 3.0f, max.y + 3.0f),
						IM_COL32(79, 195, 247, 255), 0.0f, 0, 2.0f);
				}
			}
		}
		for (const auto& [id, name] : hoveredNow) {
			if (!m_guiHover.count(id)) m_world->QueueUiEvent("ui.hover", name, std::string(), 1);
		}
		for (const auto& [id, name] : m_guiHover) {
			if (!hoveredNow.count(id)) m_world->QueueUiEvent("ui.hover", name, std::string(), 0);
		}
		m_guiHover.swap(hoveredNow);
	}

	void LuaGameModule::SetCameraTemplate(const Camera& camera) {
		// Same policy as the native game camera: copy the engine view's position and projection.
		m_camera = std::make_shared<Camera>(camera.GetCameraPosition(), glm::vec3(0.0f, 1.0f, 0.0f), 90.0f, 0.0f,
			camera.GetCameraFov(), std::max(camera.GetCameraNearPlane(), 0.05f), std::max(camera.GetCameraFarPlane(), 600.0f),
			10.0f, 0.1f);
		m_camera->SetProjectionType(Camera::ProjectionType::Perspective);
		if (m_world) m_world->SetCamera(m_camera.get());
	}

	void LuaGameModule::Configure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& scene) {
		m_game = game;
		m_scene = &scene;
		m_paused = false;
		m_available = false;
		m_reloadRequested = false;
		m_observed.clear();
		m_pollElapsed = 0.0;
		m_stableElapsed = 0.0;
		const nlohmann::json authored = sceneRoot.is_object() ? sceneRoot.value("scripting", nlohmann::json::object()) : nlohmann::json::object();
		m_authoredEntities = authored.is_object() ? authored.value("entities", nlohmann::json::object()) : nlohmann::json::object();

		// The action map is plain data: parse it once per module (no GPU or ImGui needed).
		// Project audio buses join the mixer (names that are invalid or duplicates are skipped).
		for (const std::string& bus : m_descriptor.audioBuses) {
			if (!AudioMixer::Get().HasBus(bus) && !AudioMixer::Get().AddBus(bus)) {
				m_resourceMessages.push_back("audio bus '" + bus + "' is invalid");
			}
		}

		if (!m_inputsLoaded) {
			m_inputsLoaded = true;
			std::vector<std::string> inputErrors;
			m_inputs.LoadFile(m_descriptor.inputPath, inputErrors);
			for (const std::string& e : inputErrors) m_resourceMessages.push_back("input map: " + e);
		}

		// A fresh configuration (scene load or Stop): the previous world is released here.
		ClearSubsteps();
		m_docs.game = game;
		m_docs.root = sceneRoot;
		// A scene loaded by ctx:load_scene(path, payload) takes its payload here; any other configuration takes none.
		std::optional<nlohmann::json> payload = TakePendingScenePayload();
		StageOutcome outcome = StageWorld(m_limits, *m_files, m_descriptor.content, scene, m_authoredEntities, nullptr, &m_docs);
		m_world = std::move(outcome.world);
		if (m_world) m_world->SetScenePayload(std::move(payload));
		m_available = outcome.ok;
		m_stamps = outcome.ok ? outcome.stamps : std::vector<ModuleStamp>();
		AttachResources(m_world.get());
	}

	void LuaGameModule::Reconfigure(const nlohmann::json& game, const nlohmann::json& sceneRoot, SceneGraph& scene) {
		if (!m_world || !m_available || m_scene != &scene || !m_world->HasConfigureHook()) {
			Configure(game, sceneRoot, scene);
			return;
		}
		m_game = game;
		m_docs.game = game;
		m_docs.root = sceneRoot;
		m_world->SetSceneDocuments(game, sceneRoot);
		std::string error;
		if (!m_world->RunConfigureHook(error)) {
			// The hook reported its own diagnostics; fall back to a clean world with the new data.
			Configure(game, sceneRoot, scene);
		}
	}

	bool LuaGameModule::RequestReload(std::string* error) {
		if (!m_world || !m_scene) {
			if (error) *error = "no Lua project world is loaded";
			return false;
		}
		m_reloadRequested = true;
		return true;
	}

	void LuaGameModule::BeginPlay() {
		if (!m_world || !m_available) return;
		m_paused = false;
		RegisterSubsteps();
		m_world->BeginPlay();
	}

	void LuaGameModule::EndPlay() {
		ClearSubsteps();
		if (!m_world) return;
		m_world->EndPlay();
		m_paused = false;
	}

	void LuaGameModule::RecordRejectedReload(const std::string& reason) {
		++m_reloadRejected;
		Diagnostic d;
		d.severity = Diagnostic::Severity::Error;
		d.source = "reload";
		d.message = "reload rejected, previous code and state kept: " + reason;
		m_reloadLog.push_back(std::move(d));
		if (m_reloadLog.size() > LuaWorld::kMaxDiagnostics) m_reloadLog.erase(m_reloadLog.begin());
	}

	// Frame boundary: snapshot the running world, stage a candidate, swap on success.
	void LuaGameModule::ApplyReload() {
		m_reloadRequested = false;
		if (!m_scene) {
			RecordRejectedReload("no scene is configured");
			return;
		}
		StateSnapshot snapshot;
		std::string error;
		if (m_world && !m_world->TakeSnapshot(snapshot, error)) {
			RecordRejectedReload(error);
			return;
		}
		StageOutcome outcome = StageWorld(m_limits, *m_files, m_descriptor.content, *m_scene, m_authoredEntities,
			m_world ? &snapshot : nullptr, &m_docs);
		if (!outcome.ok) {
			RecordRejectedReload(outcome.reason);
			return;   // the candidate is destroyed with `outcome`; nothing of the old world was touched
		}

		// Swap. The old world releases its registry references in its destructor, then its VM closes.
		// No initializer or begin_play runs: the candidate continues from the imported declared state.
		const bool wantsPointer = m_world ? m_world->WantsPointer() : true;
		std::optional<nlohmann::json> payload;   // the scene payload belongs to the scene, not to its code: it carries over
		if (m_world && m_world->ScenePayloadPtr()) payload = *m_world->ScenePayloadPtr();
		std::unique_ptr<LuaWorld> previous = std::move(m_world);
		m_world = std::move(outcome.world);
		m_world->SetWantsPointer(wantsPointer);
		m_world->SetScenePayload(std::move(payload));
		// (the candidate's nox.on_configure already ran during staging)
		AttachResources(m_world.get());
		// Running vehicles move into the candidate (controllers rebound to its VM, declared state restored) before the
		// old VM is released. A failed staging never reaches this point, so the running cars are untouched then.
		if (previous) m_world->Vehicles().AdoptFrom(previous->Vehicles());
		previous.reset();
		m_stamps = outcome.stamps;
		m_available = true;
		m_observed.clear();
		m_stableElapsed = 0.0;
		++m_reloadCount;
		Diagnostic d;
		d.severity = Diagnostic::Severity::Warning;
		d.source = "reload";
		d.message = "reload applied (" + std::to_string(outcome.attempts) + " staging attempt(s)); faults were cleared";
		m_reloadLog.push_back(std::move(d));
		if (m_reloadLog.size() > LuaWorld::kMaxDiagnostics) m_reloadLog.erase(m_reloadLog.begin());
	}

	// Loose-file watcher: a module change that stays the same for the debounce period requests a reload.
	void LuaGameModule::PollWatcher(float dt) {
		if (!m_watchEnabled || m_stamps.empty()) return;
		m_pollElapsed += dt;
		if (m_pollElapsed < kWatchPollSeconds) return;
		m_pollElapsed = 0.0;

		std::vector<ModuleStamp> now;
		bool changed = false;
		for (const ModuleStamp& loaded : m_stamps) {
			FileStamp current;
			const bool present = m_files->Stat(loaded.path, current);
			now.push_back(ModuleStamp{ loaded.path, present ? current : FileStamp{} });
			if (!present || current != loaded.stamp) changed = true;
		}
		if (!changed) {
			m_observed.clear();
			m_stableElapsed = 0.0;
			return;
		}
		if (!SameStamps(now, m_observed)) {
			m_observed = std::move(now);
			m_stableElapsed = 0.0;
			return;
		}
		m_stableElapsed += kWatchPollSeconds;
		if (m_stableElapsed + 1e-9 >= kWatchDebounceSeconds) {
			m_observed.clear();
			m_stableElapsed = 0.0;
			RequestReload();
		}
	}

	IGameModule::UpdateResult LuaGameModule::Update(float dt, bool inputEnabled, float aspect) {
		PollWatcher(dt);
		if (m_reloadRequested) ApplyReload();
		// Input is sampled once per frame (latched for every system and fixed substep of this frame).
		const bool inputLive = inputEnabled && ui::Pointer::Get().HasFocus();
		m_inputs.Sample(inputLive);
		// Menu navigation and UI consumption are decided before gameplay runs, so a paused menu still answers the keys.
		if (m_world && m_available) m_world->BeginUiInput(SdlInputDevices(inputLive), inputLive);
		if (!m_world || !m_available) return UpdateResult::Continue;
		m_world->SetAspect(aspect);
		if (!m_paused) {
			m_world->Update(dt, inputEnabled);
			// Native particle pools tick once per frame, and only when the project has emitted (ctx:fx_*).
			if (m_world->Services().fxUsed) VehicleFX::Update(dt);
		}
		PublishAudio();   // also while paused: the synth fades out
		// ctx:request_exit() asks the host to close the application (Core decides what that means in the editor).
		if (m_world->ExitRequested()) return UpdateResult::ExitApplication;
		return UpdateResult::Continue;
	}

	bool LuaGameModule::ConsumeTeleport() {
		return m_world ? m_world->ConsumeTeleport() : false;
	}

	const std::vector<Diagnostic>& LuaGameModule::Diagnostics() const {
		return m_world ? m_world->Diagnostics() : NoDiagnostics();
	}

	std::unique_ptr<IGameModule> CreateLuaProjectModule(const std::string& descriptorPath,
		const nlohmann::json& descriptor, std::string& error) {
		ProjectDescriptor project;
		if (!LoadProjectDescriptor(descriptorPath, descriptor, project, error)) return nullptr;
		RegisterLuaEditorPanel();
		// Editor support for external IDEs (annotations and VS Code settings): written when missing or stale, loose tree only.
		if (AssetIO::IsInitialized() && AssetIO::GetMode() == AssetIO::Mode::Loose) {
			std::string report;
			EnsureLuaEditorSupport(project.descriptorPath, report);
			std::cout << report;
		}
		return std::make_unique<LuaGameModule>(std::move(project));
	}
}
