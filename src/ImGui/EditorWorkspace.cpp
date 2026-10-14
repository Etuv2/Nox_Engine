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
#include "../UiIconAtlas.h"
#include "../Scene.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../editor/ProcessRunner.h"
#include "../editor/solace/SolaceUI.h"
#include "../editor/InspectorUndo.h"
#include "../scene/Prefab.h"
#include "InspectorWidgets.h"

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
	// Legacy panels (BaseWindow names).
	const char* kWinRendering = "Rendering Settings";
	const char* kWinLighting = "Lighting System";
	const char* kWinPerformance = "Performance Monitor";
	const char* kWinAnimation = "Animation Preview";
	const char* kWinState = "State & Export";
	const char* kWinCamera = "Camera Controls";

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
		m_showAuthoring = s.value("authoring_overlay", m_showAuthoring);
		m_extSaved = s.value("extension_panels", json::object());
		if (!m_extSaved.is_object()) m_extSaved = json::object();
		m_layoutBuilt = s.value("layout_built", false);
		m_selectedPreset = s.value("preset", m_selectedPreset);
	}
	m_presets = AssetIO::ReadJsonOrNull(kPresetsPath);
	if (!m_presets.is_object()) m_presets = json::object();
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

void EditorWorkspace::SaveSettings() const
{
	json s;
	s["panels"] = {
		{"hierarchy", m_showHierarchy}, {"assets", m_showAssets}, {"inspector", m_showInspector}, {"materials", m_showMaterials},
		{"console", m_showConsole}, {"build", m_showBuild},
		{"rendering", m_showLegacyRendering}, {"lighting", m_showLegacyLighting}, {"performance", m_showLegacyPerformance},
		{"animation", m_showLegacyAnimation}, {"state", m_showLegacyState}, {"camera", m_showLegacyCamera}, {"presets", m_showPresets} };
	json ext = json::object();
	for (const auto& [id, open] : m_extOpen) ext[id] = open;
	s["extension_panels"] = ext;
	s["authoring_overlay"] = m_showAuthoring;
	s["layout_built"] = m_layoutBuilt;
	s["preset"] = m_selectedPreset;
	UserSettings::SaveUserJson(kSettingsPath, s);
}

void EditorWorkspace::OnSceneLoaded()
{
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
	ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
	ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
	ImGui::DockBuilderDockWindow(kWinScene, center);
	ImGui::DockBuilderDockWindow(kWinGame, center);
	ImGui::DockBuilderDockWindow(kWinHierarchy, left);
	ImGui::DockBuilderDockWindow(kWinAssets, leftBottom);
	ImGui::DockBuilderDockWindow(kWinInspector, right);
	ImGui::DockBuilderDockWindow(kWinMaterials, right);
	ImGui::DockBuilderDockWindow(kWinLighting, right);
	ImGui::DockBuilderDockWindow(kWinRendering, right);
	ImGui::DockBuilderDockWindow(kWinCamera, right);
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
		if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !m_core.IsPlaying())) {
			m_core.SaveCurrentScene();
			ui::Toast("Scene saved", ui::ToastKind::Success);
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
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel != nullptr && !m_core.IsPlaying())) DuplicateNodeWithUndo(sel);
		if (ImGui::MenuItem("Delete", "Del", false, sel != nullptr && !m_core.IsPlaying())) DeleteNodeWithUndo(sel);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Node")) {
		if (ImGui::BeginMenu("Add Node", !m_core.IsPlaying())) {
			DrawAddNodeItems();
			ImGui::EndMenu();
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
		}
		ImGui::MenuItem("ImGui Demo", nullptr, &m_showDemo);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Build")) {
		const bool busy = m_runner->IsRunning();
		if (ImGui::MenuItem("Build Release", nullptr, false, !busy)) StartTool("Build", "tools/Build.ps1", "-Configuration Release");
		if (ImGui::MenuItem("Run Standalone", nullptr, false, true)) RunStandalone();
		if (ImGui::MenuItem("Package", nullptr, false, !busy)) StartTool("Package", "tools/Package.ps1", "-Configuration Release");
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
	if (ui::Tool("##package", solace::Icon::Package, "Package", false, !busy, "Package a self-contained build (tools/Package.ps1)")) StartTool("Package", "tools/Package.ps1", "-Configuration Release");
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

	// Click selection (not on gizmo or authoring handles).
	if (m_scene.hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
		m_dragHandle < 0 && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0.0f && !m_core.IsEditorCameraActive()) {
		bool handleHit = false;
		for (const ViewportHandle& h : m_handles) {
			ImVec2 p;
			if (ProjectToViewport(m_scene, h.world, p) && std::hypot(p.x - io.MousePos.x, p.y - io.MousePos.y) < 10.0f) { handleHit = true; break; }
		}
		if (!handleHit) {
			const ImVec2 scale = io.DisplayFramebufferScale;
			const int x = static_cast<int>((io.MousePos.x - m_scene.pos.x) * scale.x);
			const int y = static_cast<int>((io.MousePos.y - m_scene.pos.y) * scale.y);
			SelectNode(m_core.PickNode(x, y, m_scene.texWidth, m_scene.texHeight));
		}
	}

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
	auto node = m_legacy.GetSelectedNode();
	auto cam = m_core.GetCamera();
	if (!node || !cam || m_core.IsEditorCameraActive()) {
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
	m_legacy.SetSelectedNode(node);
}

