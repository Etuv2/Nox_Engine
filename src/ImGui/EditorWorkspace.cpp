#include "EditorWorkspace.h"

#include "ConsoleLog.h"
#include "../Core.h"
#include "../Camera.h"
#include "../assets/AssetIO.h"
#include "../assets/AssetIOJson.h"
#include "../assets/AssetIOUser.h"
#include "../app/UserSettings.h"
#include "../GameModule.h"
#include "../ImGuiInterface.h"
#include "../LightNode.h"
#include "../BaseLight.h"
#include "../ModularRenderer.h"
#include "../GizmoRayCast.h"
#include "../UiIconAtlas.h"
#include "../Scene.h"
#include "../SceneGraph.h"
#include "../PhysicsEngine.h"
#include "../SceneNode.h"
#include "../editor/ProcessRunner.h"
#include "../editor/ExternalEditor.h"
#include "../editor/solace/SolaceUI.h"
#include "../scripting/LuaEditorSupport.h"
#include "../editor/InspectorUndo.h"
#include "../scene/Prefab.h"
#include "../app/ProjectBootstrap.h"
#include "InspectorWidgets.h"
#include "ContentBrowser.h"

#include "rhi/RHI.h"
#include <SDL/SDL.h>
#include <IMGUI/imgui.h>
#include <IMGUI/imgui_internal.h>
#include <IMGUI/ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <cstdlib>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
	const char* kSettingsPath = "settings/editor_settings.json"; // per-user (AssetIO::UserDir)
	const char* kPresetsPath = "config/launch_presets.json";
	// Dock layout version: bump to rebuild the default arrangement (2: Content Browser docked along the bottom;
	// 3: the bottom drawer is taller so the Content Browser's details strip fits).
	const int kLayoutVersion = 3;

	// Opens a logical asset in the external editor (line 0 = top); failures are toasted.
	void OpenAssetExternally(const std::string& logicalPath, int line) {
		std::string error;
		if (!ExternalEditor::OpenLogicalFile(logicalPath, line, error)) ui::Toast(error, ui::ToastKind::Warning);
	}

	// Panel window names (stable IDs for the dock layout).
	const char* kWinScene = "Scene";
	const char* kWinGame = "Game";
	const char* kWinHierarchy = "Hierarchy";
	const char* kWinAssets = "Assets";
	const char* kWinInspector = "Inspector";
	const char* kWinMaterials = "Materials";
	const char* kWinConsole = "Console";
	const char* kWinBuild = "Build Output";
	const char* kWinPresets = "Launch Presets";
	const char* kWinPreferences = "Preferences";
	// Legacy panels (BaseWindow names).
	const char* kWinRendering = "Rendering Settings";
	const char* kWinLighting = "Lighting System";
	const char* kWinPerformance = "Performance Monitor";
	const char* kWinAnimation = "Animation Preview";
	const char* kWinState = "State & Export";
	const char* kWinCamera = "Camera Controls";
	const char* kWinPhysics = "Physics";

	// Camera pose from a position and a view direction (Camera has no yaw/pitch getters).
	void PoseCamera(Camera& cam, const glm::vec3& eye, const glm::vec3& dir) {
		const glm::vec3 d = glm::normalize(dir);
		cam.SetPose(eye, glm::vec3(0, 1, 0), glm::degrees(std::atan2(d.z, d.x)), glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))));
	}

	std::array<float, 10> CaptureTRS(const std::shared_ptr<SceneNode>& n) {
		const glm::vec3 p = n->GetPosition();
		const glm::quat q = n->GetOrientation();
		const glm::vec3 s = n->GetScale();
		return { p.x, p.y, p.z, q.w, q.x, q.y, q.z, s.x, s.y, s.z };
	}
	void ApplyTRS(const std::shared_ptr<SceneNode>& n, const std::array<float, 10>& t) {
		n->SetLocalTRS(glm::vec3(t[0], t[1], t[2]), glm::quat(t[3], t[4], t[5], t[6]), glm::vec3(t[7], t[8], t[9]));
	}

	bool ContainsNoCase(const std::string& text, const char* filter) {
		if (!filter || !filter[0]) return true;
		std::string a = text, b = filter;
		std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a.find(b) != std::string::npos;
	}

	std::string FormatTime(float t) {
		if (t < 0.0f) return "--:--.--";
		char buf[32];
		const int m = static_cast<int>(t / 60.0f);
		std::snprintf(buf, sizeof(buf), "%d:%05.2f", m, t - m * 60.0f);
		return buf;
	}

	std::shared_ptr<SceneNode> FindSharedNode(const std::shared_ptr<SceneNode>& root, const SceneNode* target) {
		if (!root) return nullptr;
		if (root.get() == target) return root;
		for (const auto& c : root->children) {
			if (auto found = FindSharedNode(c, target)) return found;
		}
		return nullptr;
	}

	// Scene-file JSON for the Add Node menu: the same shape the loader reads, so the node is identical to
	// one loaded from a scene.
	json NodeTemplate(const std::string& kind) {
		if (kind == "directional") return { {"type", "light"}, {"name", "Directional Light"},
			{"light", { {"type", "directional"}, {"color", { 1.0f, 1.0f, 1.0f }}, {"intensity", 3.0f} }} };
		if (kind == "point") return { {"type", "light"}, {"name", "Point Light"},
			{"light", { {"type", "point"}, {"color", { 1.0f, 1.0f, 1.0f }}, {"intensity", 10.0f}, {"range", 10.0f} }} };
		if (kind == "spot") return { {"type", "light"}, {"name", "Spot Light"},
			{"light", { {"type", "spot"}, {"color", { 1.0f, 1.0f, 1.0f }}, {"intensity", 10.0f}, {"range", 15.0f},
				{"cutOff", 20.0f}, {"outerCutOff", 30.0f} }} };
		if (kind == "audio") return { {"type", "audio"}, {"name", "Audio Source"}, {"volume", 1.0f}, {"loop", false}, {"is3d", true} };
		if (kind == "camera") return { {"type", "camera"}, {"name", "Camera"} };
		if (kind == "gui") return { {"type", "gui"}, {"name", "GUI"} };
		if (kind == "lpv_volume") return { {"type", "lpv_volume"}, {"name", "LPV Volume"} };
		return { {"type", "node"}, {"name", "Empty"} };
	}

	// Spawn point in front of the editor camera, in the parent's local space (world space at the root).
	glm::vec3 SpawnPosition(Core& core, const std::shared_ptr<SceneNode>& parent, float height) {
		glm::vec3 world(0.0f, height, 0.0f);
		if (auto cam = core.GetCamera()) {
			world = cam->GetCameraPosition() + cam->GetCameraFrontVector() * 8.0f;
			world.y = height;
		}
		if (!parent) return world;
		return glm::vec3(glm::inverse(parent->GetWorldPosition4x4()) * glm::vec4(world, 1.0f));
	}

	// Placement of a new asset: the given world point (converted to the parent's space) or, without one, in front of the camera.
	glm::vec3 PlacementPosition(Core& core, const std::shared_ptr<SceneNode>& parent, const glm::vec3* at)
	{
		if (!at) return SpawnPosition(core, parent, 0.0f);
		if (!parent) return *at;
		return glm::vec3(glm::inverse(parent->GetWorldPosition4x4()) * glm::vec4(*at, 1.0f));
	}

	// Child name not used by a sibling: base, then "base 2", "base 3", ... Keeps "Folder/Child" path lookups unambiguous.
	std::string UniqueChildName(const std::shared_ptr<SceneNode>& parent, const std::string& base) {
		std::string name = base;
		for (int n = 2; ; ++n) {
			bool taken = false;
			for (const auto& c : parent->children) taken = taken || (c && c->GetName() == name);
			if (!taken) return name;
			name = base + " " + std::to_string(n);
		}
	}

	std::shared_ptr<SceneNode> FindParent(const std::shared_ptr<SceneNode>& root, const SceneNode* child) {
		if (!root) return nullptr;
		for (const auto& c : root->children) {
			if (c.get() == child) return root;
			if (auto p = FindParent(c, child)) return p;
		}
		return nullptr;
	}
}

EditorWorkspace::EditorWorkspace(Core& core, ImGuiInterface& legacyUi)
	: m_core(core), m_legacy(legacyUi), m_runner(std::make_unique<ProcessRunner>())
{
	const json s = UserSettings::LoadUserJson(kSettingsPath).value_or(json());
	if (s.is_object()) {
		const json p = s.value("panels", json::object());
		auto rd = [&](const char* key, bool& v) { v = p.value(key, v); };
		rd("hierarchy", m_showHierarchy); rd("assets", m_showAssets); rd("inspector", m_showInspector);
		rd("materials", m_showMaterials); rd("console", m_showConsole); rd("build", m_showBuild);
		rd("rendering", m_showLegacyRendering);
		rd("lighting", m_showLegacyLighting); rd("performance", m_showLegacyPerformance); rd("animation", m_showLegacyAnimation);
		rd("state", m_showLegacyState); rd("camera", m_showLegacyCamera); rd("presets", m_showPresets);
		rd("physics", m_showPhysics);
		m_showAuthoring = s.value("authoring_overlay", m_showAuthoring);
		m_extSaved = s.value("extension_panels", json::object());
		if (!m_extSaved.is_object()) m_extSaved = json::object();
		m_layoutBuilt = s.value("layout_built", false) && s.value("layout_version", 0) >= kLayoutVersion;
		m_selectedPreset = s.value("preset", m_selectedPreset);
		const json o = s.value("selection_outline", json::object());
		if (o.is_object()) {
			const json c = o.value("color", json::array());
			if (c.is_array() && c.size() == 3) for (int i = 0; i < 3; ++i) m_outline.color[i] = c[i].get<float>();
			m_outline.width = std::clamp(o.value("width", m_outline.width), 1, 4);
			m_outline.showOccluded = o.value("show_occluded", m_outline.showOccluded);
			m_outline.hover = o.value("hover", m_outline.hover);
		}
		ExternalEditor::Current() = ExternalEditor::FromJson(s.value("external_editor", json()));
		m_hierarchySaved = s.value("hierarchy", json::object());
		if (!m_hierarchySaved.is_object()) m_hierarchySaved = json::object();
		m_hierarchySort = nox::editor::ParseHierarchySort(m_hierarchySaved.value("sort", std::string("scene")));
	}
	m_presets = AssetIO::ReadJsonOrNull(kPresetsPath);
	if (!m_presets.is_object()) m_presets = json::object();
	m_contentBrowser = std::make_unique<nox::content::ContentBrowser>(nox::content::ContentBrowser::Host{
		[this] { return m_core.IsPlaying(); },
		[this](const std::string& scene) { RequestScene(scene); },
		[this](const std::string& asset) { PlaceAssetInScene(asset, m_legacy.GetSelectedNode(), nullptr); },
		[this](const std::string& label, std::function<void()> undo, std::function<void()> redo) {
			if (!m_core.IsPlaying()) Push(label, std::move(undo), std::move(redo));
		},
		[this] { return m_core.GetCurrentScenePath(); },
	});
	// The layout ini may have been deleted: rebuild when no dock node exists yet.
	const std::string userLayout = AssetIO::UserOutputPath("editor_layout.ini");
	const bool haveLayout = (!userLayout.empty() && fs::exists(userLayout)) || AssetIO::Exists("config/editor_layout.ini");
	if (!haveLayout) m_layoutBuilt = false;
}

EditorWorkspace::~EditorWorkspace()
{
	SaveSettings();
	for (Viewport* vp : { &m_scene, &m_game }) {
		if (vp->fbo) rhi::Gpu().DestroyFramebuffer(vp->fbo);
		if (vp->color) rhi::Gpu().DestroyTexture(vp->color);
		if (vp->depth) rhi::Gpu().DestroyTexture(vp->depth);
	}
}

json EditorWorkspace::HierarchySettingsJson() const
{
	json h = m_hierarchySaved;
	h["sort"] = nox::editor::HierarchySortName(m_hierarchySort);
	if (!h.contains("scenes") || !h["scenes"].is_object()) h["scenes"] = json::object();
	if (!m_hierarchyScenePath.empty()) h["scenes"][m_hierarchyScenePath] = m_hierarchyView.Save();
	return h;
}

void EditorWorkspace::SaveSettings() const
{
	json s;
	s["panels"] = {
		{"hierarchy", m_showHierarchy}, {"assets", m_showAssets}, {"inspector", m_showInspector}, {"materials", m_showMaterials},
		{"console", m_showConsole}, {"build", m_showBuild},
		{"rendering", m_showLegacyRendering}, {"lighting", m_showLegacyLighting}, {"performance", m_showLegacyPerformance},
		{"animation", m_showLegacyAnimation}, {"state", m_showLegacyState}, {"camera", m_showLegacyCamera}, {"presets", m_showPresets},
		{"physics", m_showPhysics} };
	json ext = json::object();
	for (const auto& [id, open] : m_extOpen) ext[id] = open;
	s["extension_panels"] = ext;
	s["authoring_overlay"] = m_showAuthoring;
	s["layout_built"] = m_layoutBuilt;
	s["layout_version"] = kLayoutVersion;
	s["preset"] = m_selectedPreset;
	s["selection_outline"] = {
		{"color", json::array({ m_outline.color[0], m_outline.color[1], m_outline.color[2] })},
		{"width", m_outline.width}, {"show_occluded", m_outline.showOccluded}, {"hover", m_outline.hover} };
	s["external_editor"] = ExternalEditor::ToJson(ExternalEditor::Current());
	s["hierarchy"] = HierarchySettingsJson();
	UserSettings::SaveUserJson(kSettingsPath, s);
}

void EditorWorkspace::OnSceneLoaded()
{
	// Collapse state is per scene: keep the state of the scene being left, then load the one being opened.
	if (!m_hierarchyScenePath.empty()) m_hierarchySaved["scenes"][m_hierarchyScenePath] = m_hierarchyView.Save();
	m_hierarchyScenePath = m_core.GetCurrentScenePath();
	const json scenes = m_hierarchySaved.value("scenes", json::object());
	m_hierarchyView.Load(scenes.value(m_hierarchyScenePath, json()));
	m_hierarchyRevealed.reset();
	m_undo.clear();
	m_redo.clear();
	m_handles.clear();
	m_dragHandle = -1;
	m_gameEditActive = false;
	m_propEdit = nox::editor::PropertyEdit{};
	m_nodeTable.Clear();
	m_renameTarget.reset();
	m_scene.rendered = m_game.rendered = false;
}

void EditorWorkspace::FocusGame()
{
	m_focusGameNextFrame = true;
}

bool EditorWorkspace::GameInputFocus() const
{
	const ImGuiIO& io = ImGui::GetIO();
	return m_game.focused && !io.WantTextInput;
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::Draw(float dt)
{
	m_dt = dt;
	m_fpsSmoothed = m_fpsSmoothed <= 0.0f ? (dt > 0 ? 1.0f / dt : 0.0f) : m_fpsSmoothed + ((dt > 0 ? 1.0f / dt : 0.0f) - m_fpsSmoothed) * 0.05f;
	m_statusTimer = std::max(0.0f, m_statusTimer - dt);
	SyncSelection();

	ImGuiViewport* mainVp = ImGui::GetMainViewport();
	DrawMenuBar();
	DrawToolbar();

	// Dockspace below the menu + toolbar.
	const float toolbarH = ImGui::GetFrameHeight() * 1.9f;
	ImGui::SetNextWindowPos(ImVec2(mainVp->WorkPos.x, mainVp->WorkPos.y + toolbarH));
	ImGui::SetNextWindowSize(ImVec2(mainVp->WorkSize.x, mainVp->WorkSize.y - toolbarH - ImGui::GetFrameHeight()));
	ImGui::SetNextWindowViewport(mainVp->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoBackground;
	ImGui::Begin("##DockHost", nullptr, hostFlags);
	ImGui::PopStyleVar(3);
	const ImGuiID dockId = ImGui::GetID("NoxDockspace");
	if (!m_layoutBuilt || m_resetLayout || ImGui::DockBuilderGetNode(dockId) == nullptr) {
		BuildDefaultLayout(dockId);
		m_layoutBuilt = true;
		m_resetLayout = false;
		SaveSettings();
	}
	ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::End();

	// Status bar.
	{
		ImGui::SetNextWindowPos(ImVec2(mainVp->WorkPos.x, mainVp->WorkPos.y + mainVp->WorkSize.y - ImGui::GetFrameHeight()));
		ImGui::SetNextWindowSize(ImVec2(mainVp->WorkSize.x, ImGui::GetFrameHeight()));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 2));
		ImGui::Begin("##StatusBar", nullptr, hostFlags & ~ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
		ImGui::PopStyleVar();
		const std::string scene = m_core.GetCurrentScenePath();
		ImGui::TextDisabled("%s%s", scene.c_str(), m_core.IsDirty() ? "  (modified)" : "");
		ImGui::SameLine(ImGui::GetWindowWidth() * 0.45f);
		if (m_statusTimer > 0.0f) ImGui::TextUnformatted(m_status.c_str());
		else ImGui::TextDisabled("%s", m_core.IsPlaying() ? (m_core.IsPaused() ? "Paused" : "Playing - click the Game view to control the game") : "Editing");
		char fps[64];
		std::snprintf(fps, sizeof(fps), "%.0f fps  %.2f ms", m_fpsSmoothed, m_fpsSmoothed > 0 ? 1000.0f / m_fpsSmoothed : 0.0f);
		ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(fps).x - 12.0f);
		ImGui::TextDisabled("%s", fps);
		ImGui::End();
	}

	// Legacy panels keep their engine connections; visibility follows the Window menu.
	m_legacy.SetWindowVisible("Rendering", m_showLegacyRendering);
	m_legacy.SetWindowVisible("Lighting", m_showLegacyLighting);
	m_legacy.SetWindowVisible("Performance", m_showLegacyPerformance);
	m_legacy.SetWindowVisible("Animation", m_showLegacyAnimation);
	m_legacy.SetWindowVisible("StateExport", m_showLegacyState);
	m_legacy.SetWindowVisible("Camera", m_showLegacyCamera);
	m_legacy.SetWindowVisible("Status", false);
	m_legacy.SetWindowVisible("Hierarchy", false);
	m_legacy.SetWindowVisible("Help", m_showHelp);

	TrackGameEdits();

	DrawGameViewport();
	DrawSceneViewport();
	if (m_showHierarchy) DrawHierarchy();
	if (m_showAssets) DrawAssets();
	if (m_showInspector) DrawInspector();
	if (m_showPhysics) DrawPhysics();
	if (m_showMaterials) DrawMaterials();
	if (m_showConsole) DrawConsole();
	if (m_showBuild) DrawBuildOutput();
	// Application panels (EditorExtensions): each is a dockable window with its own open state.
	for (const EditorPanel& panel : EditorExtensions::Panels()) {
		bool& open = PanelOpen(panel);
		if (!open || (panel.available && !panel.available(*this))) continue;
		if (ImGui::Begin(panel.title.c_str(), &open) && panel.draw) panel.draw(*this);
		ImGui::End();
	}
	if (m_showPresets) DrawLaunchPresets();
	if (m_showPreferences) DrawPreferences();
	if (m_showDemo) ImGui::ShowDemoWindow(&m_showDemo);
	DrawModals();
	HandleShortcuts();
	ChooseRenderTarget();
	RunSmokeScript();

	// Legacy panel visibility may have been closed with their own close buttons.
	m_showLegacyRendering = m_legacy.IsWindowVisible("Rendering");
	m_showLegacyLighting = m_legacy.IsWindowVisible("Lighting");
	m_showLegacyPerformance = m_legacy.IsWindowVisible("Performance");
	m_showLegacyAnimation = m_legacy.IsWindowVisible("Animation");
	m_showLegacyState = m_legacy.IsWindowVisible("StateExport");
	m_showLegacyCamera = m_legacy.IsWindowVisible("Camera");
	m_showHelp = m_legacy.IsWindowVisible("Help");
	solace::DrawToasts(ImRect(mainVp->WorkPos, ImVec2(mainVp->WorkPos.x + mainVp->WorkSize.x, mainVp->WorkPos.y + mainVp->WorkSize.y - 30.0f)));
}