void EditorWorkspace::DrawHierarchyNode(const std::shared_ptr<SceneNode>& node)
{
	if (!node) return;
	if (node->IsGenerated() && m_hierarchyFilter[0] == 0) return; // generated lights are rebuilt on load
	const std::string name = node->GetName().empty() ? std::string("(unnamed)") : node->GetName();
	bool childMatches = false;
	if (m_hierarchyFilter[0]) {
		std::function<bool(const std::shared_ptr<SceneNode>&)> any = [&](const std::shared_ptr<SceneNode>& n) {
			if (ContainsNoCase(n->GetName(), m_hierarchyFilter)) return true;
			for (const auto& c : n->children) if (any(c)) return true;
			return false;
		};
		if (!any(node)) return;
		childMatches = true;
	}
	const bool playing = m_core.IsPlaying();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (node->children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
	if (m_legacy.GetSelectedNode() == node) flags |= ImGuiTreeNodeFlags_Selected;
	if (childMatches) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	const char* tag = "";
	ImU32 tagCol = IM_COL32(150, 150, 160, 255);
	switch (node->GetNodeType()) {
	case SceneNode::LIGHT: tag = "LIGHT"; tagCol = IM_COL32(255, 210, 90, 255); break;
	case SceneNode::MODEL: tag = "MESH"; tagCol = IM_COL32(120, 180, 255, 255); break;
	case SceneNode::AUDIO: tag = "AUDIO"; tagCol = IM_COL32(90, 230, 220, 255); break;
	case SceneNode::CAMERA: tag = "CAM"; tagCol = IM_COL32(120, 255, 140, 255); break;
	default: break;
	}
	const bool open = ImGui::TreeNodeEx(node.get(), flags, "%s", name.c_str());
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) SelectNode(node);
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) BeginRename(node);
	// Drag a node onto another to reparent it (world transform kept).
	if (ImGui::BeginDragDropSource()) {
		SceneNode* raw = node.get();
		ImGui::SetDragDropPayload(nox::editor::kNodeDragType, &raw, sizeof(raw));
		ImGui::TextUnformatted(name.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(nox::editor::kNodeDragType)) {
			if (payload->DataSize == sizeof(SceneNode*)) {
				SceneNode* raw = *static_cast<SceneNode* const*>(payload->Data);
				if (auto dragged = FindSharedNode(RootNode(), raw)) ReparentWithUndo(dragged, node);
			}
		}
		if (const ImGuiPayload* asset = ImGui::AcceptDragDropPayload(nox::editor::kAssetDragType)) {
			const std::string path(static_cast<const char*>(asset->Data));
			if (nox::prefab::IsPrefabPath(path)) InstantiatePrefabFromAsset(path, node);
		}
		ImGui::EndDragDropTarget();
	}
	if (tag[0]) {
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(tag).x - 4);
		ImGui::PushStyleColor(ImGuiCol_Text, tagCol);
		ImGui::TextUnformatted(tag);
		ImGui::PopStyleColor();
	}
	if (ImGui::BeginPopupContextItem()) {
		SelectNode(node);
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
		if (ImGui::MenuItem("Delete", "Del", false, !playing)) {
			DeleteNodeWithUndo(node);
			ImGui::EndPopup();
			if (open) ImGui::TreePop();
			return;
		}
		if (ImGui::BeginMenu("Add Child", !playing)) {
			DrawAddNodeItems();
			ImGui::EndMenu();
		}
		ImGui::EndPopup();
	}
	if (open) {
		for (const auto& c : node->children) DrawHierarchyNode(c);
		ImGui::TreePop();
	}
}