void EditorWorkspace::BuildDefaultLayout(unsigned int dockspaceId)
{
	ImGui::DockBuilderRemoveNode(dockspaceId);
	ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
	ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::DockBuilderSetNodeSize(dockspaceId, vp->WorkSize);
	ImGuiID center = dockspaceId;
	ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
	ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
	ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.36f, nullptr, &center);
	// The Content Browser ("Assets") is a drawer along the bottom-left; Console, Build and the tools share the rest.
	ImGuiID assets = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.62f, nullptr, &bottom);
	ImGui::DockBuilderDockWindow(kWinScene, center);
	ImGui::DockBuilderDockWindow(kWinGame, center);
	ImGui::DockBuilderDockWindow(kWinHierarchy, left);
	ImGui::DockBuilderDockWindow(kWinAssets, assets);
	ImGui::DockBuilderDockWindow(kWinInspector, right);
	ImGui::DockBuilderDockWindow(kWinMaterials, right);
	ImGui::DockBuilderDockWindow(kWinLighting, right);
	ImGui::DockBuilderDockWindow(kWinRendering, right);
	ImGui::DockBuilderDockWindow(kWinCamera, right);
	ImGui::DockBuilderDockWindow(kWinPhysics, right);
	ImGui::DockBuilderDockWindow(kWinConsole, bottom);
	ImGui::DockBuilderDockWindow(kWinBuild, bottom);
	ImGui::DockBuilderDockWindow(kWinPerformance, bottom);
	for (const EditorPanel& panel : EditorExtensions::Panels()) ImGui::DockBuilderDockWindow(panel.title.c_str(), bottom);
	ImGui::DockBuilderDockWindow(kWinAnimation, bottom);
	ImGui::DockBuilderDockWindow(kWinState, bottom);
	ImGui::DockBuilderDockWindow(kWinPresets, bottom);
	ImGui::DockBuilderDockWindow("Help & Controls", right);
	ImGui::DockBuilderFinish(dockspaceId);
	// Scene tab in front, Inspector and Console selected in their stacks.
	ImGui::SetWindowFocus(kWinGame);
	ImGui::SetWindowFocus(kWinConsole);
	ImGui::SetWindowFocus(kWinInspector);
	ImGui::SetWindowFocus(kWinScene);
}

// ---------------------------------------------------------------------------------------------
// Menu bar and toolbar
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::RequestScene(const std::string& path)
{
	if (m_core.IsPlaying()) m_core.RequestStop();
	if (m_core.IsDirty()) {
		m_pendingScene = path;
		m_openUnsavedModal = true;
	}
	else {
		m_core.RequestSceneSwap(path);
	}
}

void EditorWorkspace::DrawMenuBar()
{
	if (!ImGui::BeginMainMenuBar()) return;
	if (ImGui::BeginMenu("File")) {
		if (ImGui::BeginMenu("Open Scene")) {
			for (const std::string& s : m_core.GetSceneList()) {
				if (ImGui::MenuItem(s.c_str(), nullptr, s == m_core.GetCurrentScenePath())) RequestScene(s);
			}
			ImGui::EndMenu();
		}
		// Projects need the source checkout (templates/ and projects/ are read from it), not a packaged build.
		const bool haveCheckout = !ProjectBootstrap::Info().sourceRoot.empty();
		if (ImGui::MenuItem("New Project...", nullptr, false, haveCheckout && !m_core.IsPlaying())) m_openNewProject = true;
		if (ImGui::MenuItem("Open Project...", nullptr, false, haveCheckout && !m_core.IsPlaying())) m_openOpenProject = true;
		ImGui::Separator();
		if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !m_core.IsPlaying())) {
			m_core.SaveCurrentScene();
			ui::Toast("Scene saved", ui::ToastKind::Success);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Generate Lua Editor Support", nullptr, false, !m_core.IsPlaying())) {
			// Annotations and VS Code settings of the active Lua project (also written when a project opens).
			std::string report;
			const bool ok = NoxLua::GenerateLuaEditorSupport(Game(), report);
			std::cout << report;
			ui::Toast(ok ? "Lua editor support written (see the log)" : report, ok ? ui::ToastKind::Success : ui::ToastKind::Warning);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Exit")) m_quit = true;
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Edit")) {
		if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_undo.empty())) Undo();
		if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_redo.empty())) Redo();
		ImGui::Separator();
		auto sel = m_legacy.GetSelectedNode();
		const bool playing = m_core.IsPlaying();
		if (ImGui::MenuItem("Cut", "Ctrl+X", false, sel != nullptr && !playing)) CopySelection(true);
		if (ImGui::MenuItem("Copy", "Ctrl+C", false, sel != nullptr)) CopySelection(false);
		if (ImGui::MenuItem("Paste", "Ctrl+V", false, !playing)) PasteFromClipboard();
		ImGui::Separator();
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel != nullptr && !playing)) DuplicateSelectionWithUndo();
		if (ImGui::MenuItem("Delete", "Del", false, sel != nullptr && !playing)) DeleteSelectionWithUndo();
		if (ImGui::MenuItem("Select All", "Ctrl+A", false, !playing)) SelectAllTopLevel();
		ImGui::Separator();
		if (ImGui::MenuItem("Preferences...")) m_showPreferences = true;
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Node")) {
		if (ImGui::BeginMenu("Add Node", !m_core.IsPlaying())) {
			DrawAddNodeItems();
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Add collision to selection (recursive)", nullptr, false, !m_core.IsPlaying() && m_selection.Size() > 0)) {
			AddStaticCollisionToSelection();
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Window")) {
		ImGui::MenuItem(kWinHierarchy, nullptr, &m_showHierarchy);
		ImGui::MenuItem(kWinAssets, nullptr, &m_showAssets);
		ImGui::MenuItem(kWinInspector, nullptr, &m_showInspector);
		ImGui::MenuItem(kWinMaterials, nullptr, &m_showMaterials);
		ImGui::MenuItem("Lighting", nullptr, &m_showLegacyLighting);
		ImGui::MenuItem("Rendering", nullptr, &m_showLegacyRendering);
		ImGui::MenuItem("Editor Camera", nullptr, &m_showLegacyCamera);
		ImGui::MenuItem(kWinPhysics, nullptr, &m_showPhysics);
		ImGui::Separator();
		ImGui::MenuItem(kWinConsole, nullptr, &m_showConsole);
		ImGui::MenuItem(kWinBuild, nullptr, &m_showBuild);
		ImGui::MenuItem("Profiler", nullptr, &m_showLegacyPerformance);
		for (const EditorPanel& panel : EditorExtensions::Panels()) {
			if (panel.available && !panel.available(*this)) continue;
			ImGui::MenuItem(panel.title.c_str(), nullptr, &PanelOpen(panel));
		}
		ImGui::MenuItem("Animation", nullptr, &m_showLegacyAnimation);
		ImGui::MenuItem("State & Export", nullptr, &m_showLegacyState);
		ImGui::MenuItem(kWinPresets, nullptr, &m_showPresets);
		ImGui::Separator();
		ImGui::MenuItem("Authoring Overlay", nullptr, &m_showAuthoring);
		if (ImGui::MenuItem("Reset Layout")) {
			m_resetLayout = true;
			m_showHierarchy = m_showAssets = m_showInspector = m_showMaterials = m_showConsole = m_showBuild = true;
			for (const EditorPanel& panel : EditorExtensions::Panels()) PanelOpen(panel) = panel.openByDefault;
			m_showLegacyRendering = m_showLegacyLighting = m_showLegacyPerformance = true;
			m_showPhysics = true;
		}
		ImGui::MenuItem("ImGui Demo", nullptr, &m_showDemo);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Build")) {
		const bool busy = m_runner->IsRunning();
		if (ImGui::MenuItem("Build Release", nullptr, false, !busy)) StartTool("Build", "tools/Build.ps1", "-Configuration Release");
		if (ImGui::MenuItem("Run Standalone", nullptr, false, true)) RunStandalone();
		if (ImGui::MenuItem("Package", nullptr, false, !busy)) StartPackage();
		if (ImGui::MenuItem("Cancel", nullptr, false, busy)) m_runner->Cancel();
		ImGui::Separator();
		ImGui::MenuItem(kWinPresets, nullptr, &m_showPresets);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Help")) {
		ImGui::MenuItem("Controls", nullptr, &m_showHelp);
		ImGui::EndMenu();
	}
	ImGui::EndMainMenuBar();
}

void EditorWorkspace::DrawToolbar()
{
	ImGuiViewport* vp = ImGui::GetMainViewport();
	const float h = ImGui::GetFrameHeight() * 1.9f;
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, (h - ImGui::GetFrameHeight()) * 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin("##Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar(2);

	// Scene selection.
	ImGui::SetNextItemWidth(260.0f);
	const std::string current = m_core.GetCurrentScenePath();
	std::string label = fs::path(current).filename().string();
	if (m_core.IsDirty()) label += " *";
	if (ImGui::BeginCombo("##scene", label.c_str())) {
		for (const std::string& s : m_core.GetSceneList()) {
			if (ImGui::Selectable(s.c_str(), s == current)) RequestScene(s);
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ui::ToolIcon("##save", EditorExtensions::ResolveToolbarIcon("editor/save"), solace::Icon::Save, "Save", false, !m_core.IsPlaying(), "Save scene (Ctrl+S)")) {
		m_core.SaveCurrentScene();
		ui::Toast("Scene saved", ui::ToastKind::Success);
	}
	if (m_core.IsDirty()) {
		ImGui::SameLine(0, 2);
		solace::DirtyDot(true);
	}

	// Play controls, centred.
	const float playW = 3 * (ImGui::GetFrameHeight() + 8.0f);
	ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20.0f, (ImGui::GetWindowWidth() - playW) * 0.5f));
	const bool playing = m_core.IsPlaying();
	if (ui::ToolIcon("##play", EditorExtensions::ResolveToolbarIcon("editor/play"), solace::Icon::Play, nullptr, playing, !playing, "Play (F5)")) {
		m_core.RequestPlay();
		FocusGame();
	}
	ImGui::SameLine();
	if (ui::ToolIcon("##pause", EditorExtensions::ResolveToolbarIcon("editor/pause"), solace::Icon::Pause, nullptr, m_core.IsPaused(), playing, "Pause (F6)")) m_core.TogglePause();
	ImGui::SameLine();
	if (ui::Tool("##stop", solace::Icon::Stop, nullptr, false, playing, "Stop (Shift+F5) - restores the pre-play state")) m_core.RequestStop();

	// Build / run / package on the right.
	const bool busy = m_runner->IsRunning();
	ImGui::SameLine(ImGui::GetWindowWidth() - 470.0f);
	if (busy) {
		const float p = m_runner->Progress();
		ui::Progress(p, 110.0f);
		ImGui::SameLine();
	}
	else {
		ImGui::Dummy(ImVec2(110, 1));
		ImGui::SameLine();
	}
	if (ui::Tool("##build", solace::Icon::Build, "Build", false, !busy, "Build Release (tools/Build.ps1)")) StartTool("Build", "tools/Build.ps1", "-Configuration Release");
	ImGui::SameLine();
	if (ui::Tool("##run", solace::Icon::Run, "Run", false, true, "Run Standalone with the selected launch preset")) RunStandalone();
	ImGui::SameLine();
	if (ui::Tool("##package", solace::Icon::Package, "Package", false, !busy, "Package a self-contained build (tools/Package.ps1)")) StartPackage();
	ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Viewports
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::EnsureTarget(Viewport& vp, int width, int height)
{
	width = std::max(width, 16);
	height = std::max(height, 16);
	if (vp.fbo && vp.texWidth == width && vp.texHeight == height) return;
	rhi::Device& gpu = rhi::Gpu();
	if (!vp.fbo) {
		vp.fbo = gpu.CreateFramebuffer(rhi::FramebufferDesc{});
		vp.color = gpu.CreateTexture(rhi::TextureDesc{});
		rhi::TextureDesc depthDesc;
		depthDesc.format = rhi::Format::D24S8;
		vp.depth = gpu.CreateTexture(depthDesc);
	}
	gpu.ResizeTexture(vp.color, width, height, 1, 1);
	gpu.SetTextureFilter(vp.color, rhi::Filter::Linear, rhi::Filter::Linear);
	gpu.SetTextureWrapAxis(vp.color, 0, rhi::Wrap::ClampToEdge);
	gpu.SetTextureWrapAxis(vp.color, 1, rhi::Wrap::ClampToEdge);
	gpu.ResizeTexture(vp.depth, width, height, 1, 1);
	gpu.BindFramebuffer(vp.fbo);
	gpu.SetAttachment(vp.fbo, rhi::AttachmentPoint::Color0, rhi::Attachment{ vp.color, 0, rhi::kAllLayers });
	gpu.SetAttachment(vp.fbo, rhi::AttachmentPoint::DepthStencil, rhi::Attachment{ vp.depth });
	gpu.SetClearColor(glm::vec4(0.02f, 0.02f, 0.03f, 1.0f));
	gpu.Clear(rhi::ClearColor | rhi::ClearDepth);
	gpu.BindFramebuffer(rhi::kBackBuffer);
	vp.texWidth = width;
	vp.texHeight = height;
	vp.rendered = false;
}

void EditorWorkspace::DrawViewportImage(Viewport& vp)
{
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	vp.pos = ImGui::GetCursorScreenPos();
	vp.size = ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, 1.0f));
	const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
	EnsureTarget(vp, static_cast<int>(vp.size.x * scale.x), static_cast<int>(vp.size.y * scale.y));
	if (vp.rendered) {
		ImGui::Image((ImTextureID)rhi::Gpu().ImGuiTextureId(vp.color), vp.size, ImVec2(0, 1), ImVec2(1, 0));
	}
	else {
		ImGui::Dummy(vp.size);
	}
	vp.hovered = ImGui::IsItemHovered();
}

void EditorWorkspace::DrawGameViewport()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	if (m_focusGameNextFrame) {
		ImGui::SetNextWindowFocus();
		m_focusGameNextFrame = false;
	}
	const bool open = ImGui::Begin(kWinGame, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	m_game.visible = open && !ImGui::IsWindowCollapsed();
	m_game.focused = m_game.visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (m_game.visible) {
		DrawViewportImage(m_game);
		if (m_game.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ImGui::SetWindowFocus();
		if (IGameModule* game = m_core.GetGameModule()) {
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(m_game.pos, ImVec2(m_game.pos.x + m_game.size.x, m_game.pos.y + m_game.size.y), true);
			game->DrawOverlay(dl, glm::vec2(m_game.pos.x, m_game.pos.y), glm::vec2(m_game.size.x, m_game.size.y), m_dt);
			if (m_core.IsPlaying() && !m_game.focused) {
				const char* hint = "Click to control the game";
				const ImVec2 ts = ImGui::CalcTextSize(hint);
				const ImVec2 c(m_game.pos.x + m_game.size.x * 0.5f, m_game.pos.y + 28.0f);
				dl->AddRectFilled(ImVec2(c.x - ts.x * 0.5f - 10, c.y - 4), ImVec2(c.x + ts.x * 0.5f + 10, c.y + ts.y + 4), IM_COL32(0, 0, 0, 150), 6.0f);
				dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y), IM_COL32(255, 255, 255, 220), hint);
			}
			dl->PopClipRect();
		}
	}
	ImGui::End();
}

bool EditorWorkspace::ProjectToViewport(const Viewport& vp, const glm::vec3& world, ImVec2& out) const
{
	auto cam = m_core.GetCamera();
	if (!cam || vp.size.x < 1 || vp.size.y < 1) return false;
	const glm::mat4 proj = glm::perspective(glm::radians(cam->GetCameraFov()), vp.size.x / vp.size.y, 0.05f, 5000.0f);
	const glm::vec4 clip = proj * cam->GetViewMatrix() * glm::vec4(world, 1.0f);
	if (clip.w <= 0.05f) return false;
	const glm::vec3 n = glm::vec3(clip) / clip.w;
	out = ImVec2(vp.pos.x + (n.x * 0.5f + 0.5f) * vp.size.x, vp.pos.y + (0.5f - n.y * 0.5f) * vp.size.y);
	return true;
}

bool EditorWorkspace::ViewportRayToGround(const Viewport& vp, const ImVec2& mouse, glm::vec3& out) const
{
	auto cam = m_core.GetCamera();
	if (!cam) return false;
	const glm::mat4 proj = glm::perspective(glm::radians(cam->GetCameraFov()), vp.size.x / vp.size.y, 0.05f, 5000.0f);
	const glm::mat4 inv = glm::inverse(proj * cam->GetViewMatrix());
	const float nx = (mouse.x - vp.pos.x) / vp.size.x * 2.0f - 1.0f;
	const float ny = 1.0f - (mouse.y - vp.pos.y) / vp.size.y * 2.0f;
	glm::vec4 a = inv * glm::vec4(nx, ny, -1.0f, 1.0f), b = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
	const glm::vec3 o = glm::vec3(a) / a.w, d = glm::normalize(glm::vec3(b) / b.w - o);
	if (std::abs(d.y) < 1e-4f) return false;
	const float t = -o.y / d.y;
	if (t < 0.0f) return false;
	out = o + d * t;
	return true;
}