void EditorWorkspace::DrawHierarchy()
{
	if (!ImGui::Begin(kWinHierarchy, &m_showHierarchy)) { ImGui::End(); return; }
	solace::SearchField("##hfilter", m_hierarchyFilter, sizeof(m_hierarchyFilter), "Filter nodes");
	ImGui::Separator();
	ImGui::BeginChild("##tree");
	if (auto root = RootNode()) {
		for (const auto& c : root->children) DrawHierarchyNode(c);
		// Empty space below the nodes: drop here to move a node to the scene root; right-click adds nodes.
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
				const std::string path(static_cast<const char*>(asset->Data));
				if (nox::prefab::IsPrefabPath(path)) InstantiatePrefabFromAsset(path, root);
			}
			ImGui::EndDragDropTarget();
		}
		if (ImGui::BeginPopupContextItem("##hierarchy_ctx", ImGuiPopupFlags_MouseButtonRight)) {
			SelectNode(nullptr);
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

void EditorWorkspace::AddModelFromAsset(const std::string& path)
{
	if (m_core.IsPlaying()) return;
	const auto parent = m_legacy.GetSelectedNode();
	const glm::vec3 position = SpawnPosition(m_core, parent, 0.0f);
	if (auto node = m_core.AddModelNode(path, position, parent)) {
		nox::editor::RecordCreatedNode(m_nodeTable, CoreNodeOps(), UndoSink(), "Add " + fs::path(path).stem().string(), node);
		SelectNode(node);
		NoteEdit();
	}
}

void EditorWorkspace::DrawAddNodeItems()
{
	const bool playing = m_core.IsPlaying();
	if (ImGui::MenuItem("Empty", nullptr, false, !playing)) AddNodeFromTemplate("empty");
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

void EditorWorkspace::ReparentWithUndo(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<SceneNode>& parent)
{
	if (!node || !parent || node == parent || m_core.IsPlaying()) return;
	if (node->parentNode.lock() == parent) return;
	if (nox::editor::ReparentNodeUndoable(m_nodeTable, CoreNodeOps(), UndoSink(), node, parent, -1)) NoteEdit();
	else ui::Toast("A node cannot be moved under itself", ui::ToastKind::Warning);
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

void EditorWorkspace::InstantiatePrefabFromAsset(const std::string& path, const std::shared_ptr<SceneNode>& parent)
{
	if (m_core.IsPlaying()) return;
	const glm::vec3 position = SpawnPosition(m_core, parent, 0.0f);
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
	if (!ImGui::Begin(kWinAssets, &m_showAssets)) { ImGui::End(); return; }
	static const char* folders[] = { "scenes", "prefabs", "models", "textures", "hdrs", "sounds", "fonts", "config", "shaders", "generated" };
	ImGui::SetNextItemWidth(110);
	if (ImGui::BeginCombo("##folder", m_assetFolder.c_str())) {
		for (const char* f : folders) if (ImGui::Selectable(f, m_assetFolder == f)) { m_assetFolder = f; m_assetCacheTimer = 0.0f; }
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	solace::SearchField("##afilter", m_assetFilter, sizeof(m_assetFilter), "Search assets");
	m_assetCacheTimer -= m_dt;
	if (m_assetCacheTimer <= 0.0f) {
		m_assetCacheTimer = 2.0f;
		m_assetCache.clear();
		std::error_code ec;
		AssetIO::ListOptions options;
		options.recursive = true;
		m_assetCache = AssetIO::List(m_assetFolder, options);
		if (m_assetCache.size() > 4000) m_assetCache.resize(4000);
		std::sort(m_assetCache.begin(), m_assetCache.end());
	}
	ImGui::BeginChild("##assets");
	for (const std::string& path : m_assetCache) {
		if (!ContainsNoCase(path, m_assetFilter)) continue;
		const std::string ext = fs::path(path).extension().string();
		ImGui::PushID(path.c_str());
		ImGui::Selectable(path.c_str());
		if (ImGui::BeginDragDropSource()) {
			ImGui::SetDragDropPayload(nox::editor::kAssetDragType, path.c_str(), path.size() + 1);
			ImGui::TextUnformatted(path.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			if (ext == ".json" && m_assetFolder == "scenes") RequestScene(path);
		}
		if (ImGui::BeginPopupContextItem()) {
			if (ext == ".json" && m_assetFolder == "scenes" && ImGui::MenuItem("Open Scene")) RequestScene(path);
			if (nox::prefab::IsPrefabPath(path) && ImGui::MenuItem("Add to Scene", nullptr, false, !m_core.IsPlaying())) {
				InstantiatePrefabFromAsset(path, m_legacy.GetSelectedNode());
			}
			if ((ext == ".glb" || ext == ".gltf" || ext == ".fbx") && ImGui::MenuItem("Add to Scene", nullptr, false, !m_core.IsPlaying())) {
				AddModelFromAsset(path);
			}
			if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(path.c_str());
			ImGui::EndPopup();
		}
		if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
			std::error_code ec;
			ImGui::Text("%s\n%.1f KB", path.c_str(), static_cast<double>(fs::file_size(path, ec)) / 1024.0);
			ImGui::EndTooltip();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::End();
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
		if (!node->GetPrefabPath().empty()) {
			ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Prefab: %s", node->GetPrefabPath().c_str());
			ImGui::SameLine();
			if (ui::Btn("Unpack", false) && !m_core.IsPlaying()) UnpackPrefabWithUndo(node);
		}
		// Node names for NodeRef pickers (flattened scene graph, root excluded).
		std::vector<std::string> nodeNames;
		std::function<void(const std::shared_ptr<SceneNode>&)> collect = [&](const std::shared_ptr<SceneNode>& n) {
			for (const auto& c : n->children) {
				nodeNames.push_back(c->GetName());
				collect(c);
			}
		};
		if (auto root = RootNode()) collect(root);

		// Builtin facets (Node, Transform) first; the registry sorts the rest by their order.
		std::vector<const nox::reflect::ComponentDesc*> components = nox::reflect::Registry::Get().ComponentsOf(*node);
		std::stable_partition(components.begin(), components.end(),
			[](const nox::reflect::ComponentDesc* d) { return d->builtin; });
		for (const nox::reflect::ComponentDesc* desc : components) {
			DrawComponentSection(node, *desc, m_assetCache, nodeNames);
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
}

void EditorWorkspace::DrawPlayBanner()
{
	const ImVec4 colour = m_core.IsPaused() ? ImVec4(0.62f, 0.45f, 0.10f, 1.0f) : ImVec4(0.62f, 0.16f, 0.12f, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, colour);
	ImGui::BeginChild("##playbanner", ImVec2(-1.0f, ImGui::GetFrameHeight() + 8.0f), false, ImGuiWindowFlags_NoScrollbar);
	ImGui::SetCursorPos(ImVec2(8.0f, 4.0f));
	ImGui::TextColored(ImVec4(1, 1, 1, 1), "Playing: changes are not saved");
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

		solace::PropertyLabel(prop.label.c_str(), prop.tooltip.empty() ? nullptr : prop.tooltip.c_str());
		const std::string widgetId = desc.name + "." + prop.name;
		const bool ownsEdit = m_propEdit.active && m_propEdit.component == desc.name && m_propEdit.property == prop.name
			&& m_nodeTable.Resolve(m_propEdit.node) == node;
		const nox::editor::PropertyWidgetResult r = nox::editor::DrawPropertyWidget(prop, value, assets, nodeNames, widgetId.c_str());
		if (r.changed) {
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

void EditorWorkspace::DrawMaterials()
{
	if (!ImGui::Begin(kWinMaterials, &m_showMaterials)) { ImGui::End(); return; }
	auto node = m_legacy.GetSelectedNode();
	std::shared_ptr<Scene> model = node ? node->GetModel() : nullptr;
	// Imported mesh children share the parent's model: walk up to the node that owns the file.
	if (!model) {
		ImGui::TextDisabled("Select a model node to edit its materials.");
		ImGui::End();
		return;
	}
	static int selectedMaterial = 0;
	std::vector<std::string> names;
	for (const MeshComponent& m : model->meshes) {
		if (std::find(names.begin(), names.end(), m.materialName) == names.end()) names.push_back(m.materialName);
	}
	if (names.empty()) { ImGui::End(); return; }
	selectedMaterial = std::clamp(selectedMaterial, 0, static_cast<int>(names.size()) - 1);
	ImGui::SetNextItemWidth(-1);
	if (ImGui::BeginCombo("##mat", names[selectedMaterial].empty() ? "(unnamed)" : names[selectedMaterial].c_str())) {
		for (int i = 0; i < static_cast<int>(names.size()); ++i) {
			if (ImGui::Selectable((names[i].empty() ? "(unnamed)##" + std::to_string(i) : names[i]).c_str(), i == selectedMaterial)) selectedMaterial = i;
		}
		ImGui::EndCombo();
	}
	const std::string matName = names[selectedMaterial];
	MeshComponent* first = nullptr;
	for (MeshComponent& m : model->meshes) if (m.materialName == matName) { first = &m; break; }
	if (!first) { ImGui::End(); return; }
	MaterialDesc d = first->GetMaterialDesc();
	bool changed = false;
	ui::Grid("##matgrid");
	ui::Label("Base color"); changed |= ImGui::ColorEdit4("##bc", &d.baseColorFactor.x);
	ui::Label("Metallic"); changed |= ImGui::SliderFloat("##me", &d.metallicFactor, 0.0f, 1.0f);
	ui::Label("Roughness"); changed |= ImGui::SliderFloat("##ro", &d.roughnessFactor, 0.0f, 1.0f);
	ui::Label("Clearcoat"); changed |= ImGui::SliderFloat("##cc", &d.clearcoatFactor, 0.0f, 1.0f);
	ui::Label("Coat rough."); changed |= ImGui::SliderFloat("##ccr", &d.clearcoatRoughnessFactor, 0.0f, 1.0f);
	ui::Label("Emissive"); changed |= ImGui::ColorEdit3("##em", &d.emissiveFactor.x);
	ui::Label("Emissive cd/m2"); changed |= ImGui::DragFloat("##ems", &d.emissiveStrength, 1.0f, 0.0f, 100000.0f);
	ui::Label("Wetness"); changed |= ImGui::SliderFloat("##wet", &d.wetnessResponse, 0.0f, 1.0f);
	ui::Label("Puddles"); changed |= ImGui::SliderFloat("##pud", &d.puddleResponse, 0.0f, 1.0f);
	ui::Label("Rain beads"); changed |= ImGui::SliderFloat("##rb", &d.rainBeadResponse, 0.0f, 1.0f);
	ui::EndGrid();
	if (changed) {
		for (MeshComponent& m : model->meshes) if (m.materialName == matName) m.SetMaterialDesc(d);
		m_core.MarkDirty();
	}
	ImGui::Spacing();
	if (ui::Btn("Save as scene override", true)) {
		// Persist through the node's material_overrides (written back by SceneLoader::SaveScene).
		json extras = node->GetAuthoredExtras().empty() ? json::object() : json::parse(node->GetAuthoredExtras(), nullptr, false);
		if (!extras.is_object()) extras = json::object();
		json overrides = extras.value("material_overrides", json::array());
		json entry = { {"match", matName}, {"base_color", { d.baseColorFactor.r, d.baseColorFactor.g, d.baseColorFactor.b, d.baseColorFactor.a }},
			{"metallic", d.metallicFactor}, {"roughness", d.roughnessFactor}, {"clearcoat", d.clearcoatFactor},
			{"clearcoat_roughness", d.clearcoatRoughnessFactor}, {"emissive", { d.emissiveFactor.r, d.emissiveFactor.g, d.emissiveFactor.b }},
			{"emissive_strength", d.emissiveStrength}, {"wetness_response", d.wetnessResponse}, {"puddle_response", d.puddleResponse},
			{"rain_bead_response", d.rainBeadResponse} };
		bool replaced = false;
		for (json& o : overrides) if (o.value("match", std::string()) == matName) { o.update(entry); replaced = true; }
		if (!replaced) overrides.push_back(entry);
		extras["material_overrides"] = overrides;
		node->SetAuthoredExtras(extras.dump());
		m_core.MarkDirty();
		ui::Toast("Material override stored - save the scene to keep it", ui::ToastKind::Info);
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
	const std::string cmd = "\"" + exe + "\" --preset " + m_selectedPreset + " --game";
	if (ProcessRunner::LaunchDetached(cmd, fs::current_path().string())) ui::Toast("Launched standalone (" + m_selectedPreset + ")", ui::ToastKind::Success);
	else ui::Toast("Could not launch " + exe, ui::ToastKind::Error);
	(void)exePath;
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
void EditorWorkspace::DrawModals()
{
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
				AddModelFromAsset(path);
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
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && sel && !m_core.IsPlaying()) DuplicateNodeWithUndo(sel);
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && sel && !m_core.IsPlaying() && (m_scene.focused || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))) {
		if (m_scene.focused) DeleteNodeWithUndo(sel);
	}
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
	if (frame == 60 && graph) {
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
	if (frame == 90) {
		texW0 = m_game.texWidth;
		IGameModule* game = m_core.GetGameModule();
		hasPlayer = game && game->GetPlayerPose(playerBefore);
		m_core.RequestPlay();   // Play needs a game module (Core::RequestPlay)
		FocusGame();
		if (m_core.GetGameModule()) check(m_core.IsPlaying(), "play");
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