void EditorWorkspace::DrawSceneViewport()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	const bool open = ImGui::Begin(kWinScene, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	m_scene.visible = open && !ImGui::IsWindowCollapsed();
	m_scene.focused = m_scene.visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (!m_scene.visible) {
		ImGui::End();
		return;
	}
	DrawViewportImage(m_scene);
	ImGuiIO& io = ImGui::GetIO();

	// Content Browser drop: a prefab or model lands on the ground point under the cursor.
	if (!m_core.IsPlaying() && ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* asset = ImGui::AcceptDragDropPayload(nox::editor::kAssetDragType)) {
			glm::vec3 ground;
			if (ViewportRayToGround(m_scene, io.MousePos, ground)) {
				PlaceAssetInScene(static_cast<const char*>(asset->Data), m_legacy.GetSelectedNode(), &ground);
			}
		}
		ImGui::EndDragDropTarget();
	}

	// Right mouse held in the viewport: fly camera (WASD/QE, mouse look).
	if (m_scene.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
		ImGui::SetWindowFocus();
		m_core.SetEditorCameraActive(true);
	}
	if (m_core.IsEditorCameraActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_core.SetEditorCameraActive(false);
	if (m_scene.hovered && io.MouseWheel != 0.0f && !m_core.IsEditorCameraActive()) {
		if (auto cam = m_core.GetCamera()) {
			// Wheel: dolly along the view direction.
			const glm::vec3 f = cam->GetCameraFrontVector();
			PoseCamera(*cam, cam->GetCameraPosition() + f * io.MouseWheel * std::max(cam->GetCameraMovementSpeed() * 0.25f, 1.0f), f);
		}
	}

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(m_scene.pos, ImVec2(m_scene.pos.x + m_scene.size.x, m_scene.pos.y + m_scene.size.y), true);
	if (m_showAuthoring) DrawAuthoringOverlay(dl);
	else m_handles.clear();
	m_core.DrawDebugOverlay(dl, m_scene.pos.x, m_scene.pos.y, m_scene.size.x, m_scene.size.y);
	DrawGizmo(m_scene);
	UpdateHover(io);
	DrawSelectionBounds(dl);

	// Click, Ctrl+click and marquee selection (not on the gizmo or authoring handles).
	UpdateMarquee(io, dl);

	// In-viewport tool strip.
	ImGui::SetCursorScreenPos(ImVec2(m_scene.pos.x + 8, m_scene.pos.y + 8));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
	if (ui::Seg("##gizmoop", &m_gizmoOp, { {"Move", ImGuizmo::TRANSLATE}, {"Rotate", ImGuizmo::ROTATE}, {"Scale", ImGuizmo::SCALE} })) {}
	ImGui::SameLine();
	if (ui::Seg("##gizmomode", &m_gizmoMode, { {"Local", ImGuizmo::LOCAL}, {"World", ImGuizmo::WORLD} })) {}
	ImGui::SameLine();
	ImGui::Checkbox("Snap", &m_snap);
	ImGui::SameLine();
	ImGui::Checkbox("Authoring", &m_showAuthoring);
	ImGui::SameLine();
	if (ImGui::Button("Outline")) ImGui::OpenPopup("##outline_style");
	DrawOutlineStylePopup();
	if (auto cam = m_core.GetCamera()) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90);
		float speed = cam->GetCameraMovementSpeed();
		if (ImGui::DragFloat("##camspeed", &speed, 0.2f, 0.5f, 200.0f, "cam %.0f m/s")) cam->m_movementSpeed = speed;
	}
	ImGui::PopStyleVar();
	if (m_scene.size.y > 80) {
		ImGui::SetCursorScreenPos(ImVec2(m_scene.pos.x + 10, m_scene.pos.y + m_scene.size.y - ImGui::GetTextLineHeight() - 8));
		ImGui::TextDisabled("RMB + WASD/QE fly | wheel dolly | LMB select / drag handles | W/E/R gizmo | F focus");
	}
	dl->PopClipRect();
	ImGui::End();
}

void EditorWorkspace::DrawGizmo(const Viewport& vp)
{
	if (m_selection.Size() > 1) {
		DrawGroupGizmo(vp);
		return;
	}
	auto node = m_legacy.GetSelectedNode();
	auto cam = m_core.GetCamera();
	if (!node || !cam || m_core.IsEditorCameraActive() || node->IsFolder()) {
		m_wasUsingGizmo = false;
		return;
	}
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
	ImGuizmo::SetRect(vp.pos.x, vp.pos.y, vp.size.x, vp.size.y);
	const glm::mat4 view = cam->GetViewMatrix();
	const glm::mat4 proj = glm::perspective(glm::radians(cam->GetCameraFov()), vp.size.x / vp.size.y, 0.05f, 5000.0f);
	glm::mat4 model = node->GetWorldPosition4x4();
	float snap[3];
	const int op = m_gizmoOp;
	const float s = op == ImGuizmo::ROTATE ? m_snapValues[1] : (op == ImGuizmo::SCALE ? m_snapValues[2] : m_snapValues[0]);
	snap[0] = snap[1] = snap[2] = s;
	if (!m_wasUsingGizmo) m_gizmoBefore = CaptureTRS(node);
	ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), static_cast<ImGuizmo::OPERATION>(op),
		static_cast<ImGuizmo::MODE>(m_gizmoMode), glm::value_ptr(model), nullptr, m_snap ? snap : nullptr);
	const bool using_ = ImGuizmo::IsUsing();
	if (using_) {
		node->SetWorldTransform(model);
		m_core.InvalidateSceneCaches();
	}
	if (m_wasUsingGizmo && !using_) {
		const auto before = m_gizmoBefore;
		const auto after = CaptureTRS(node);
		std::weak_ptr<SceneNode> weak = node;
		Push("Transform " + node->GetName(), [weak, before] { if (auto n = weak.lock()) ApplyTRS(n, before); },
			[weak, after] { if (auto n = weak.lock()) ApplyTRS(n, after); });
		m_core.MarkDirty();
	}
	m_wasUsingGizmo = using_;
}

namespace {

// Pivot of a group gizmo: the centre of the nodes' bounds when any of them has a model, else the centroid of their
// world positions.
glm::vec3 GroupPivot(const std::vector<std::shared_ptr<SceneNode>>& nodes)
{
	bool anyModel = false;
	for (const auto& n : nodes) anyModel = anyModel || n->GetModel() != nullptr;
	if (anyModel) {
		const auto [lo, hi] = GizmoRayCast::CalculateMultiSelectionBounds(nodes);
		return (lo + hi) * 0.5f;
	}
	glm::vec3 centre(0.0f);
	for (const auto& n : nodes) centre += n->GetWorldPosition();
	return nodes.empty() ? centre : centre / static_cast<float>(nodes.size());
}

// Applies a world-space group delta to one top-level node. The position goes through the whole delta; the orientation
// takes the delta's rotation (composed in world space, so no shear from the rotation); the scale takes the delta's
// scale: uniform scale multiplies the world scale exactly, non-uniform scale multiplies each axis of the node's own
// frame (documented in docs/editor.md, since a rotated node cannot take a non-uniform world scale without shearing).
void ApplyGroupDelta(const std::shared_ptr<SceneNode>& node, const glm::mat4& delta)
{
	glm::vec3 dScale, dTrans, dSkew;
	glm::quat dRot;
	glm::vec4 dPersp;
	glm::decompose(delta, dScale, dRot, dTrans, dSkew, dPersp);

	glm::vec3 scale, trans, skew;
	glm::quat rot;
	glm::vec4 persp;
	glm::decompose(node->GetWorldPosition4x4(), scale, rot, trans, skew, persp);

	const glm::vec3 newTrans = glm::vec3(delta * glm::vec4(trans, 1.0f));
	const glm::quat newRot = glm::normalize(dRot * rot);
	const bool uniform = std::abs(dScale.x - dScale.y) < 1e-4f && std::abs(dScale.y - dScale.z) < 1e-4f;
	const glm::vec3 newScale = uniform ? scale * dScale.x : scale * dScale;
	const glm::mat4 world = glm::translate(glm::mat4(1.0f), newTrans) * glm::mat4_cast(newRot) * glm::scale(glm::mat4(1.0f), newScale);
	node->SetWorldTransform(world);
}

} // namespace

// Several nodes selected: one gizmo at the centre of their top-level nodes. Dragging moves, turns or scales each
// top-level node about that pivot (its children follow it). One undo step per drag. Rotation and non-uniform scale
// of a node that is already rotated can shear it, as with any world-space group transform.
void EditorWorkspace::DrawGroupGizmo(const Viewport& vp)
{
	auto cam = m_core.GetCamera();
	const auto top = m_selection.TopLevel();
	if (!cam || top.empty() || m_core.IsEditorCameraActive()) {
		m_wasUsingGizmo = false;
		return;
	}
	if (!m_wasUsingGizmo) m_groupModel = glm::translate(glm::mat4(1.0f), GroupPivot(top));
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
	ImGuizmo::SetRect(vp.pos.x, vp.pos.y, vp.size.x, vp.size.y);
	const glm::mat4 view = cam->GetViewMatrix();
	const glm::mat4 proj = glm::perspective(glm::radians(cam->GetCameraFov()), vp.size.x / vp.size.y, 0.05f, 5000.0f);
	float snap[3];
	const int op = m_gizmoOp;
	const float s = op == ImGuizmo::ROTATE ? m_snapValues[1] : (op == ImGuizmo::SCALE ? m_snapValues[2] : m_snapValues[0]);
	snap[0] = snap[1] = snap[2] = s;
	const glm::mat4 pre = m_groupModel;
	ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), static_cast<ImGuizmo::OPERATION>(op),
		static_cast<ImGuizmo::MODE>(m_gizmoMode), glm::value_ptr(m_groupModel), nullptr, m_snap ? snap : nullptr);
	const bool using_ = ImGuizmo::IsUsing();
	if (using_) {
		if (!m_wasUsingGizmo) {
			m_groupBefore.clear();
			for (const auto& n : top) m_groupBefore.push_back({ n, CaptureTRS(n) });
		}
		const glm::mat4 delta = m_groupModel * glm::inverse(pre);   // pivotT * (this frame's R, S) * pivotT^-1, with the move
		for (const auto& n : top) ApplyGroupDelta(n, delta);
		m_core.InvalidateSceneCaches();
	}
	if (m_wasUsingGizmo && !using_) {
		std::vector<std::pair<std::weak_ptr<SceneNode>, std::array<float, 10>>> before = m_groupBefore;
		std::vector<std::pair<std::weak_ptr<SceneNode>, std::array<float, 10>>> after;
		for (const auto& entry : before) {
			if (auto n = entry.first.lock()) after.push_back({ entry.first, CaptureTRS(n) });
		}
		const std::string label = before.size() == 1 ? "Transform " + (before[0].first.lock() ? before[0].first.lock()->GetName() : std::string("node"))
			: "Transform " + std::to_string(before.size()) + " nodes";
		Push(label,
			[before] { for (const auto& e : before) if (auto n = e.first.lock()) ApplyTRS(n, e.second); },
			[after] { for (const auto& e : after) if (auto n = e.first.lock()) ApplyTRS(n, e.second); });
		m_core.MarkDirty();
	}
	m_wasUsingGizmo = using_;
}

void EditorWorkspace::DrawOutlineStylePopup()
{
	if (!ImGui::BeginPopup("##outline_style")) return;
	bool changed = false;
	changed |= ImGui::ColorEdit3("Outline colour", m_outline.color, ImGuiColorEditFlags_NoInputs);
	changed |= ImGui::SliderInt("Width (px)", &m_outline.width, 1, 4);
	changed |= ImGui::Checkbox("Show parts behind other objects (dashed)", &m_outline.showOccluded);
	changed |= ImGui::Checkbox("Hover highlight", &m_outline.hover);
	ImGui::EndPopup();
	if (changed) {
		if (!m_outline.hover) m_hoverNode.reset();
		SaveSettings();
	}
}

// Hover highlight: the node under the mouse in the Scene viewport, picked about ten times a second.
// Off while dragging a gizmo or handle, flying the camera, or playing.
void EditorWorkspace::UpdateHover(const ImGuiIO& io)
{
	m_hoverTimer -= io.DeltaTime;
	bool pickable = m_outline.hover && m_scene.visible && m_scene.hovered && !m_core.IsPlaying() &&
		!m_core.IsEditorCameraActive() && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() && m_dragHandle < 0;
	if (pickable) {
		for (const ViewportHandle& h : m_handles) {
			ImVec2 p;
			if (ProjectToViewport(m_scene, h.world, p) && std::hypot(p.x - io.MousePos.x, p.y - io.MousePos.y) < 10.0f) {
				pickable = false;
				break;
			}
		}
	}
	if (!pickable) {
		m_hoverNode.reset();
		m_hoverTimer = 0.0f;
		return;
	}
	if (m_hoverTimer > 0.0f) return;
	m_hoverTimer = 0.1f;
	const ImVec2 scale = io.DisplayFramebufferScale;
	const int x = static_cast<int>((io.MousePos.x - m_scene.pos.x) * scale.x);
	const int y = static_cast<int>((io.MousePos.y - m_scene.pos.y) * scale.y);
	m_hoverNode = m_core.PickNode(x, y, m_scene.texWidth, m_scene.texHeight);
}

// Nodes without a mesh (lights, cameras, empty nodes) cannot be outlined: their selection is a wire box
// around the node (the bounds the debug overlay uses) and a ring at its origin.
void EditorWorkspace::DrawSelectionBounds(ImDrawList* dl)
{
	if (m_core.IsPlaying() || !m_scene.visible) return;
	for (const auto& node : m_selection.Nodes()) {
	if (!node || node->GetModel()) continue;
	const glm::mat4 world = node->GetWorldPosition4x4();
	ImVec2 corner[8];
	bool visible[8];
	for (int i = 0; i < 8; ++i) {
		const glm::vec3 local((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
		visible[i] = ProjectToViewport(m_scene, glm::vec3(world * glm::vec4(local, 1.0f)), corner[i]);
	}
	const ImU32 col = IM_COL32(static_cast<int>(m_outline.color[0] * 255.0f), static_cast<int>(m_outline.color[1] * 255.0f),
		static_cast<int>(m_outline.color[2] * 255.0f), 255);
	static const int edges[12][2] = { {0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7} };
	for (const auto& e : edges) {
		if (visible[e[0]] && visible[e[1]]) dl->AddLine(corner[e[0]], corner[e[1]], col, 2.0f);
	}
	ImVec2 origin;
	if (ProjectToViewport(m_scene, glm::vec3(world[3]), origin)) dl->AddCircle(origin, 9.0f, col, 0, 2.0f);
	}
}

void EditorWorkspace::DrawAuthoringOverlay(ImDrawList* dl)
{
	m_handles.clear();
	const EditorExtensions::ViewportOverlay& overlay = EditorExtensions::GetViewportOverlay();
	if (!overlay) return;
	overlay(*this, dl);

	// Handles: draw, hover, drag on the ground plane (undoable as one game edit).
	const Viewport& vp = m_scene;
	ImGuiIO& io = ImGui::GetIO();
	int hovered = -1;
	for (size_t i = 0; i < m_handles.size(); ++i) {
		ImVec2 s;
		if (!ProjectToViewport(vp, m_handles[i].world, s)) continue;
		const bool isHover = vp.hovered && std::hypot(s.x - io.MousePos.x, s.y - io.MousePos.y) < 10.0f;
		if (isHover && hovered < 0) hovered = static_cast<int>(i);
		const float r = m_handles[i].radius;
		dl->AddCircleFilled(s, r + (isHover || static_cast<int>(i) == m_dragHandle ? 2.0f : 0.0f), m_handles[i].color);
		dl->AddCircle(s, r + 2.0f, IM_COL32(0, 0, 0, 160), 0, 1.5f);
	}
	if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() && !IsPlaying()) {
		m_dragHandle = hovered;
		m_dragBefore = GetAuthoredGameJson();
		if (m_handles[hovered].onPress) m_handles[hovered].onPress();
	}
	if (m_dragHandle >= 0 && m_dragHandle < static_cast<int>(m_handles.size())) {
		const ViewportHandle& h = m_handles[m_dragHandle];
		glm::vec3 ground;
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && ViewportRayToGround(vp, io.MousePos, ground)) {
			if (h.onDrag) h.onDrag(glm::vec2(ground.x, ground.z));
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			const json before = m_dragBefore, after = GetAuthoredGameJson();
			if (before != after) {
				Push("Move handle", [this, before] { m_core.ApplyGameJson(before); }, [this, after] { m_core.ApplyGameJson(after); });
				m_core.ApplyGameJson(after);
				m_core.MarkDirty();
			}
			m_dragHandle = -1;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Application extensions (EditorHost)
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::RequestPlay()
{
	m_core.RequestPlay();
	FocusGame();
}

void EditorWorkspace::RequestStop()
{
	m_core.RequestStop();
}

bool EditorWorkspace::IsPlaying() const { return m_core.IsPlaying(); }
void EditorWorkspace::MarkDirty() { m_core.MarkDirty(); }
void EditorWorkspace::ApplyGameJson(const json& game) { m_core.ApplyGameJson(game); }
void EditorWorkspace::ApplyRuntimeChangesToAuthored() { m_core.ApplyRuntimeChangesToAuthored(); }
IGameModule* EditorWorkspace::Game() { return m_core.GetGameModule(); }
SceneGraph* EditorWorkspace::GetSceneGraph() { return m_core.GetSceneGraph().get(); }
ModularRenderer* EditorWorkspace::GetRenderer() { return m_core.GetRenderer().get(); }

void EditorWorkspace::PushUndo(const std::string& label, std::function<void()> undo, std::function<void()> redo)
{
	Push(label, std::move(undo), std::move(redo));
}

bool EditorWorkspace::ProjectToViewport(const glm::vec3& world, ImVec2& out) const
{
	return ProjectToViewport(m_scene, world, out);
}

void EditorWorkspace::ShowPanel(const std::string& id)
{
	m_extOpen[id] = true;
}

json EditorWorkspace::GetAuthoredGameJson() const
{
	IGameModule* game = m_core.GetGameModule();
	return game ? game->SaveGameJson() : json::object();
}

bool& EditorWorkspace::PanelOpen(const EditorPanel& panel)
{
	auto it = m_extOpen.find(panel.id);
	if (it == m_extOpen.end()) {
		const bool saved = m_extSaved.is_object() && m_extSaved.contains(panel.id) && m_extSaved[panel.id].is_boolean()
			? m_extSaved[panel.id].get<bool>() : panel.openByDefault;
		it = m_extOpen.emplace(panel.id, saved).first;
	}
	return it->second;
}

void EditorWorkspace::CommitGameEdit(const std::string& label)
{
	IGameModule* game = m_core.GetGameModule();
	if (!game) return;
	const json before = m_gameSnapshot, after = game->SaveGameJson();
	if (before.is_null() || before == after) return;
	Push(label, [this, before] { m_core.ApplyGameJson(before); }, [this, after] { m_core.ApplyGameJson(after); });
	m_core.ApplyGameJson(after);
	m_gameSnapshot = after;
	m_core.MarkDirty();
}

void EditorWorkspace::ChooseRenderTarget()
{
	// One viewport per frame keeps the renderer's temporal history coherent.
	if (m_scene.focused) m_lastFocused = Target::Scene;
	else if (m_game.focused) m_lastFocused = Target::Game;
	Target t = Target::None;
	if (m_scene.visible && m_game.visible) t = m_core.IsPlaying() ? Target::Game : m_lastFocused;
	else if (m_game.visible) t = Target::Game;
	else if (m_scene.visible) t = Target::Scene;
	m_renderTarget = t;
	if (t == Target::Scene) m_scene.rendered = true;
	if (t == Target::Game) m_game.rendered = true;
}

// ---------------------------------------------------------------------------------------------
// Hierarchy / assets / inspector / materials
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::SelectNode(const std::shared_ptr<SceneNode>& node)
{
	m_selection.Set(node);
	m_legacy.SetSelectedNode(node);
}

void EditorWorkspace::DrawHierarchyRow(const nox::editor::HierarchyRow& row)
{
	const std::shared_ptr<SceneNode> node = row.node;
	if (!node) return;
	const bool playing = m_core.IsPlaying();
	const bool folder = node->IsFolder();
	const std::string name = node->GetName().empty() ? std::string("(unnamed)") : node->GetName();
	ImGui::PushID(node.get());
	const float frame = ImGui::GetFrameHeight();
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + row.depth * ImGui::GetStyle().IndentSpacing);
	if (row.hasChildren) {
		if (ImGui::ArrowButton("##toggle", row.open ? ImGuiDir_Down : ImGuiDir_Right)) m_hierarchyView.SetCollapsed(row.path, row.open);
	}
	else {
		ImGui::Dummy(ImVec2(frame, frame));
	}
	ImGui::SameLine();
	const bool selected = m_legacy.GetSelectedNode() == node;
	const std::string label = folder ? name + "  [" + std::to_string(node->GetChildCount()) + "]" : name;
	if (folder) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 200, 120, 255));
	const bool clicked = ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAvailWidth);
	if (folder) ImGui::PopStyleColor();
	if (clicked) {
		const ImGuiIO& io = ImGui::GetIO();
		ClickSelect(node, io.KeyCtrl, io.KeyShift);
	}
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) BeginRename(node);
	const ImVec2 rowMin = ImGui::GetItemRectMin();
	const ImVec2 rowMax = ImGui::GetItemRectMax();
	if (ImGui::BeginDragDropSource()) {
		SceneNode* raw = node.get();
		ImGui::SetDragDropPayload(nox::editor::kNodeDragType, &raw, sizeof(raw));
		ImGui::TextUnformatted(name.c_str());
		ImGui::EndDragDropSource();
	}
	// Drop on a row: the top and bottom quarters insert before or after it under the same parent (this is how a
	// node leaves a folder or changes its place among siblings); the middle makes the node a child of the row.
	// Prefab assets dropped on a row are instantiated under it.
	if (ImGui::BeginDragDropTarget()) {
		const float band = (rowMax.y - rowMin.y) * 0.25f;
		const float y = ImGui::GetMousePos().y;
		const bool before = y < rowMin.y + band;
		const bool after = !before && y > rowMax.y - band;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImU32 markCol = IM_COL32(255, 210, 90, 255);
		if (before) dl->AddLine(ImVec2(rowMin.x, rowMin.y), ImVec2(rowMax.x, rowMin.y), markCol, 2.0f);
		else if (after) dl->AddLine(ImVec2(rowMin.x, rowMax.y), ImVec2(rowMax.x, rowMax.y), markCol, 2.0f);
		else dl->AddRect(rowMin, rowMax, markCol, 0.0f, 0, 2.0f);
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(nox::editor::kNodeDragType)) {
			if (payload->DataSize == sizeof(SceneNode*)) {
				SceneNode* raw = *static_cast<SceneNode* const*>(payload->Data);
				if (auto dragged = FindSharedNode(RootNode(), raw)) {
					if (before || after) MoveNodeNextTo(dragged, node, after);
					else ReparentWithUndo(dragged, node);
				}
			}
		}
		if (const ImGuiPayload* asset = ImGui::AcceptDragDropPayload(nox::editor::kAssetDragType)) {
			PlaceAssetInScene(static_cast<const char*>(asset->Data), node, nullptr);
		}
		ImGui::EndDragDropTarget();
	}
	if (ImGui::BeginPopupContextItem()) {
		if (!m_selection.Contains(node)) SelectNode(node);
		if (ImGui::MenuItem("Focus")) {
			if (auto cam = m_core.GetCamera()) {
				const glm::vec3 p = node->GetWorldPosition();
				const glm::vec3 eye = p + glm::vec3(-6, 4, -6);
				const glm::vec3 d = glm::normalize(p - eye);
				cam->SetPose(eye, glm::vec3(0, 1, 0), glm::degrees(std::atan2(d.z, d.x)), glm::degrees(std::asin(d.y)));
			}
		}
		if (ImGui::MenuItem("Rename", "F2", false, !playing)) BeginRename(node);
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !playing)) DuplicateNodeWithUndo(node);
		if (ImGui::MenuItem("Save as Prefab...", nullptr, false, !playing)) BeginSavePrefab(node);
		ImGui::Separator();
		if (ImGui::MenuItem("New Folder", nullptr, false, !playing)) CreateFolderUnder(node);
		if (ImGui::MenuItem("Group into Folder", nullptr, false, !playing && !node->IsGenerated() && node->parentNode.lock() != nullptr)) {
			GroupIntoFolderWithUndo(node);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete", "Del", false, !playing)) {
			DeleteNodeWithUndo(node);
			ImGui::EndPopup();
			ImGui::PopID();
			return;
		}
		if (ImGui::BeginMenu("Add Child", !playing)) {
			DrawAddNodeItems();
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}
	const char* tag = "";
	ImU32 tagCol = IM_COL32(150, 150, 160, 255);
	switch (node->GetNodeType()) {
	case SceneNode::LIGHT: tag = "LIGHT"; tagCol = IM_COL32(255, 210, 90, 255); break;
	case SceneNode::MODEL: tag = "MESH"; tagCol = IM_COL32(120, 180, 255, 255); break;
	case SceneNode::AUDIO: tag = "AUDIO"; tagCol = IM_COL32(90, 230, 220, 255); break;
	case SceneNode::CAMERA: tag = "CAM"; tagCol = IM_COL32(120, 255, 140, 255); break;
	case SceneNode::FOLDER: tag = "FOLDER"; tagCol = IM_COL32(230, 200, 120, 255); break;
	default: break;
	}
	if (tag[0]) {
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(tag).x - 4);
		ImGui::PushStyleColor(ImGuiCol_Text, tagCol);
		ImGui::TextUnformatted(tag);
		ImGui::PopStyleColor();
	}
	ImGui::PopID();
}

void EditorWorkspace::DrawHierarchy()
{
	const bool hierarchyOpen = ImGui::Begin(kWinHierarchy, &m_showHierarchy);
	m_hierarchyFocused = hierarchyOpen && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
	if (!hierarchyOpen) { ImGui::End(); return; }
	solace::SearchField("##hfilter", m_hierarchyFilter, sizeof(m_hierarchyFilter), "Search: name or t:folder");
	if (ImGui::SmallButton("Collapse")) m_hierarchyView.CollapseAll(RootNode());
	ImGui::SameLine();
	if (ImGui::SmallButton("Expand")) m_hierarchyView.ExpandAll();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(170.0f);
	const char* sortLabel = m_hierarchySort == nox::editor::HierarchySort::NameFoldersFirst ? "Name, folders first" : "Scene order";
	if (ImGui::BeginCombo("##hsort", sortLabel)) {
		if (ImGui::Selectable("Scene order", m_hierarchySort == nox::editor::HierarchySort::SceneOrder)) m_hierarchySort = nox::editor::HierarchySort::SceneOrder;
		if (ImGui::Selectable("Name, folders first", m_hierarchySort == nox::editor::HierarchySort::NameFoldersFirst)) m_hierarchySort = nox::editor::HierarchySort::NameFoldersFirst;
		ImGui::EndCombo();
	}
	ImGui::Separator();
	ImGui::BeginChild("##tree");
	if (auto root = RootNode()) {
		// A selection made elsewhere (viewport, Lua, undo) expands the path to it once.
		const auto selected = m_legacy.GetSelectedNode();
		if (selected && selected != m_hierarchyRevealed.lock()) m_hierarchyView.Reveal(selected.get());
		m_hierarchyRevealed = selected;
		const nox::editor::HierarchyFilter filter = nox::editor::HierarchyFilter::Parse(m_hierarchyFilter);
		const auto rows = m_hierarchyView.Build(root, filter, m_hierarchySort);
		if (rows.empty()) ImGui::TextDisabled("%s", filter.Active() ? "No node matches the search." : "The scene is empty. Right-click to add a node.");
		m_visibleRows.clear();
		for (const auto& row : rows) m_visibleRows.push_back(row.node);
		for (const auto& row : rows) DrawHierarchyRow(row);
		// Empty space below the rows: drop here to move a node to the scene root; right-click adds nodes.
		const float freeHeight = std::max(ImGui::GetContentRegionAvail().y, 12.0f);
		ImGui::InvisibleButton("##hierarchy_root", ImVec2(-1.0f, freeHeight));
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(nox::editor::kNodeDragType)) {
				if (payload->DataSize == sizeof(SceneNode*)) {
					SceneNode* raw = *static_cast<SceneNode* const*>(payload->Data);
					if (auto dragged = FindSharedNode(root, raw)) ReparentWithUndo(dragged, root);
				}
			}
			if (const ImGuiPayload* asset = ImGui::AcceptDragDropPayload(nox::editor::kAssetDragType)) {
				PlaceAssetInScene(static_cast<const char*>(asset->Data), root, nullptr);
			}
			ImGui::EndDragDropTarget();
		}
		if (ImGui::BeginPopupContextItem("##hierarchy_ctx", ImGuiPopupFlags_MouseButtonRight)) {
			SelectNode(nullptr);
			if (ImGui::MenuItem("New Folder", nullptr, false, !m_core.IsPlaying())) CreateFolderUnder(root);
			if (ImGui::BeginMenu("Add Node", !m_core.IsPlaying())) {
				DrawAddNodeItems();
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Node operations (undoable): add, duplicate, delete, reparent, rename
// ---------------------------------------------------------------------------------------------
std::shared_ptr<SceneNode> EditorWorkspace::RootNode() const
{
	auto graph = m_core.GetSceneGraph();
	return graph ? graph->GetRoot() : nullptr;
}

nox::editor::UndoPush EditorWorkspace::UndoSink()
{
	// Play-mode edits are not authored, so they never enter the undo stack.
	if (m_core.IsPlaying()) return nullptr;
	return [this](const std::string& label, std::function<void()> undo, std::function<void()> redo) {
		Push(label, std::move(undo), std::move(redo));
	};
}

nox::editor::NodeOps EditorWorkspace::CoreNodeOps()
{
	nox::editor::NodeOps ops;
	ops.serialize = [this](const std::shared_ptr<SceneNode>& n) { return m_core.SerializeNodeForEditor(n); };
	ops.instantiate = [this](const json& j, const std::shared_ptr<SceneNode>& p) { return m_core.InstantiateNodeFromJson(j, p); };
	ops.destroy = [this](const std::shared_ptr<SceneNode>& n) { return m_core.DeleteNode(n); };
	ops.reparent = [this](const std::shared_ptr<SceneNode>& n, const std::shared_ptr<SceneNode>& p, int index, bool keepWorld) {
		return m_core.ReparentNode(n, p, index, keepWorld);
	};
	return ops;
}

void EditorWorkspace::NoteEdit()
{
	// Edits in play mode change the running world only; the authored scene stays clean.
	if (!m_core.IsPlaying()) m_core.MarkDirty();
	m_core.InvalidateSceneCaches();
}

void EditorWorkspace::AddNodeFromTemplate(const std::string& kind)
{
	if (m_core.IsPlaying()) return;
	const auto parent = m_legacy.GetSelectedNode();
	json nodeJson = NodeTemplate(kind);
	const glm::vec3 position = SpawnPosition(m_core, parent, nodeJson.value("type", "node") == "light" ? 3.0f : 0.0f);
	nodeJson["position"] = { position.x, position.y, position.z };
	const std::string label = "Add " + nodeJson.value("name", std::string("Node"));
	if (auto node = nox::editor::CreateNodeUndoable(m_nodeTable, CoreNodeOps(), UndoSink(), label, nodeJson, parent)) {
		SelectNode(node);
		NoteEdit();
	}
}

void EditorWorkspace::AddModelFromAsset(const std::string& path, const std::shared_ptr<SceneNode>& parent, const glm::vec3* at)
{
	if (m_core.IsPlaying()) return;
	const glm::vec3 position = PlacementPosition(m_core, parent, at);
	if (auto node = m_core.AddModelNode(path, position, parent)) {
		nox::editor::RecordCreatedNode(m_nodeTable, CoreNodeOps(), UndoSink(), "Add " + fs::path(path).stem().string(), node);
		SelectNode(node);
		NoteEdit();
	}
}

// Content Browser / asset drops: prefabs and models are placed under parent (at a world point when given).
bool EditorWorkspace::PlaceAssetInScene(const std::string& path, const std::shared_ptr<SceneNode>& parent, const glm::vec3* at)
{
	if (m_core.IsPlaying()) return false;
	if (nox::prefab::IsPrefabPath(path)) {
		InstantiatePrefabFromAsset(path, parent, at);
		return true;
	}
	const std::string ext = fs::path(path).extension().string();
	if (ext == ".glb" || ext == ".gltf" || ext == ".fbx") {
		AddModelFromAsset(path, parent, at);
		return true;
	}
	return false;
}

void EditorWorkspace::DrawAddNodeItems()
{
	const bool playing = m_core.IsPlaying();
	if (ImGui::MenuItem("Empty", nullptr, false, !playing)) AddNodeFromTemplate("empty");
	if (ImGui::MenuItem("Folder", nullptr, false, !playing)) CreateFolderUnder(m_legacy.GetSelectedNode());
	ImGui::Separator();
	if (ImGui::MenuItem("Directional Light", nullptr, false, !playing)) AddNodeFromTemplate("directional");
	if (ImGui::MenuItem("Point Light", nullptr, false, !playing)) AddNodeFromTemplate("point");
	if (ImGui::MenuItem("Spot Light", nullptr, false, !playing)) AddNodeFromTemplate("spot");
	ImGui::Separator();
	if (ImGui::MenuItem("Audio Source", nullptr, false, !playing)) AddNodeFromTemplate("audio");
	if (ImGui::MenuItem("Camera", nullptr, false, !playing)) AddNodeFromTemplate("camera");
	if (ImGui::MenuItem("GUI", nullptr, false, !playing)) AddNodeFromTemplate("gui");
	if (ImGui::MenuItem("LPV Volume", nullptr, false, !playing)) AddNodeFromTemplate("lpv_volume");
	ImGui::Separator();
	if (ImGui::MenuItem("Model from asset...", nullptr, false, !playing)) m_openAddModel = true;
}

void EditorWorkspace::DuplicateNodeWithUndo(const std::shared_ptr<SceneNode>& node)
{
	if (!node || m_core.IsPlaying()) return;
	if (auto copy = m_core.DuplicateNode(node)) {
		nox::editor::RecordCreatedNode(m_nodeTable, CoreNodeOps(), UndoSink(), "Duplicate " + node->GetName(), copy);
		SelectNode(copy);
	}
}

void EditorWorkspace::DeleteNodeWithUndo(const std::shared_ptr<SceneNode>& node)
{
	if (!node || m_core.IsPlaying()) return;
	if (nox::editor::DeleteNodeUndoable(m_nodeTable, CoreNodeOps(), UndoSink(), node)) NoteEdit();
	SelectNode(nullptr);
}

void EditorWorkspace::ReparentWithUndo(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<SceneNode>& parent, int index)
{
	if (!node || !parent || node == parent || m_core.IsPlaying()) return;
	if (index < 0 && node->parentNode.lock() == parent) return;
	if (nox::editor::ReparentNodeUndoable(m_nodeTable, CoreNodeOps(), UndoSink(), node, parent, index)) NoteEdit();
	else ui::Toast("A node cannot be moved under itself", ui::ToastKind::Warning);
}

void EditorWorkspace::MoveNodeNextTo(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<SceneNode>& target, bool after)
{
	if (!node || !target || node == target) return;
	const std::shared_ptr<SceneNode> parent = target->parentNode.lock();
	if (!parent) return;
	// Index among the parent's children without the moved node (that is how ReparentNode counts).
	int index = 0;
	for (const auto& c : parent->children) {
		if (c == node) continue;
		if (c == target) break;
		++index;
	}
	ReparentWithUndo(node, parent, after ? index + 1 : index);
}

void EditorWorkspace::CreateFolderUnder(const std::shared_ptr<SceneNode>& parent)
{
	if (m_core.IsPlaying()) return;
	const std::shared_ptr<SceneNode> target = parent ? parent : RootNode();
	if (!target) return;
	const json folderJson = { {"type", "folder"}, {"name", UniqueChildName(target, "Folder")} };
	if (auto folder = nox::editor::CreateNodeUndoable(m_nodeTable, CoreNodeOps(), UndoSink(), "New Folder", folderJson, target)) {
		SelectNode(folder);
		NoteEdit();
	}
}

void EditorWorkspace::GroupIntoFolderWithUndo(const std::shared_ptr<SceneNode>& node)
{
	if (!node || m_core.IsPlaying() || node->IsGenerated()) return;
	const std::shared_ptr<SceneNode> parent = node->parentNode.lock();
	if (!parent) return;
	const json folderJson = { {"type", "folder"}, {"name", UniqueChildName(parent, "Folder")} };
	if (auto folder = nox::editor::GroupIntoFolderUndoable(m_nodeTable, CoreNodeOps(), UndoSink(), node, folderJson)) {
		SelectNode(folder);
		NoteEdit();
	}
}

void EditorWorkspace::BeginRename(const std::shared_ptr<SceneNode>& node)
{
	if (!node || m_core.IsPlaying()) return;
	m_renameTarget = node;
	std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", node->GetName().c_str());
	m_openRename = true;
}

void EditorWorkspace::BeginSavePrefab(const std::shared_ptr<SceneNode>& node)
{
	if (!node || m_core.IsPlaying()) return;
	m_prefabTarget = node;
	std::string name;
	for (char c : node->GetName()) name += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
	if (name.empty()) name = "node";
	std::snprintf(m_prefabPathBuf, sizeof(m_prefabPathBuf), "prefabs/%s%s", name.c_str(), nox::prefab::kExtension);
	m_openSavePrefab = true;
}

void EditorWorkspace::InstantiatePrefabFromAsset(const std::string& path, const std::shared_ptr<SceneNode>& parent, const glm::vec3* at)
{
	if (m_core.IsPlaying()) return;
	const glm::vec3 position = PlacementPosition(m_core, parent, at);
	nlohmann::json placement = { { "position", { position.x, position.y, position.z } } };
	std::string error;
	auto node = m_core.InstantiatePrefabNode(path, parent, placement, &error);
	if (!node) {
		ui::Toast(error.empty() ? std::string("Prefab could not be instantiated") : error, ui::ToastKind::Warning);
		return;
	}
	nox::editor::RecordCreatedNode(m_nodeTable, CoreNodeOps(), UndoSink(), "Add " + fs::path(path).stem().stem().string(), node);
	SelectNode(node);
	NoteEdit();
}

void EditorWorkspace::UnpackPrefabWithUndo(const std::shared_ptr<SceneNode>& node)
{
	if (!node || m_core.IsPlaying()) return;
	if (nox::editor::UnpackPrefabUndoable(m_nodeTable, UndoSink(), node)) NoteEdit();
}

void EditorWorkspace::DrawAssets()
{
	// Content Browser (ContentBrowser.h): folder tree, tiles, search, preview, file actions and drag sources.
	m_contentBrowser->Draw(kWinAssets, &m_showAssets);
}

void EditorWorkspace::DrawInspector()
{
	if (!ImGui::Begin(kWinInspector, &m_showInspector)) { ImGui::End(); return; }
	if (m_core.IsPlaying()) DrawPlayBanner();
	auto node = m_legacy.GetSelectedNode();
	if (!node) {
		ImGui::TextDisabled("Select a node in the Hierarchy or the Scene view.");
	}
	else {
		if (m_selection.Size() > 1) {
			ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.36f, 1.0f), "%d selected. Showing %s; edits apply to this node only.", static_cast<int>(m_selection.Size()), node->GetName().c_str());
		}
		if (!node->GetPrefabPath().empty()) {
			ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Prefab: %s", node->GetPrefabPath().c_str());
			ImGui::SameLine();
			if (ui::Btn("Unpack", false) && !m_core.IsPlaying()) UnpackPrefabWithUndo(node);
		}
		const std::vector<std::string> nodeNames = FlatNodeNames();

		if (node->IsFolder()) ImGui::TextDisabled("Folder: groups nodes. No transform; not rendered; children keep their own transforms.");
		// Builtin facets (Node, Transform) first; the registry sorts the rest by their order.
		std::vector<const nox::reflect::ComponentDesc*> components = nox::reflect::Registry::Get().ComponentsOf(*node);
		std::stable_partition(components.begin(), components.end(),
			[](const nox::reflect::ComponentDesc* d) { return d->builtin; });
		for (const nox::reflect::ComponentDesc* desc : components) {
			DrawComponentSection(node, *desc, m_contentBrowser->AssetPaths(), nodeNames);
		}
		DrawAddComponentButton(node);

		// Info footer: summaries that the registry does not own (models are the MeshRenderer component).
		if (ui::Section("Info", false)) {
			ImGui::Text("Entity %u", node->GetEntityID());
			ImGui::Text("Children %d", static_cast<int>(node->children.size()));
			const glm::vec3 w = node->GetWorldPosition();
			ImGui::Text("World (%.2f, %.2f, %.2f)", w.x, w.y, w.z);
			if (node->IsGenerated()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "Generated: rebuilt on load, not saved");
		}
	}
	ImGui::End();
	// Safety net: a continuous edit whose widget is gone ends once no item is active.
	if (m_propEdit.active && !ImGui::IsAnyItemActive()) EndPropertyEdit(m_propEdit, m_nodeTable, UndoSink());
	if (m_multiActive && !ImGui::IsAnyItemActive()) EndMultiEdit();
}

// Node names for NodeRef pickers (flattened scene graph, root excluded).
std::vector<std::string> EditorWorkspace::FlatNodeNames()
{
	std::vector<std::string> nodeNames;
	std::function<void(const std::shared_ptr<SceneNode>&)> collect = [&](const std::shared_ptr<SceneNode>& n) {
		for (const auto& c : n->children) {
			nodeNames.push_back(c->GetName());
			collect(c);
		}
	};
	if (auto root = RootNode()) collect(root);
	return nodeNames;
}

PhysicsWindow::Host EditorWorkspace::PhysicsHost()
{
	PhysicsWindow::Host host;
	host.physics = m_core.GetPhysicsEngine();
	host.selected = m_legacy.GetSelectedNode();
	host.playing = m_core.IsPlaying();
	host.paused = m_core.IsPaused();
	host.undo = UndoSink();
	if (!host.playing) host.markDirty = [this] { m_core.MarkDirty(); };
	host.drawComponent = [this](const std::shared_ptr<SceneNode>& node, const nox::reflect::ComponentDesc& desc) {
		DrawComponentSection(node, desc, m_contentBrowser->AssetPaths(), FlatNodeNames());
	};
	return host;
}

void EditorWorkspace::DrawPhysics()
{
	m_physicsDrawn = false;
	if (!ImGui::Begin(kWinPhysics, &m_showPhysics)) { ImGui::End(); return; }
	m_physicsDrawn = true;
	m_physicsWindow.Draw(PhysicsHost());
	ImGui::End();
	// Safety net: a continuous edit whose widget is gone ends once no item is active.
	if (m_propEdit.active && !ImGui::IsAnyItemActive()) EndPropertyEdit(m_propEdit, m_nodeTable, UndoSink());
	if (m_multiActive && !ImGui::IsAnyItemActive()) EndMultiEdit();
}

void EditorWorkspace::DrawPlayBanner()
{
	const ImVec4 colour = m_core.IsPaused() ? ImVec4(0.62f, 0.45f, 0.10f, 1.0f) : ImVec4(0.62f, 0.16f, 0.12f, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, colour);
	ImGui::BeginChild("##playbanner", ImVec2(-1.0f, ImGui::GetFrameHeight() + 8.0f), false, ImGuiWindowFlags_NoScrollbar);
	ImGui::SetCursorPos(ImVec2(8.0f, 4.0f));
	ImGui::TextColored(ImVec4(1, 1, 1, 1), "Play mode: changes will be lost when you Stop (no undo here)");
	ImGui::EndChild();
	ImGui::PopStyleColor();
	ImGui::Spacing();
}

void EditorWorkspace::DrawComponentSection(const std::shared_ptr<SceneNode>& node, const nox::reflect::ComponentDesc& desc,
	const std::vector<std::string>& assets, const std::vector<std::string>& nodeNames)
{
	using nox::reflect::MethodDesc;
	using nox::reflect::PropertyDesc;
	using nox::reflect::PropValue;
	const bool open = ui::Section(desc.name.c_str(), desc.builtin);
	const bool removable = !desc.builtin && desc.remove != nullptr;
	bool removed = false;
	const std::string ctxId = "##ctx_" + desc.name;
	if (ImGui::BeginPopupContextItem(ctxId.c_str())) {
		if (ImGui::MenuItem("Remove Component", nullptr, false, removable)) {
			removed = nox::editor::RemoveComponentUndoable(m_nodeTable, UndoSink(), node, desc);
			if (removed) NoteEdit();
		}
		ImGui::EndPopup();
	}
	if (removed || !open || !desc.has || !desc.has(*node)) return;

	ui::Grid(("##grid_" + desc.name).c_str());
	for (const PropertyDesc& prop : desc.properties) {
		if (prop.flags & nox::reflect::PropHidden) continue;
		if (!prop.AppliesTo(*node)) continue;
		PropValue value;
		if (!nox::reflect::GetProperty(*node, desc.name, prop.name, value)) continue;
		// Multi-edit: the other selected nodes that have this property take the same edit.
		std::vector<std::shared_ptr<SceneNode>> targets{ node };
		for (const auto& other : m_selection.Nodes()) {
			PropValue ignored;
			if (other && other != node && nox::reflect::GetProperty(*other, desc.name, prop.name, ignored) && prop.AppliesTo(*other)) targets.push_back(other);
		}
		const bool multi = targets.size() > 1;
		bool mixed = false;
		for (size_t i = 1; i < targets.size(); ++i) {
			PropValue other;
			if (nox::reflect::GetProperty(*targets[i], desc.name, prop.name, other) && !(other == value)) mixed = true;
		}

		const std::string label = mixed ? std::string(prop.label) + "  (mixed)" : prop.label;
		solace::PropertyLabel(label.c_str(), prop.tooltip.empty() ? nullptr : prop.tooltip.c_str());
		const std::string widgetId = desc.name + "." + prop.name;
		const bool multiOwned = m_multiActive && m_multiComponent == desc.name && m_multiProperty == prop.name;
		const bool ownsEdit = !multi && m_propEdit.active && m_propEdit.component == desc.name && m_propEdit.property == prop.name
			&& m_nodeTable.Resolve(m_propEdit.node) == node;
		const PropValue before = value;   // the primary's value before this frame's widget edit
		const nox::editor::PropertyWidgetResult r = nox::editor::DrawPropertyWidget(prop, value, assets, nodeNames, widgetId.c_str());
		if (r.changed && multi) {
			ApplyMultiEdit(targets, desc.name, prop.name, before, value, !multiOwned && r.final);
			NoteEdit();
		}
		else if (r.changed) {
			std::string error;
			bool ok;
			if (!ownsEdit && r.final) {
				// Discrete change (checkbox, combo, Enter in a text field): one step now.
				ok = nox::editor::SetPropertyUndoable(m_nodeTable, UndoSink(), node, desc.name, prop.name, value, &error);
			}
			else {
				// Drag, slider or colour: the first write latches the old value; the step ends below.
				ok = nox::editor::SetPropertyContinuous(m_propEdit, m_nodeTable, node, desc.name, prop.name, value, &error);
			}
			if (!ok) ui::Toast(error.empty() ? "Value rejected" : error, ui::ToastKind::Warning);
			NoteEdit();
		}
		const bool stillOwned = m_propEdit.active && m_propEdit.component == desc.name && m_propEdit.property == prop.name
			&& m_nodeTable.Resolve(m_propEdit.node) == node;
		if (stillOwned && (r.final || !r.active)) EndPropertyEdit(m_propEdit, m_nodeTable, UndoSink());
		if (multiOwned && (r.final || !r.active)) EndMultiEdit();
		// A Lua script reference (asset path ending in .lua) gets a button to open it in the external editor.
		const std::string* assetPath = prop.type == nox::reflect::PropType::AssetPath ? std::get_if<std::string>(&value) : nullptr;
		if (assetPath && assetPath->size() > 4 && assetPath->compare(assetPath->size() - 4, 4, ".lua") == 0) {
			ImGui::PushID(widgetId.c_str());
			if (ImGui::SmallButton("Open in external editor")) OpenAssetExternally(*assetPath, 0);
			ImGui::PopID();
		}
	}
	// Methods flagged editorButton and taking no arguments.
	for (const MethodDesc& method : desc.methods) {
		if (!method.editorButton || !method.args.empty()) continue;
		solace::PropertyLabel(method.name.c_str(), method.tooltip.empty() ? nullptr : method.tooltip.c_str());
		ImGui::PushID((desc.name + "." + method.name).c_str());
		if (ImGui::Button("Run")) InvokeEditorButton(node, desc, method);
		ImGui::PopID();
	}
	ui::EndGrid();
}

void EditorWorkspace::InvokeEditorButton(const std::shared_ptr<SceneNode>& node, const nox::reflect::ComponentDesc& desc,
	const nox::reflect::MethodDesc& method)
{
	nox::reflect::PropValue result;
	std::string error;
	if (!nox::reflect::InvokeMethod(*node, desc.name, method.name, {}, result, &error)) {
		ui::Toast(error.empty() ? "Action failed" : error, ui::ToastKind::Warning);
		return;
	}
	NoteEdit();
}

void EditorWorkspace::DrawAddComponentButton(const std::shared_ptr<SceneNode>& node)
{
	ImGui::Spacing();
	if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f))) ImGui::OpenPopup("##addcomponent");
	if (!ImGui::BeginPopup("##addcomponent")) return;
	const auto& all = nox::reflect::Registry::Get().All();
	auto addable = [&](const nox::reflect::ComponentDesc& d) { return d.add != nullptr && !(d.has && d.has(*node)); };
	std::set<std::string> categories;
	for (const auto& d : all) if (addable(d)) categories.insert(d.category.empty() ? std::string("Other") : d.category);
	for (const std::string& category : categories) {
		ImGui::TextDisabled("%s", category.c_str());
		for (const auto& d : all) {
			const std::string dCategory = d.category.empty() ? std::string("Other") : d.category;
			if (dCategory != category || !addable(d)) continue;
			if (ImGui::MenuItem(d.name.c_str())) {
				if (nox::editor::AddComponentUndoable(m_nodeTable, UndoSink(), node, d)) NoteEdit();
			}
			if (ImGui::IsItemHovered() && !d.tooltip.empty()) ImGui::SetTooltip("%s", d.tooltip.c_str());
		}
		ImGui::Separator();
	}
	if (categories.empty()) ImGui::TextDisabled("Every component is already present.");
	ImGui::EndPopup();
}

namespace {

// Material state of one primitive: what the Materials window edits, and what undo steps restore.
struct PrimitiveMaterialState {
	MaterialDesc desc;
	std::shared_ptr<Texture> textures[MaterialDesc::SlotCount];
	std::array<std::string, MaterialDesc::SlotCount> paths;
};

PrimitiveMaterialState CapturePrimitiveMaterial(MeshComponent& mesh)
{
	PrimitiveMaterialState state;
	state.desc = mesh.GetMaterialDesc();
	for (int slot = 0; slot < MaterialDesc::SlotCount; ++slot) {
		state.textures[slot] = mesh.TextureForSlot(slot);
		state.paths[slot] = mesh.textureSourcePaths[slot];
	}
	return state;
}

void ApplyPrimitiveMaterial(MeshComponent& mesh, const PrimitiveMaterialState& state)
{
	mesh.SetMaterialDesc(state.desc);
	for (int slot = 0; slot < MaterialDesc::SlotCount; ++slot) {
		mesh.TextureForSlot(slot) = state.textures[slot];
		mesh.textureSourcePaths[slot] = state.paths[slot];
	}
}

PrimitiveMaterialState StateOfImported(const Scene::ImportedMaterial& imported)
{
	PrimitiveMaterialState state;
	state.desc = imported.material;
	for (int slot = 0; slot < MaterialDesc::SlotCount; ++slot) {
		state.textures[slot] = imported.textures[slot];
		state.paths[slot] = imported.texturePaths[slot];
	}
	return state;
}

// What a texture slot shows: the assigned file, or "(imported)" / "(none)".
std::string TextureSlotName(const PrimitiveMaterialState& state, const PrimitiveMaterialState& imported, int slot)
{
	if (!state.textures[slot]) return "(none)";
	if (!state.paths[slot].empty()) return state.paths[slot];
	return state.textures[slot] == imported.textures[slot] ? "(imported)" : "(changed)";
}

const char* AlphaModeKey(MaterialDesc::AlphaMode mode)
{
	switch (mode) {
	case MaterialDesc::AlphaMode::Mask: return "mask";
	case MaterialDesc::AlphaMode::Blend: return "blend";
	default: return "opaque";
	}
}

// Override entry for one primitive. Keys the editor owns are rewritten from the state; keys it does not
// know (disable_textures, hand-written extras) are kept from the previous entry.
json BuildOverrideEntry(const json& previous, const std::string& matName, int slot,
	const PrimitiveMaterialState& state, const PrimitiveMaterialState& imported)
{
	const MaterialDesc& d = state.desc;
	json entry = previous.is_object() ? previous : json::object();
	entry["match"] = matName;
	entry["slot"] = slot;
	entry["base_color"] = { d.baseColorFactor.r, d.baseColorFactor.g, d.baseColorFactor.b, d.baseColorFactor.a };
	entry["metallic"] = d.metallicFactor;
	entry["roughness"] = d.roughnessFactor;
	entry["normal_scale"] = d.normalScale;
	entry["occlusion_strength"] = d.occlusionStrength;
	entry["emissive"] = { d.emissiveFactor.r, d.emissiveFactor.g, d.emissiveFactor.b };
	entry["emissive_strength"] = d.emissiveStrength;
	entry["specular"] = d.specularFactor;
	entry["specular_color"] = { d.specularColorFactor.r, d.specularColorFactor.g, d.specularColorFactor.b };
	entry["ior"] = d.ior;
	entry["clearcoat"] = d.clearcoatFactor;
	entry["clearcoat_roughness"] = d.clearcoatRoughnessFactor;
	entry["transmission"] = d.transmissionFactor;
	entry["thickness"] = d.thicknessFactor;
	entry["attenuation_color"] = { d.attenuationColor.r, d.attenuationColor.g, d.attenuationColor.b };
	if (std::isfinite(d.attenuationDistance)) entry["attenuation_distance"] = d.attenuationDistance;
	else entry.erase("attenuation_distance");
	entry["alpha_mode"] = AlphaModeKey(d.alphaMode);
	entry["alpha_cutoff"] = d.alphaCutoff;
	entry["double_sided"] = d.doubleSided;
	entry["unlit"] = d.unlit;
	entry["sheen_color"] = { d.sheenColorFactor.r, d.sheenColorFactor.g, d.sheenColorFactor.b };
	entry["sheen_roughness"] = d.sheenRoughnessFactor;
	entry["anisotropy_strength"] = d.anisotropyStrength;
	entry["anisotropy_rotation"] = d.anisotropyRotation;
	entry["iridescence"] = d.iridescenceFactor;
	entry["iridescence_ior"] = d.iridescenceIor;
	entry["iridescence_thickness_min"] = d.iridescenceThicknessMin;
	entry["iridescence_thickness_max"] = d.iridescenceThicknessMax;
	entry["dispersion"] = d.dispersion;
	entry["wetness_response"] = d.wetnessResponse;
	entry["puddle_response"] = d.puddleResponse;
	entry["rain_bead_response"] = d.rainBeadResponse;
	entry["flake_intensity"] = d.flakeIntensity;
	entry["flake_density"] = d.flakeDensity;
	entry["interior_mapping"] = d.interiorMapping;
	entry["interior_depth"] = d.interiorDepth;
	entry["interior_strength"] = d.interiorStrength;
	entry["interior_seed"] = d.interiorSeed;
	entry["interior_lighting"] = d.interiorLighting;
	entry["interior_blinds"] = d.interiorBlinds;
	entry["interior_style"] = d.interiorStyle;

	// Texture slots that differ from the import: a file path, or null for a cleared slot.
	json textures = json::object();
	for (int slot = 0; slot < MaterialDesc::SlotCount; ++slot) {
		if (state.textures[slot] == imported.textures[slot] && state.paths[slot] == imported.paths[slot]) continue;
		const char* key = MaterialDesc::TextureSlotKey(slot);
		if (!state.textures[slot]) textures[key] = nullptr;
		else if (!state.paths[slot].empty()) textures[key] = state.paths[slot];
	}
	if (textures.empty()) entry.erase("textures");
	else entry["textures"] = textures;

	// UV transforms that are not identity.
	json transforms = json::object();
	for (int slot = 0; slot < MaterialDesc::SlotCount; ++slot) {
		const TextureTransform& xf = d.textureTransforms[slot];
		const int texCoord = d.TexCoordOf(slot);
		if (xf.IsIdentity() && texCoord == 0) continue;
		transforms[MaterialDesc::TextureSlotKey(slot)] = {
			{ "offset", { xf.offset.x, xf.offset.y } },
			{ "scale", { xf.scale.x, xf.scale.y } },
			{ "rotation_degrees", glm::degrees(xf.rotation) },
			{ "texcoord", texCoord } };
	}
	if (transforms.empty()) entry.erase("texture_transforms");
	else entry["texture_transforms"] = transforms;
	return entry;
}

// Index of the override entry that names this primitive (match + slot), or npos.
size_t FindPrimitiveOverride(const json& overrides, const std::string& matName, int slot)
{
	for (size_t i = 0; i < overrides.size(); ++i) {
		const json& o = overrides[i];
		if (o.is_object() && o.value("match", std::string()) == matName && o.contains("slot") &&
			o["slot"].is_number_integer() && o["slot"].get<int64_t>() == slot) {
			return i;
		}
	}
	return std::string::npos;
}

// Texture files the Materials window offers for assignment (textures/ folder, image extensions only).
std::vector<std::string> ListTextureAssets()
{
	AssetIO::ListOptions options;
	options.recursive = true;
	std::vector<std::string> out;
	for (std::string& path : AssetIO::List("textures", options)) {
		std::string ext = fs::path(path).extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") out.push_back(std::move(path));
	}
	std::sort(out.begin(), out.end());
	if (out.size() > 4000) out.resize(4000);
	return out;
}

} // namespace

void EditorWorkspace::DrawMaterials()
{
	// The window edits one primitive at a time. A continuous edit (drag, slider, text) becomes one undo step
	// when the widgets release; the before-state is captured at the start of each idle frame.
	static PrimitiveMaterialState s_before;
	static bool s_editPending = false;
	static nox::editor::NodeHandle s_editNode;
	static int s_editSlot = 0;
	static int s_slot = 0;
	static std::vector<std::string> s_textureAssets;
	static float s_textureAssetsTimer = 0.0f;
	static char s_texturePickFilter[128] = "";

	if (!ImGui::Begin(kWinMaterials, &m_showMaterials)) { ImGui::End(); return; }
	auto node = m_legacy.GetSelectedNode();
	std::shared_ptr<Scene> model = node ? node->GetModel() : nullptr;
	if (!model || model->meshes.empty()) {
		ImGui::TextDisabled(model ? "This model has no primitives." : "Select a model node to edit its materials.");
		ImGui::End();
		return;
	}

	// Every primitive gets a slot: the glTF material name, per primitive, so shared names stay separate.
	const int count = static_cast<int>(model->meshes.size());
	s_slot = std::clamp(s_slot, 0, count - 1);
	ImGui::TextDisabled("%d primitive slot%s", count, count == 1 ? "" : "s");
	const float listHeight = std::min(count, 6) * ImGui::GetTextLineHeightWithSpacing() + 6.0f;
	if (ImGui::BeginListBox("##primitives", ImVec2(-1.0f, listHeight))) {
		for (int i = 0; i < count; ++i) {
			const std::string& name = model->meshes[i].materialName;
			const std::string label = "[" + std::to_string(i) + "] " + (name.empty() ? std::string("(unnamed)") : name) + "##prim" + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), i == s_slot)) s_slot = i;
		}
		ImGui::EndListBox();
	}

	MeshComponent& mesh = model->meshes[s_slot];
	const std::string matName = mesh.materialName;
	const bool hasImported = s_slot < static_cast<int>(model->importedMaterials.size());
	const PrimitiveMaterialState imported = hasImported
		? StateOfImported(model->importedMaterials[s_slot]) : CapturePrimitiveMaterial(mesh);
	if (!s_editPending) {
		s_before = CapturePrimitiveMaterial(mesh);
		s_editSlot = s_slot;
		s_editNode = m_nodeTable.HandleOf(node);
	}

	MaterialDesc d = mesh.GetMaterialDesc();
	bool changed = false;
	ImGui::BeginDisabled(!hasImported);
	const bool resetRequested = ui::Btn("Reset to imported", false);
	ImGui::EndDisabled();
	ImGui::SameLine();
	const bool saveRequested = ui::Btn("Save as scene override", true);
	if (!hasImported) ImGui::TextDisabled("Reset is unavailable for primitives without an import snapshot.");

	if (ui::Section("Base", true)) {
		ui::Grid("##mat_base");
		ui::Label("Base color"); changed |= ImGui::ColorEdit4("##bc", &d.baseColorFactor.x);
		ui::Label("Metallic"); changed |= ImGui::SliderFloat("##me", &d.metallicFactor, 0.0f, 1.0f);
		ui::Label("Roughness"); changed |= ImGui::SliderFloat("##ro", &d.roughnessFactor, 0.0f, 1.0f);
		ui::Label("Normal scale"); changed |= ImGui::DragFloat("##ns", &d.normalScale, 0.01f, 0.0f, 4.0f);
		ui::Label("Occlusion strength"); changed |= ImGui::SliderFloat("##os", &d.occlusionStrength, 0.0f, 1.0f);
		ui::Label("Alpha mode");
		int alpha = static_cast<int>(d.alphaMode);
		static const char* const alphaNames[] = { "Opaque", "Mask", "Blend" };
		if (ImGui::Combo("##am", &alpha, alphaNames, 3)) {
			d.alphaMode = static_cast<MaterialDesc::AlphaMode>(alpha);
			changed = true;
		}
		if (d.alphaMode == MaterialDesc::AlphaMode::Mask) {
			ui::Label("Alpha cutoff"); changed |= ImGui::SliderFloat("##ac", &d.alphaCutoff, 0.0f, 1.0f);
		}
		ui::Label("Double-sided"); changed |= ImGui::Checkbox("##ds", &d.doubleSided);
		ui::Label("Unlit (KHR)"); changed |= ImGui::Checkbox("##ul", &d.unlit);
		ui::EndGrid();
	}
	if (ui::Section("Emissive", true)) {
		ui::Grid("##mat_emissive");
		ui::Label("Emissive"); changed |= ImGui::ColorEdit3("##em", &d.emissiveFactor.x);
		ui::Label("Emissive strength"); changed |= ImGui::DragFloat("##ems", &d.emissiveStrength, 0.05f, 0.0f, 100000.0f);
		ui::EndGrid();
	}
	if (ui::Section("Specular, IOR, clearcoat, transmission", false)) {
		ui::Grid("##mat_spec");
		ui::Label("Specular"); changed |= ImGui::SliderFloat("##sp", &d.specularFactor, 0.0f, 1.0f);
		ui::Label("Specular color"); changed |= ImGui::ColorEdit3("##spc", &d.specularColorFactor.x);
		ui::Label("IOR"); changed |= ImGui::DragFloat("##ior", &d.ior, 0.005f, 1.0f, 3.0f);
		ui::Label("Clearcoat"); changed |= ImGui::SliderFloat("##cc", &d.clearcoatFactor, 0.0f, 1.0f);
		ui::Label("Coat roughness"); changed |= ImGui::SliderFloat("##ccr", &d.clearcoatRoughnessFactor, 0.0f, 1.0f);
		ui::Label("Transmission"); changed |= ImGui::SliderFloat("##tr", &d.transmissionFactor, 0.0f, 1.0f);
		ui::Label("Thickness (m)"); changed |= ImGui::DragFloat("##th", &d.thicknessFactor, 0.01f, 0.0f, 100.0f);
		ui::Label("Attenuation color"); changed |= ImGui::ColorEdit3("##att", &d.attenuationColor.x);
		ui::Label("Attenuation dist."); changed |= ImGui::DragFloat("##attd", &d.attenuationDistance, 0.05f, 0.0f, 100000.0f);
		ui::EndGrid();
	}
	if (ui::Section("Sheen, anisotropy, iridescence, dispersion (stored, not shaded yet)", false)) {
		ui::Grid("##mat_ext");
		ui::Label("Sheen color"); changed |= ImGui::ColorEdit3("##shc", &d.sheenColorFactor.x);
		ui::Label("Sheen roughness"); changed |= ImGui::SliderFloat("##shr", &d.sheenRoughnessFactor, 0.0f, 1.0f);
		ui::Label("Anisotropy"); changed |= ImGui::SliderFloat("##an", &d.anisotropyStrength, 0.0f, 1.0f);
		ui::Label("Anisotropy rot."); changed |= ImGui::SliderFloat("##anr", &d.anisotropyRotation, 0.0f, 6.2831853f);
		ui::Label("Iridescence"); changed |= ImGui::SliderFloat("##irf", &d.iridescenceFactor, 0.0f, 1.0f);
		ui::Label("Iridescence IOR"); changed |= ImGui::DragFloat("##iri", &d.iridescenceIor, 0.01f, 1.0f, 3.0f);
		ui::Label("Thin film min (nm)"); changed |= ImGui::DragFloat("##irtmin", &d.iridescenceThicknessMin, 1.0f, 0.0f, 2000.0f);
		ui::Label("Thin film max (nm)"); changed |= ImGui::DragFloat("##irtmax", &d.iridescenceThicknessMax, 1.0f, 0.0f, 2000.0f);
		ui::Label("Dispersion"); changed |= ImGui::DragFloat("##disp", &d.dispersion, 0.005f, 0.0f, 1.0f);
		ui::EndGrid();
	}
	if (ui::Section("NOX surface and interior", false)) {
		ui::Grid("##mat_nox");
		ui::Label("Wetness"); changed |= ImGui::SliderFloat("##wet", &d.wetnessResponse, 0.0f, 1.0f);
		ui::Label("Puddles"); changed |= ImGui::SliderFloat("##pud", &d.puddleResponse, 0.0f, 1.0f);
		ui::Label("Rain beads"); changed |= ImGui::SliderFloat("##rb", &d.rainBeadResponse, 0.0f, 1.0f);
		ui::Label("Flake intensity"); changed |= ImGui::SliderFloat("##fli", &d.flakeIntensity, 0.0f, 1.0f);
		ui::Label("Flake density"); changed |= ImGui::DragFloat("##fld", &d.flakeDensity, 10.0f, 10.0f, 100000.0f);
		ui::Label("Interior mapping"); changed |= ImGui::SliderFloat("##imap", &d.interiorMapping, 0.0f, 2.0f, "%.0f");
		ui::Label("Interior depth (m)"); changed |= ImGui::DragFloat("##idep", &d.interiorDepth, 0.05f, 1.0f, 20.0f);
		ui::Label("Interior strength"); changed |= ImGui::DragFloat("##istr", &d.interiorStrength, 0.01f, 0.0f, 10.0f);
		ui::Label("Interior lighting"); changed |= ImGui::SliderFloat("##ilit", &d.interiorLighting, 0.0f, 1.0f);
		ui::Label("Interior blinds"); changed |= ImGui::SliderFloat("##ibld", &d.interiorBlinds, 0.0f, 1.0f);
		ui::Label("Interior style"); changed |= ImGui::DragFloat("##isty", &d.interiorStyle, 0.1f, 0.0f, 16.0f);
		ui::Label("Interior seed"); changed |= ImGui::DragFloat("##iseed", &d.interiorSeed, 0.1f, 0.0f, 1000.0f);
		ui::EndGrid();
	}

	// Texture slots: assignment from the textures folder, clear / revert, and the KHR_texture_transform fields.
	if (ui::Section("Textures and UV transforms", true)) {
		s_textureAssetsTimer -= m_dt;
		if (s_textureAssetsTimer <= 0.0f) {
			s_textureAssetsTimer = 5.0f;
			s_textureAssets = ListTextureAssets();
		}
		const PrimitiveMaterialState current = CapturePrimitiveMaterial(mesh);
		for (int slot = 0; slot < MaterialDesc::SlotCount; ++slot) {
			ImGui::PushID(slot);
			const std::string title = std::string(MaterialDesc::TextureSlotLabel(slot)) + ":  " + TextureSlotName(current, imported, slot);
			const std::string popupId = "##texpick" + std::to_string(slot);
			if (ImGui::TreeNodeEx(("##texslot" + std::to_string(slot)).c_str(), 0, "%s", title.c_str())) {
				if (ui::Btn("Pick...", false)) {
					ImGui::OpenPopup(popupId.c_str());
				}
				if (ImGui::BeginPopup(popupId.c_str())) {
					ImGui::SetNextItemWidth(300.0f);
					ImGui::InputTextWithHint("##tfilter", "Filter textures", s_texturePickFilter, sizeof(s_texturePickFilter));
					ImGui::BeginChild("##tlist", ImVec2(300.0f, 240.0f));
					for (const std::string& path : s_textureAssets) {
						if (!ContainsNoCase(path, s_texturePickFilter)) continue;
						if (ImGui::Selectable(path.c_str())) {
							if (TexturePtr texture = TextureFactory::FromFile(path, MaterialDesc::TextureSlotIsSRGB(slot))) {
								mesh.TextureForSlot(slot) = texture;
								mesh.textureSourcePaths[slot] = path;
								changed = true;
							}
							else {
								ui::Toast("Could not load " + path, ui::ToastKind::Error);
							}
							ImGui::CloseCurrentPopup();
						}
					}
					ImGui::EndChild();
					ImGui::EndPopup();
				}
				ImGui::SameLine();
				if (ui::Btn("Clear", false)) {
					mesh.TextureForSlot(slot).reset();
					mesh.textureSourcePaths[slot].clear();
					changed = true;
				}
				if (current.textures[slot] != imported.textures[slot] || current.paths[slot] != imported.paths[slot]) {
					ImGui::SameLine();
					if (ui::Btn("Revert", false)) {
						mesh.TextureForSlot(slot) = imported.textures[slot];
						mesh.textureSourcePaths[slot] = imported.paths[slot];
						changed = true;
					}
				}

				// KHR_texture_transform: uv' = T * R * S * uv on this slot, plus the TEXCOORD set it samples.
				int texCoord = d.TexCoordOf(slot);
				ImGui::SetNextItemWidth(160.0f);
				if (ImGui::Combo("UV set##uvset", &texCoord, "TEXCOORD_0\0TEXCOORD_1\0")) {
					d.TexCoordOf(slot) = texCoord;
					changed = true;
				}
				TextureTransform& xf = d.textureTransforms[slot];
				float rotationDeg = glm::degrees(xf.rotation);
				ImGui::SetNextItemWidth(200.0f);
				changed |= ImGui::DragFloat2("Offset##uvo", &xf.offset.x, 0.01f);
				ImGui::SetNextItemWidth(200.0f);
				changed |= ImGui::DragFloat2("Scale##uvs", &xf.scale.x, 0.01f, -1000.0f, 1000.0f);
				ImGui::SetNextItemWidth(200.0f);
				if (ImGui::DragFloat("Rotation (deg)##uvr", &rotationDeg, 0.5f, -360.0f, 360.0f)) {
					xf.rotation = glm::radians(rotationDeg);
					changed = true;
				}
				ImGui::SameLine();
				if (ui::Btn("Reset UV", false)) {
					xf = TextureTransform();
					d.TexCoordOf(slot) = 0;
					changed = true;
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}

	// Applying: the widgets already changed d / the textures; push the new state live.
	if (resetRequested && hasImported) {
		const PrimitiveMaterialState before = CapturePrimitiveMaterial(mesh);
		ApplyPrimitiveMaterial(mesh, imported);
		if (nox::editor::UndoPush push = UndoSink()) {
			const nox::editor::NodeHandle handle = m_nodeTable.HandleOf(node);
			const int slotIndex = s_slot;
			auto apply = [this, handle, slotIndex](const PrimitiveMaterialState& st) {
				auto n = m_nodeTable.Resolve(handle);
				auto mdl = n ? n->GetModel() : nullptr;
				if (!mdl || slotIndex >= static_cast<int>(mdl->meshes.size())) return;
				ApplyPrimitiveMaterial(mdl->meshes[slotIndex], st);
				m_core.MarkDirty();
			};
			push("Reset material to imported", [apply, before] { apply(before); }, [apply, imported] { apply(imported); });
		}
		// The saved override for this primitive goes too, so a reload does not bring it back.
		json extras = node->GetAuthoredExtras().empty() ? json::object() : json::parse(node->GetAuthoredExtras(), nullptr, false);
		if (extras.is_object() && extras.contains("material_overrides") && extras["material_overrides"].is_array()) {
			json& overrides = extras["material_overrides"];
			const size_t at = FindPrimitiveOverride(overrides, matName, s_slot);
			if (at != std::string::npos) {
				overrides.erase(overrides.begin() + static_cast<std::ptrdiff_t>(at));
				node->SetAuthoredExtras(extras.dump());
			}
		}
		s_editPending = false;
		m_core.MarkDirty();
		ui::Toast("Primitive reset to the imported material", ui::ToastKind::Info);
	}
	else {
		if (changed) {
			mesh.SetMaterialDesc(d);
			m_core.MarkDirty();
			s_editPending = true;
		}
		if (s_editPending && !ImGui::IsAnyItemActive()) {
			const PrimitiveMaterialState after = CapturePrimitiveMaterial(model->meshes[s_editSlot]);
			if (nox::editor::UndoPush push = UndoSink()) {
				const nox::editor::NodeHandle handle = s_editNode;
				const int slotIndex = s_editSlot;
				auto apply = [this, handle, slotIndex](const PrimitiveMaterialState& st) {
					auto n = m_nodeTable.Resolve(handle);
					auto mdl = n ? n->GetModel() : nullptr;
					if (!mdl || slotIndex >= static_cast<int>(mdl->meshes.size())) return;
					ApplyPrimitiveMaterial(mdl->meshes[slotIndex], st);
					m_core.MarkDirty();
				};
				push("Edit material", [apply, before = s_before] { apply(before); }, [apply, after] { apply(after); });
			}
			s_editPending = false;
		}
	}

	if (saveRequested) {
		// Persist as one entry per primitive: match (glTF material name) + slot (primitive index).
		json extras = node->GetAuthoredExtras().empty() ? json::object() : json::parse(node->GetAuthoredExtras(), nullptr, false);
		if (!extras.is_object()) extras = json::object();
		json overrides = extras.value("material_overrides", json::array());
		if (!overrides.is_array()) overrides = json::array();
		const PrimitiveMaterialState current = CapturePrimitiveMaterial(mesh);
		const size_t at = FindPrimitiveOverride(overrides, matName, s_slot);
		const json previous = at == std::string::npos ? json::object() : overrides[at];
		const json entry = BuildOverrideEntry(previous, matName, s_slot, current, hasImported ? imported : current);
		if (at == std::string::npos) overrides.push_back(entry);
		else overrides[at] = entry;
		extras["material_overrides"] = overrides;
		node->SetAuthoredExtras(extras.dump());
		m_core.MarkDirty();
		ui::Toast("Material override stored for primitive " + std::to_string(s_slot) + " - save the scene to keep it", ui::ToastKind::Info);
	}
	ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Console / build output
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::DrawConsole()
{
	size_t total = 0;
	for (const ConsoleLog::Line& l : ConsoleLog::Get().Since(m_consoleCursor, total)) m_consoleLines.push_back({ l.text, l.level });
	m_consoleCursor = total;
	if (m_consoleLines.size() > 8000) m_consoleLines.erase(m_consoleLines.begin(), m_consoleLines.begin() + 2000);
	if (!ImGui::Begin(kWinConsole, &m_showConsole)) { ImGui::End(); return; }
	if (ui::Btn("Clear", false)) m_consoleLines.clear();
	ImGui::SameLine();
	ImGui::Checkbox("Info", &m_consoleShowInfo); ImGui::SameLine();
	ImGui::Checkbox("Warnings", &m_consoleShowWarn); ImGui::SameLine();
	ImGui::Checkbox("Errors", &m_consoleShowError); ImGui::SameLine();
	ImGui::Checkbox("Auto-scroll", &m_consoleAutoScroll); ImGui::SameLine();
	solace::SearchField("##cfilter", m_consoleFilter, sizeof(m_consoleFilter), "Filter");
	ImGui::Separator();
	ImGui::BeginChild("##log", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
	const solace::Palette& pal = solace::GetPalette();
	for (const auto& [text, level] : m_consoleLines) {
		if ((level == 0 && !m_consoleShowInfo) || (level == 1 && !m_consoleShowWarn) || (level == 2 && !m_consoleShowError)) continue;
		if (!ContainsNoCase(text, m_consoleFilter)) continue;
		if (level == 2) ImGui::PushStyleColor(ImGuiCol_Text, pal.error);
		else if (level == 1) ImGui::PushStyleColor(ImGuiCol_Text, pal.warning);
		ImGui::TextUnformatted(text.c_str());
		if (level > 0) ImGui::PopStyleColor();
	}
	if (m_consoleAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
	ImGui::End();
}

void EditorWorkspace::StartTool(const std::string& title, const std::string& script, const std::string& args)
{
	if (m_runner->IsRunning()) {
		ui::Toast("Another build operation is still running", ui::ToastKind::Warning);
		return;
	}
	if (m_core.IsDirty()) ui::Toast("Note: unsaved scene changes are not included", ui::ToastKind::Warning);
	const std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + fs::absolute(script).string() + "\" " + args;
	m_buildLines.clear();
	m_buildCursor = 0;
	if (m_runner->Start(title, cmd, fs::current_path().string())) {
		m_showBuild = true;
		ImGui::SetWindowFocus(kWinBuild);
		ui::Toast(title + " started", ui::ToastKind::Info);
	}
	else {
		ui::Toast(title + " failed to start: " + m_runner->LastError(), ui::ToastKind::Error);
	}
}

void EditorWorkspace::RunStandalone()
{
	// The running editor binary doubles as the standalone game (separate process, own window).
	char exePath[1024] = {};
	std::string exe;
#ifdef _WIN32
	extern std::string NoxExecutablePath();
	exe = NoxExecutablePath();
#endif
	if (exe.empty()) exe = "Nox_Engine.exe";
	if (m_core.IsDirty()) ui::Toast("Standalone uses the saved scene - save to include edits", ui::ToastKind::Warning);
	std::string cmd = "\"" + exe + "\" --preset " + m_selectedPreset + " --game";
	// A scene under projects/ belongs to that project, so run that scene (the preset's scene is an engine scene).
	const std::string scene = m_core.GetCurrentScenePath();
	if (scene.rfind("projects/", 0) == 0) cmd += " --scene \"" + scene + "\"";
	if (ProcessRunner::LaunchDetached(cmd, fs::current_path().string())) ui::Toast("Launched standalone (" + m_selectedPreset + ")", ui::ToastKind::Success);
	else ui::Toast("Could not launch " + exe, ui::ToastKind::Error);
	(void)exePath;
}

void EditorWorkspace::StartPackage()
{
	// A project packages with its own profile (packaging/<name>.json, written by File > New Project). With no
	// project active the default profile (nox-racing) stays, as before.
	std::string args = "-Configuration Release";
	const std::string project = m_core.GetGameProject();
	if (!project.empty()) {
		const fs::path root = ProjectBootstrap::Info().sourceRoot.empty() ? fs::current_path() : ProjectBootstrap::Info().sourceRoot;
		const std::string profile = ProjectTemplates::FindProfileForProject(root, project);
		if (profile.empty()) {
			ui::Toast("The active project has no packaging profile (packaging/<name>.json)", ui::ToastKind::Warning);
			return;
		}
		args += " -Profile " + ProjectTemplates::ProfileNameFor(profile);
	}
	StartTool("Package", "tools/Package.ps1", args);
}

void EditorWorkspace::DrawNewProjectModal()
{
	const fs::path root = ProjectBootstrap::Info().sourceRoot;
	if (m_openNewProject) {
		m_newProjectTemplates = ProjectTemplates::List(root);
		m_newProjectTemplate = 0;
		m_newProjectOverwrite = false;
		m_newProjectError.clear();
		m_newProjectLocation[0] = '\0';
		ImGui::OpenPopup("New Project");
		m_openNewProject = false;
	}
	if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

	ImGui::TextUnformatted("Name");
	ImGui::SetNextItemWidth(320.0f);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputText("##newprojectname", m_newProjectName, sizeof(m_newProjectName));
	const std::string defaultLocation = std::string("projects/") + m_newProjectName;
	ImGui::TextUnformatted("Location");
	ImGui::SetNextItemWidth(320.0f);
	ImGui::InputTextWithHint("##newprojectlocation", defaultLocation.c_str(), m_newProjectLocation, sizeof(m_newProjectLocation));
	ImGui::TextDisabled("A folder directly under projects/ of this checkout. Empty uses %s.", defaultLocation.c_str());

	ImGui::TextUnformatted("Template");
	const bool haveTemplates = !m_newProjectTemplates.empty();
	const char* current = haveTemplates ? m_newProjectTemplates[m_newProjectTemplate].name.c_str() : "(none found)";
	ImGui::SetNextItemWidth(320.0f);
	if (ImGui::BeginCombo("##newprojecttemplate", current)) {
		for (int i = 0; i < static_cast<int>(m_newProjectTemplates.size()); ++i) {
			if (ImGui::Selectable(m_newProjectTemplates[i].name.c_str(), i == m_newProjectTemplate)) m_newProjectTemplate = i;
		}
		ImGui::EndCombo();
	}
	if (haveTemplates) ImGui::TextWrapped("%s", m_newProjectTemplates[m_newProjectTemplate].description.c_str());
	ImGui::Spacing();
	ImGui::Checkbox("Replace files that already exist", &m_newProjectOverwrite);
	if (!m_newProjectError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_newProjectError.c_str());
	ImGui::Spacing();

	if (ui::Btn("Create", true) && haveTemplates) {
		ProjectTemplates::CreateRequest request;
		request.name = m_newProjectName;
		request.templateId = m_newProjectTemplates[m_newProjectTemplate].id;
		request.root = root;
		request.projectDir = m_newProjectLocation;
		request.overwrite = m_newProjectOverwrite;
		ProjectTemplates::CreateResult result;
		std::string error;
		if (ProjectTemplates::Create(request, result, error)) {
			ui::Toast("Created " + result.projectDescriptor, ui::ToastKind::Success);
			ImGui::CloseCurrentPopup();
			// Opening the main scene activates the project, as for any project scene.
			RequestScene(result.mainScene);
		}
		else {
			m_newProjectError = error;
		}
	}
	ImGui::SameLine();
	if (ui::Btn("Cancel", false)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void EditorWorkspace::DrawOpenProjectModal()
{
	if (m_openOpenProject) {
		m_projectFolders.clear();
		std::error_code ec;
		for (const fs::directory_entry& entry : fs::directory_iterator(ProjectBootstrap::Info().sourceRoot / "projects", ec)) {
			if (entry.is_directory(ec) && fs::is_regular_file(entry.path() / "project.json", ec)) {
				m_projectFolders.push_back(entry.path().filename().string());
			}
		}
		std::sort(m_projectFolders.begin(), m_projectFolders.end());
		ImGui::OpenPopup("Open Project");
		m_openOpenProject = false;
	}
	if (!ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	if (m_projectFolders.empty()) ImGui::TextDisabled("No projects under projects/. File > New Project creates one.");
	ImGui::BeginChild("##openprojectlist", ImVec2(360.0f, 240.0f), true);
	for (const std::string& folder : m_projectFolders) {
		if (!ImGui::Selectable(("projects/" + folder).c_str())) continue;
		// The project's main scene: scenes/main.json when present, else its first scene in the list.
		const std::string prefix = "projects/" + folder + "/scenes/";
		std::string target;
		for (const std::string& scene : m_core.GetSceneList()) {
			if (scene.rfind(prefix, 0) != 0) continue;
			if (scene == prefix + "main.json") { target = scene; break; }
			if (target.empty()) target = scene;
		}
		ImGui::CloseCurrentPopup();
		if (target.empty()) ui::Toast("projects/" + folder + " has no scenes", ui::ToastKind::Warning);
		else RequestScene(target);
	}
	ImGui::EndChild();
	if (ui::Btn("Cancel", false)) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void EditorWorkspace::DrawBuildOutput()
{
	if (m_runner) {
		for (std::string& l : m_runner->LinesSince(m_buildCursor)) m_buildLines.push_back(std::move(l));
		m_buildCursor = m_runner->LineCount();
	}
	static bool wasRunning = false;
	const bool running = m_runner && m_runner->IsRunning();
	if (wasRunning && !running) {
		if (m_runner->Succeeded()) ui::Toast(m_runner->Title() + " succeeded", ui::ToastKind::Success);
		else ui::Toast(m_runner->Title() + " failed: " + m_runner->LastError(), ui::ToastKind::Error);
	}
	wasRunning = running;
	if (!ImGui::Begin(kWinBuild, &m_showBuild)) { ImGui::End(); return; }
	if (running) {
		solace::Spinner(14.0f);
		ImGui::SameLine();
		ImGui::Text("%s  %.1fs", m_runner->Title().c_str(), m_runner->ElapsedSeconds());
		ImGui::SameLine();
		ui::Progress(m_runner->Progress(), 200.0f);
		ImGui::SameLine();
		if (ui::Btn("Cancel", false)) m_runner->Cancel();
	}
	else if (!m_runner->Title().empty()) {
		const bool ok = m_runner->Succeeded();
		solace::Badge(ok ? "SUCCEEDED" : "FAILED", ok ? solace::Status::Success : solace::Status::Error);
		ImGui::SameLine();
		ImGui::Text("%s  (%.1fs, exit %d)", m_runner->Title().c_str(), m_runner->ElapsedSeconds(), m_runner->ExitCode());
		if (!ok && !m_runner->LastError().empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, solace::GetPalette().error);
			ImGui::TextWrapped("%s", m_runner->LastError().c_str());
			ImGui::PopStyleColor();
		}
	}
	else {
		ImGui::TextDisabled("Build, Run Standalone and Package output appears here (tools/*.ps1).");
	}
	ImGui::Separator();
	ImGui::BeginChild("##buildlog", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
	for (const std::string& l : m_buildLines) {
		const bool err = ContainsNoCase(l, "error") && !ContainsNoCase(l, "0 error");
		if (err) ImGui::PushStyleColor(ImGuiCol_Text, solace::GetPalette().error);
		ImGui::TextUnformatted(l.c_str());
		if (err) ImGui::PopStyleColor();
	}
	if (running && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
	ImGui::End();
}

void EditorWorkspace::DrawLaunchPresets()
{
	if (!ImGui::Begin(kWinPresets, &m_showPresets)) { ImGui::End(); return; }
	if (!m_presets.contains("presets") || !m_presets["presets"].is_object()) m_presets["presets"] = json::object();
	json& presets = m_presets["presets"];
	ImGui::TextDisabled("config/launch_presets.json - used by Run Standalone, tools/Run.ps1 and Nox_Engine.exe --preset <name>");
	if (ImGui::BeginTable("##presets", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 30);
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Mode");
		ImGui::TableSetupColumn("Scene");
		ImGui::TableSetupColumn("Quality");
		ImGui::TableSetupColumn("Window");
		ImGui::TableSetupColumn("Size");
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30);
		ImGui::TableHeadersRow();
		std::string toErase;
		for (auto it = presets.begin(); it != presets.end(); ++it) {
			json& p = it.value();
			ImGui::PushID(it.key().c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (ImGui::RadioButton("##use", m_selectedPreset == it.key())) m_selectedPreset = it.key();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(it.key().c_str());
			auto combo = [&](const char* id, const char* key, std::initializer_list<const char*> options) {
				std::string v = p.value(key, std::string(*options.begin()));
				ImGui::SetNextItemWidth(-1);
				if (ImGui::BeginCombo(id, v.c_str())) {
					for (const char* o : options) if (ImGui::Selectable(o, v == o)) p[key] = o;
					ImGui::EndCombo();
				}
			};
			ImGui::TableNextColumn(); combo("##mode", "mode", { "game", "editor" });
			ImGui::TableNextColumn();
			{
				std::string scene = p.value("scene", std::string());
				ImGui::SetNextItemWidth(-1);
				if (ImGui::BeginCombo("##scene", scene.c_str())) {
					for (const std::string& s : m_core.GetSceneList()) if (ImGui::Selectable(s.c_str(), s == scene)) p["scene"] = s;
					ImGui::EndCombo();
				}
			}
			ImGui::TableNextColumn(); combo("##q", "quality", { "high", "ultra" });
			ImGui::TableNextColumn(); combo("##w", "window", { "windowed", "borderless", "fullscreen" });
			ImGui::TableNextColumn();
			{
				int wh[2] = { p.value("width", 1920), p.value("height", 1080) };
				ImGui::SetNextItemWidth(-1);
				if (ImGui::DragInt2("##wh", wh, 8.0f, 320, 7680)) { p["width"] = wh[0]; p["height"] = wh[1]; }
			}
			ImGui::TableNextColumn();
			if (ImGui::SmallButton("x")) toErase = it.key();
			ImGui::PopID();
		}
		ImGui::EndTable();
		if (!toErase.empty()) presets.erase(toErase);
	}
	static char newName[64] = "";
	ImGui::SetNextItemWidth(160);
	ImGui::InputTextWithHint("##newp", "new preset name", newName, sizeof(newName));
	ImGui::SameLine();
	if (ui::Btn("Add", false) && newName[0]) {
		presets[newName] = { {"mode", "game"}, {"scene", m_core.GetCurrentScenePath()}, {"quality", "high"}, {"window", "windowed"}, {"width", 1720}, {"height", 720} };
		newName[0] = 0;
	}
	ImGui::SameLine();
	if (ui::Btn("Save presets", true)) {
		m_presets["default"] = m_presets.value("default", std::string("editor"));
		// Launch presets are authored content: written to the loose source tree only.
		const std::optional<fs::path> presetsTarget = AssetIO::SourcePath(kPresetsPath);
		if (!presetsTarget) {
			ui::Toast("Launch presets are read-only in this build", ui::ToastKind::Warning);
		} else {
			std::ofstream out(*presetsTarget);
			if (out) { out << m_presets.dump(2); ui::Toast("Launch presets saved", ui::ToastKind::Success); }
		}
	}
	ImGui::SameLine();
	if (ui::Btn("Run selected", false)) RunStandalone();
	ImGui::End();
}
// ---------------------------------------------------------------------------------------------
// Modals, shortcuts, undo
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::DrawPreferences()
{
	if (!ImGui::Begin(kWinPreferences, &m_showPreferences, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
	ImGui::TextUnformatted("External script editor");
	ImGui::TextDisabled("Opens Lua scripts from the Assets window, the inspector and the Lua project panel.");
	ImGui::Spacing();
	ExternalEditor::Settings& ext = ExternalEditor::Current();
	const char* modes[] = { "Auto (VS Code, else OS default)", "OS default handler", "Custom command" };
	int mode = static_cast<int>(ext.mode);
	ImGui::SetNextItemWidth(320.0f);
	if (ImGui::Combo("Editor", &mode, modes, IM_ARRAYSIZE(modes))) {
		ext.mode = static_cast<ExternalEditor::Mode>(mode);
		SaveSettings();
	}
	const std::string vscode = ExternalEditor::DetectVSCode();
	ImGui::TextDisabled("VS Code: %s", vscode.empty() ? "not found" : vscode.c_str());
	if (ext.mode == ExternalEditor::Mode::Custom) {
		char buffer[512] = {};
		std::snprintf(buffer, sizeof(buffer), "%s", ext.command.c_str());
		ImGui::SetNextItemWidth(480.0f);
		if (ImGui::InputText("Command", buffer, sizeof(buffer))) ext.command = buffer;
		// Saved when the field is left, not on every keystroke.
		if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
		ImGui::TextDisabled("Placeholders: <exe> VS Code (or code), {file} path, {line} line, {root} project folder.");
		ImGui::TextDisabled("Example: \"<exe>\" -g \"{file}:{line}\"");
		if (ImGui::Button("Use the VS Code command")) {
			ext.command = ExternalEditor::DefaultCommand();
			SaveSettings();
		}
	}
	ImGui::End();
}

void EditorWorkspace::DrawModals()
{
	DrawNewProjectModal();
	DrawOpenProjectModal();
	if (m_openRename) {
		ImGui::OpenPopup("Rename Node");
		m_openRename = false;
	}
	if (ImGui::BeginPopupModal("Rename Node", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		auto target = m_renameTarget.lock();
		if (!target || m_core.IsPlaying()) {
			ImGui::CloseCurrentPopup();
		}
		else {
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(260.0f);
			const bool enter = ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			const bool ok = ui::Btn("OK", true);
			ImGui::SameLine();
			const bool cancel = ui::Btn("Cancel", false);
			if (enter || ok) {
				const std::string name = m_renameBuf;
				std::string error;
				if (!name.empty() && name != target->GetName()) {
					if (nox::editor::SetPropertyUndoable(m_nodeTable, UndoSink(), target, "Node", "name",
						nox::reflect::PropValue(name), &error)) {
						NoteEdit();
					}
					else {
						ui::Toast(error.empty() ? std::string("Name rejected") : error, ui::ToastKind::Warning);
					}
				}
				ImGui::CloseCurrentPopup();
			}
			else if (cancel) {
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}
	if (m_openSavePrefab) {
		ImGui::OpenPopup("Save as Prefab");
		m_openSavePrefab = false;
	}
	if (ImGui::BeginPopupModal("Save as Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		auto target = m_prefabTarget.lock();
		if (!target || m_core.IsPlaying()) {
			ImGui::CloseCurrentPopup();
		}
		else {
			ImGui::TextUnformatted("Asset path (relative to the project root):");
			ImGui::SetNextItemWidth(360.0f);
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			const bool enter = ImGui::InputText("##prefabpath", m_prefabPathBuf, sizeof(m_prefabPathBuf), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::TextDisabled("Writes the node and its children; an existing file is replaced.");
			const bool ok = ui::Btn("Save", true);
			ImGui::SameLine();
			const bool cancel = ui::Btn("Cancel", false);
			if (enter || ok) {
				std::string error;
				const auto logical = AssetIO::Normalize(m_prefabPathBuf, &error);
				if (!logical || !nox::prefab::IsPrefabPath(*logical)) {
					ui::Toast(error.empty() ? std::string("A prefab path must end with .prefab.json") : error, ui::ToastKind::Warning);
				}
				else if (m_core.SavePrefabFromNode(target, *logical, &error)) {
					ui::Toast("Prefab saved: " + *logical, ui::ToastKind::Success);
				}
				else {
					ui::Toast(error.empty() ? std::string("Prefab could not be saved") : error, ui::ToastKind::Warning);
				}
				ImGui::CloseCurrentPopup();
			}
			else if (cancel) {
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}
	if (m_openAddModel) {
		m_modelChoices.clear();
		AssetIO::ListOptions options;
		options.recursive = true;
		for (const std::string& path : AssetIO::List("models", options)) {
			const std::string ext = fs::path(path).extension().string();
			if (ext == ".glb" || ext == ".gltf" || ext == ".fbx") m_modelChoices.push_back(path);
		}
		std::sort(m_modelChoices.begin(), m_modelChoices.end());
		m_modelFilter[0] = 0;
		ImGui::OpenPopup("Add Model");
		m_openAddModel = false;
	}
	if (ImGui::BeginPopupModal("Add Model", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::SetNextItemWidth(360.0f);
		ImGui::InputTextWithHint("##modelfilter", "filter models", m_modelFilter, sizeof(m_modelFilter));
		ImGui::BeginChild("##modellist", ImVec2(360.0f, 300.0f), true);
		int shown = 0;
		for (const std::string& path : m_modelChoices) {
			if (!ContainsNoCase(path, m_modelFilter)) continue;
			if (ImGui::Selectable(path.c_str())) {
				AddModelFromAsset(path, m_legacy.GetSelectedNode());
				ImGui::CloseCurrentPopup();
			}
			if (++shown >= 500) break;
		}
		ImGui::EndChild();
		if (ui::Btn("Cancel", false)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (m_openUnsavedModal) {
		ImGui::OpenPopup("Unsaved changes");
		m_openUnsavedModal = false;
	}
	if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("Save changes to %s before opening %s?", m_core.GetCurrentScenePath().c_str(), m_pendingScene.c_str());
		ImGui::Spacing();
		if (ui::Btn("Save", true)) { m_core.SaveCurrentScene(); m_core.RequestSceneSwap(m_pendingScene); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ui::Btn("Discard", false)) { m_core.RequestSceneSwap(m_pendingScene); ImGui::CloseCurrentPopup(); }
		ImGui::SameLine();
		if (ui::Btn("Cancel", false)) { m_pendingScene.clear(); ImGui::CloseCurrentPopup(); }
		ImGui::EndPopup();
	}
}

void EditorWorkspace::HandleShortcuts()
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantTextInput || m_core.IsEditorCameraActive()) return;
	const bool ctrl = io.KeyCtrl;
	if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
		if (io.KeyShift) { if (m_core.IsPlaying()) m_core.RequestStop(); }
		else if (!m_core.IsPlaying()) { m_core.RequestPlay(); FocusGame(); }
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F6, false) && m_core.IsPlaying()) m_core.TogglePause();
	if (GameInputFocus()) return; // keys belong to the game
	if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && m_legacy.GetSelectedNode() && !m_core.IsPlaying()) BeginRename(m_legacy.GetSelectedNode());
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !m_core.IsPlaying()) { m_core.SaveCurrentScene(); ui::Toast("Scene saved", ui::ToastKind::Success); }
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Undo();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
	auto sel = m_legacy.GetSelectedNode();
	const bool scope = NodeShortcutScope();   // the Hierarchy or the Scene viewport has the keyboard
	if (scope && ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateSelectionWithUndo();
	if (scope && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelectionWithUndo();
	if (scope && ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySelection(false);
	if (scope && ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) CopySelection(true);
	if (scope && ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) PasteFromClipboard();
	if (scope && ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) SelectAllTopLevel();
	if (m_scene.focused && !ctrl) {
		if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_gizmoOp = ImGuizmo::TRANSLATE;
		if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_gizmoOp = ImGuizmo::ROTATE;
		if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoOp = ImGuizmo::SCALE;
		if (ImGui::IsKeyPressed(ImGuiKey_F, false) && sel) {
			if (auto cam = m_core.GetCamera()) {
				const glm::vec3 p = sel->GetWorldPosition();
				const glm::vec3 f = cam->GetCameraFrontVector();
				PoseCamera(*cam, p - f * 8.0f, f);
			}
		}
	}
}

void EditorWorkspace::Push(const std::string& label, std::function<void()> undo, std::function<void()> redo)
{
	m_undo.push_back({ label, std::move(undo), std::move(redo) });
	if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
	m_redo.clear();
}

void EditorWorkspace::Undo()
{
	if (m_undo.empty()) return;
	UndoAction a = m_undo.back();
	m_undo.pop_back();
	a.undo();
	m_redo.push_back(a);
	m_core.MarkDirty();
	m_core.InvalidateSceneCaches();
	ui::Toast("Undo " + a.label, ui::ToastKind::Info);
}

void EditorWorkspace::Redo()
{
	if (m_redo.empty()) return;
	UndoAction a = m_redo.back();
	m_redo.pop_back();
	a.redo();
	m_undo.push_back(a);
	m_core.MarkDirty();
	m_core.InvalidateSceneCaches();
	ui::Toast("Redo " + a.label, ui::ToastKind::Info);
}

void EditorWorkspace::TrackGameEdits()
{
	// Widgets edit the live game data; an edit becomes one undo step when the widget is released.
	if (!m_core.GetGameModule() || m_core.IsPlaying()) { m_gameSnapshot = json(); return; }
	const bool active = ImGui::IsAnyItemActive();
	if (!active && m_gameEditActive) {
		CommitGameEdit("Edit game data");
	}
	if (!active || m_gameSnapshot.is_null()) {
		if (!active) m_gameSnapshot = GetAuthoredGameJson();
	}
	m_gameEditActive = active;
}

// NOX_EDITOR_SMOKE=1: scripted editor checks for unattended validation (undo/redo of a transform
// edit, Play/Stop restoring the pre-play state, viewport resize, save). Results go to the console.
void EditorWorkspace::RunSmokeScript()
{
	static const bool enabled = [] { const char* v = std::getenv("NOX_EDITOR_SMOKE"); return v && v[0] == '1'; }();
	if (!enabled) return;
	static int frame = 0;
	static std::shared_ptr<SceneNode> subject;
	static glm::vec3 original(0.0f);
	static int texW0 = 0;
	static bool hasPlayer = false;
	static glm::vec3 playerBefore(0.0f);
	++frame;
	auto check = [](bool ok, const char* what) { std::cout << "[Smoke] " << (ok ? "PASS " : "FAIL ") << what << std::endl; };
	auto graph = m_core.GetSceneGraph();
	// NOX_EDITOR_SELECT=<node name>: selects that node at frame 40 and checks that its outline covers
	// the subtree. The edit checks below are skipped so the selection stays for frame dumps.
	const char* selectName = std::getenv("NOX_EDITOR_SELECT");
	const bool selectNamed = selectName && selectName[0];
	if (selectNamed && frame == 40 && graph) {
		std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<SceneNode>&)> byName = [&](const std::shared_ptr<SceneNode>& n) -> std::shared_ptr<SceneNode> {
			if (!n) return nullptr;
			if (n->GetName() == selectName) return n;
			for (const auto& c : n->children) if (auto f = byName(c)) return f;
			return nullptr;
		};
		auto named = byName(graph->GetRoot());
		check(named != nullptr, "select named node for outline");
		if (named) SelectNode(named);
		ImGui::SetWindowFocus(kWinScene);   // the outline is drawn for the visible Scene tab only
	}
	if (selectNamed && frame == 50) {
		const ModularRenderer* renderer = GetRenderer();
		const size_t covered = renderer ? renderer->GetContext().selectionOutline.selectedEntities.size() : 0;
		check(covered > 0, "selection outline covers the selected subtree");
	}
	if (frame == 60 && graph && !selectNamed) {
		// First authored node below the root (every node has a transform).
		std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<SceneNode>&)> find = [&](const std::shared_ptr<SceneNode>& n) -> std::shared_ptr<SceneNode> {
			if (!n) return nullptr;
			if (n != graph->GetRoot() && !n->IsGenerated()) return n;
			for (const auto& c : n->children) if (auto f = find(c)) return f;
			return nullptr;
		};
		subject = find(graph->GetRoot());
		check(subject != nullptr, "select node");
		if (subject) {
			SelectNode(subject);
			original = subject->GetPosition();
			const auto before = CaptureTRS(subject);
			subject->SetLocalTRS(original + glm::vec3(0.0f, 0.0f, 0.0f), subject->GetOrientation(), subject->GetScale() * 1.0f);
			ApplyTRS(subject, { original.x + 0.5f, original.y, original.z, before[3], before[4], before[5], before[6], before[7], before[8], before[9] });
			const auto after = CaptureTRS(subject);
			std::weak_ptr<SceneNode> weak = subject;
			Push("Smoke transform", [weak, before] { if (auto n = weak.lock()) ApplyTRS(n, before); }, [weak, after] { if (auto n = weak.lock()) ApplyTRS(n, after); });
			m_core.MarkDirty();
			check(m_core.IsDirty(), "dirty after edit");
		}
	}
	if (frame == 70 && subject) {
		Undo();
		check(glm::length(subject->GetPosition() - original) < 1e-4f, "undo restores transform");
		Redo();
		check(std::abs(subject->GetPosition().x - original.x - 0.5f) < 1e-4f, "redo reapplies transform");
		Undo();
	}
	if (frame == 75 && subject && graph) {
		// Folders: create one under the root, group the subject into a new folder, then undo both steps.
		const auto root = graph->GetRoot();
		const size_t rootCount = root->children.size();
		CreateFolderUnder(root);
		const auto folder = root->children.empty() ? nullptr : root->children.back();
		check(folder && folder->IsFolder() && root->children.size() == rootCount + 1, "new folder under root");
		GroupIntoFolderWithUndo(subject);
		const auto parent = subject->parentNode.lock();
		check(parent && parent->IsFolder(), "group moves the node into a folder");
		Undo();
		check(subject->parentNode.lock() == root, "undo group restores the parent");
		Undo();
		check(root->children.size() == rootCount, "undo new folder removes it");
	}
	if (frame == 77 && graph) {
		// Multi-selection (Ctrl+click, copy/paste, multi-delete, select all), each undo step checked. Three empty nodes
		// are made under the root for the test; the file is restored by the save check at frame 330.
		const auto root = graph->GetRoot();
		const size_t before = root->children.size();
		std::vector<std::shared_ptr<SceneNode>> trio;
		for (int i = 0; i < 3; ++i) {
			json j = NodeTemplate("empty");
			j["name"] = "SmokeSel" + std::to_string(i);
			j["position"] = { 2.0f * static_cast<float>(i), 0.5f, 7.0f };
			if (auto n = m_core.InstantiateNodeFromJson(j, root)) trio.push_back(n);
		}
		check(trio.size() == 3, "smoke: three nodes for multi-select");
		if (trio.size() == 3) {
			SelectNode(trio[0]);
			ClickSelect(trio[1], true, false);
			ClickSelect(trio[2], true, false);
			check(m_selection.Size() == 3 && m_selection.Primary() == trio[2] && m_legacy.GetSelectedNode() == trio[2],
				"ctrl+click builds a three-node selection, last clicked is primary");
			const size_t rootCount = root->children.size();
			CopySelection(false);
			PasteFromClipboard();
			check(root->children.size() == rootCount + 3, "paste creates three nodes");
			bool sameTransforms = root->children.size() == rootCount + 3;
			bool sameNames = true;
			if (sameTransforms) {
				for (size_t i = 0; i < 3; ++i) {
					const auto copy = root->children[rootCount + i];
					const auto a = CaptureTRS(trio[i]), b = CaptureTRS(copy);
					for (size_t k = 0; k < 10; ++k) sameTransforms = sameTransforms && std::abs(a[k] - b[k]) < 1e-4f;
					sameNames = sameNames && copy->GetName() == trio[i]->GetName() + " (1)";
				}
			}
			check(sameTransforms, "pasted nodes keep the copied transforms");
			check(sameNames, "pasted nodes get unique names (Name (1))");
			check(m_selection.Size() == 3, "paste selects the new nodes");
			Undo();
			check(root->children.size() == rootCount, "one undo removes all three pasted nodes");
			Redo();
			check(root->children.size() == rootCount + 3, "redo restores the three pasted nodes");
			Undo();

			m_selection.SetAll(trio);
			m_legacy.SetSelectedNode(m_selection.Primary());
			DeleteSelectionWithUndo();
			check(root->children.size() == rootCount - 3, "multi-select delete removes all three");
			Undo();
			check(root->children.size() == rootCount, "one undo restores all three deleted nodes");

			size_t topLevel = 0;
			for (const auto& c : root->children) if (c && !c->IsGenerated()) ++topLevel;
			SelectAllTopLevel();
			check(m_selection.Size() == topLevel && topLevel > 0, "ctrl+a selects all top-level nodes");
			m_selection.Clear();
			m_legacy.SetSelectedNode(nullptr);
			// Multi-edit: a name set on the primary goes to all three selected nodes, and one undo reverts all three.
			{
				// The undo of the delete above re-created the three nodes: look them up again by name.
				std::vector<std::shared_ptr<SceneNode>> live;
				for (const auto& c : root->children) if (c && c->GetName().rfind("SmokeSel", 0) == 0) live.push_back(c);
				check(live.size() == 3, "smoke: three nodes restored by undo");
				if (live.size() == 3) {
					m_selection.SetAll(live);
					m_legacy.SetSelectedNode(m_selection.Primary());
					const std::string primaryName = live[2]->GetName();
					ApplyMultiEdit(live, "Node", "name", nox::reflect::PropValue(primaryName), nox::reflect::PropValue(std::string("SmokeMulti")), true);
					check(live[0]->GetName() == "SmokeMulti" && live[1]->GetName() == "SmokeMulti" && live[2]->GetName() == "SmokeMulti",
						"multi-edit sets the property on all three selected nodes");
					Undo();
					check(live[0]->GetName() == "SmokeSel0" && live[1]->GetName() == "SmokeSel1" && live[2]->GetName() == "SmokeSel2",
						"one undo reverts the multi-edit on all three nodes");
				}
			}
			// Leave the scene as it was: delete the three nodes this block made (their restore is the saved file's job).
			std::vector<std::shared_ptr<SceneNode>> made;
			for (const auto& c : root->children) if (c && c->GetName().rfind("SmokeSel", 0) == 0) made.push_back(c);
			m_selection.SetAll(made);
			DeleteSelectionWithUndo();
			check(root->children.size() == before, "smoke nodes removed again");
		}
		m_selection.Clear();
		m_legacy.SetSelectedNode(nullptr);
	}
	if (frame == 75) {
		// Physics window: focus its tab so it draws (it docks with the Inspector).
		ImGui::SetWindowFocus(kWinPhysics);
	}
	if (frame == 80) {
		// A live physics edit reaches the engine and undoes (before Play: play edits are not undoable).
		check(m_physicsDrawn, "physics window drawn");
		PhysicsWindow::Host host = PhysicsHost();
		if (host.physics) {
			const float gravityBefore = host.physics->GetConfig().gravityY;
			m_physicsWindow.Edit(host, "Smoke gravity", [](SimulationConfig& c) { c.gravityY = -3.0f; });
			check(host.physics->GetConfig().gravityY == -3.0f && host.physics->GetAuthoredConfig().gravityY == -3.0f, "physics edit applies live");
			Undo();
			check(host.physics->GetAuthoredConfig().gravityY == gravityBefore, "physics edit undoes");
		}
	}
	if (frame == 90) {
		texW0 = m_game.texWidth;
		IGameModule* game = m_core.GetGameModule();
		hasPlayer = game && game->GetPlayerPose(playerBefore);
		m_core.RequestPlay();   // Play needs a game module (Core::RequestPlay)
		FocusGame();
		if (m_core.GetGameModule()) check(m_core.IsPlaying(), "play");
	}
	// Content Browser: waits for the first scan, opens images/ (thumbnails) and checks on the next frame that it draws items.
	if (frame == 60 && m_contentBrowser) {
		check(m_contentBrowser->WaitForScan(std::chrono::seconds(20)), "content browser scan");
		check(m_contentBrowser->SmokeNavigate("images"), "content browser opens images/");
	}
	if (frame == 61 && m_contentBrowser) {
		check(m_contentBrowser->Folder() == "images" && m_contentBrowser->VisibleCount() > 0, "content browser draws images/ items");
	}
	if (frame == 120) {
		extern SDL_Window* NoxMainWindow();
		if (SDL_Window* w = NoxMainWindow()) SDL_SetWindowSize(w, 1600, 900);
	}
	if (frame == 300) {
		check(m_game.texWidth != texW0 && m_game.texWidth > 0, "viewport framebuffer follows window resize");
		glm::vec3 playerDuring(0.0f);
		IGameModule* game = m_core.GetGameModule();
		const bool playerRunning = game && game->GetPlayerPose(playerDuring);
		m_core.RequestStop();
		check(!m_core.IsPlaying(), "stop");
		if (hasPlayer && playerRunning) {
			glm::vec3 playerAfter(0.0f);
			check(game->GetPlayerPose(playerAfter) && glm::length(playerAfter - playerBefore) < 0.01f, "stop restores player pose");
			std::cout << "[Smoke] player travelled to (" << playerDuring.x << ", " << playerDuring.z << ") during play" << std::endl;
		}
	}
	if (frame == 330) {
		// Exercises the real save path, then restores the file byte-for-byte: a test must not
		// rewrite authored scenes (runtime poses would otherwise leak into them).
		const std::string path = m_core.GetCurrentScenePath();
		const std::string original = AssetIO::ReadText(path).value_or(std::string());
		const json before = json::parse(original, nullptr, false);
		m_core.SaveCurrentScene();
		const json after = AssetIO::ReadJsonOrNull(path);
		check(after.is_object() && after.contains("nodes"), "save writes a valid scene");
		if (m_core.GetGameModule()) {
			{
				// Scenes without a game object (engine scenes, Lua projects) have no events: compare as empty.
				auto eventCount = [](const json& doc) {
					const json game = doc.is_object() ? doc.value("game", json::object()) : json::object();
					return game.is_object() ? game.value("events", json::array()).size() : size_t(0);
				};
				check(before.is_object() && eventCount(after) == eventCount(before), "save keeps game events");
			}
		}
		check(!m_core.IsDirty(), "clean after save");
		if (!original.empty()) {
			if (const auto target = AssetIO::SourcePath(path)) {
				std::ofstream restore(*target, std::ios::binary | std::ios::trunc);
				restore << original;
			}
		}
	}
	if (frame == 360) m_quit = true;
}
