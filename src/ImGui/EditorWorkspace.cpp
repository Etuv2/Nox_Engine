#include "EditorWorkspace.h"

#include "ConsoleLog.h"
#include "../Core.h"
#include "../Camera.h"
#include "../GameSession.h"
#include "../ImGuiInterface.h"
#include "../LightNode.h"
#include "../BaseLight.h"
#include "../ModularRenderer.h"
#include "../RacingHUD.h"
#include "../UiIconAtlas.h"
#include "../Scene.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../editor/ProcessRunner.h"
#include "../editor/solace/SolaceUI.h"

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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <cstdlib>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

// Thin adapters over the Solace kit (src/editor/solace/SolaceUI.h).
namespace ui {
	enum class ToastKind { Info, Success, Warning, Error };
	bool g_gridOpen = false;
	void Grid(const char* id) { g_gridOpen = solace::BeginPropertyGrid(id); }
	void Label(const char* label) {
		if (g_gridOpen) { solace::PropertyLabel(label); return; }
		ImGui::TextUnformatted(label);
		ImGui::SameLine(140.0f);
		ImGui::SetNextItemWidth(-1);
	}
	void EndGrid() { if (g_gridOpen) solace::EndPropertyGrid(); g_gridOpen = false; }
	bool Btn(const char* label, bool primary) { return solace::Button(label, primary ? solace::ButtonKind::Primary : solace::ButtonKind::Secondary); }
	bool Tool(const char* id, solace::Icon icon, const char* label, bool active, bool enabled, const char* tip) {
		return solace::ToolbarButton(id, icon, label, active, enabled, tip);
	}
	// Same toolbar button with a UI icon-atlas picture (hud.json icons.roles) in place of the
	// vector glyph; falls back to Tool() when the atlas or the icon is unavailable.
	bool ToolIcon(const char* id, const std::string& atlasId, solace::Icon fallback, const char* label, bool active,
		bool enabled, const char* tip) {
		if (!UiIconAtlas::Get().Has(atlasId)) return Tool(id, fallback, label, active, enabled, tip);
		ImGui::PushID(id);
		const ImGuiStyle& st = ImGui::GetStyle();
		const float h = ImGui::GetFrameHeight();
		const float icon = h * 0.5f;
		const float pad = st.FramePadding.x + 2.0f;
		const float gap = 6.0f;
		const bool hasText = label && *label;
		const ImVec2 text = hasText ? ImGui::CalcTextSize(label) : ImVec2(0.0f, 0.0f);
		const float w = pad + icon + (hasText ? gap + text.x : 0.0f) + pad;
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 max(min.x + w, min.y + h);
		ImGui::BeginDisabled(!enabled);
		const bool clicked = ImGui::InvisibleButton("##tool_icon", ImVec2(w, h));
		ImGui::EndDisabled();
		const bool hovered = enabled && ImGui::IsItemHovered();
		if (tip && hovered) ImGui::SetTooltip("%s", tip);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (active || hovered) {
			dl->AddRectFilled(min, max, ImGui::GetColorU32(active ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), st.FrameRounding);
		}
		ImVec4 fg = st.Colors[ImGuiCol_Text];
		if (!(active || hovered)) fg.w *= 0.7f;
		fg.w *= enabled ? 1.0f : 0.4f;
		const ImU32 fgCol = ImGui::GetColorU32(fg);
		UiIconAtlas::Get().DrawCentered(dl, atlasId, ImVec2(min.x + pad + icon * 0.5f, min.y + h * 0.5f), icon, fgCol);
		if (hasText) dl->AddText(ImVec2(min.x + pad + icon + gap, min.y + (h - text.y) * 0.5f), fgCol, label);
		ImGui::PopID();
		return clicked && enabled;
	}
	bool Section(const char* title, bool defaultOpen) {
		static std::map<std::string, bool> open;
		auto it = open.find(title);
		if (it == open.end()) it = open.emplace(title, defaultOpen).first;
		return solace::SectionHeader((std::string("##sec_") + title).c_str(), title, &it->second);
	}
	bool Seg(const char* id, int* value, std::initializer_list<std::pair<const char*, int>> items) {
		std::vector<const char*> labels;
		int current = 0, i = 0;
		for (const auto& it : items) { labels.push_back(it.first); if (it.second == *value) current = i; ++i; }
		const int next = solace::SegmentedControl(id, labels.data(), static_cast<int>(labels.size()), current);
		if (next != current && next >= 0 && next < static_cast<int>(labels.size())) { *value = (items.begin() + next)->second; return true; }
		return false;
	}
	void Toast(const std::string& text, ToastKind kind) {
		const solace::Status st = kind == ToastKind::Success ? solace::Status::Success : kind == ToastKind::Warning ? solace::Status::Warning :
			kind == ToastKind::Error ? solace::Status::Error : solace::Status::Info;
		solace::PushToast(text.c_str(), nullptr, st);
	}
	void Progress(float p, float width) { if (p < 0.0f) solace::IndeterminateProgressBar(width); else solace::ProgressBar(p, width); }
}

namespace {
	const char* kSettingsPath = "config/editor_settings.json";
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
	const char* kWinRace = "Race & Routes";
	const char* kWinDrive = "Drive";
	const char* kWinHud = "HUD";
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
	std::ifstream in(kSettingsPath);
	const json s = in ? json::parse(in, nullptr, false) : json();
	if (s.is_object()) {
		const json p = s.value("panels", json::object());
		auto rd = [&](const char* key, bool& v) { v = p.value(key, v); };
		rd("hierarchy", m_showHierarchy); rd("assets", m_showAssets); rd("inspector", m_showInspector);
		rd("materials", m_showMaterials); rd("console", m_showConsole); rd("build", m_showBuild);
		rd("race", m_showRace); rd("drive", m_showDrive); rd("hud", m_showHud); rd("rendering", m_showLegacyRendering);
		rd("lighting", m_showLegacyLighting); rd("performance", m_showLegacyPerformance); rd("animation", m_showLegacyAnimation);
		rd("state", m_showLegacyState); rd("camera", m_showLegacyCamera); rd("presets", m_showPresets);
		m_showAuthoring = s.value("authoring_overlay", m_showAuthoring);
		m_layoutBuilt = s.value("layout_built", false);
		m_selectedPreset = s.value("preset", m_selectedPreset);
	}
	std::ifstream pin(kPresetsPath);
	m_presets = pin ? json::parse(pin, nullptr, false) : json();
	if (!m_presets.is_object()) m_presets = json::object();
	// The layout ini may have been deleted: rebuild when no dock node exists yet.
	if (!fs::exists("config/editor_layout.ini")) m_layoutBuilt = false;
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
		{"console", m_showConsole}, {"build", m_showBuild}, {"race", m_showRace}, {"drive", m_showDrive}, {"hud", m_showHud},
		{"rendering", m_showLegacyRendering}, {"lighting", m_showLegacyLighting}, {"performance", m_showLegacyPerformance},
		{"animation", m_showLegacyAnimation}, {"state", m_showLegacyState}, {"camera", m_showLegacyCamera}, {"presets", m_showPresets} };
	s["authoring_overlay"] = m_showAuthoring;
	s["layout_built"] = m_layoutBuilt;
	s["preset"] = m_selectedPreset;
	std::ofstream out(kSettingsPath);
	if (out) out << s.dump(2);
}

void EditorWorkspace::OnSceneLoaded()
{
	m_undo.clear();
	m_redo.clear();
	m_handles.clear();
	m_dragHandle = -1;
	m_selectedCheckpoint = -1;
	m_selectedRoadNode = -1;
	m_gameEditActive = false;
	m_transformEditActive = false;
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
		else ImGui::TextDisabled("%s", m_core.IsPlaying() ? (m_core.IsPaused() ? "Paused" : "Playing - click the Game view to drive") : "Editing");
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
	TrackTransformEdits();

	DrawGameViewport();
	DrawSceneViewport();
	if (m_showHierarchy) DrawHierarchy();
	if (m_showAssets) DrawAssets();
	if (m_showInspector) DrawInspector();
	if (m_showMaterials) DrawMaterials();
	if (m_showConsole) DrawConsole();
	if (m_showBuild) DrawBuildOutput();
	if (m_showRace) DrawRaceTools();
	if (m_showDrive) DrawDriveTools();
	if (m_showHud) DrawHudTools();
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
	ImGui::DockBuilderDockWindow(kWinRace, bottom);
	ImGui::DockBuilderDockWindow(kWinDrive, bottom);
	ImGui::DockBuilderDockWindow(kWinHud, bottom);
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
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel != nullptr && !m_core.IsPlaying())) {
			if (auto copy = m_core.DuplicateNode(sel)) SelectNode(copy);
		}
		if (ImGui::MenuItem("Delete", "Del", false, sel != nullptr && !m_core.IsPlaying())) {
			m_core.DeleteNode(sel);
			SelectNode(nullptr);
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
		ImGui::MenuItem(kWinRace, nullptr, &m_showRace);
		ImGui::MenuItem(kWinDrive, nullptr, &m_showDrive);
		ImGui::MenuItem(kWinHud, nullptr, &m_showHud);
		ImGui::MenuItem("Animation", nullptr, &m_showLegacyAnimation);
		ImGui::MenuItem("State & Export", nullptr, &m_showLegacyState);
		ImGui::MenuItem(kWinPresets, nullptr, &m_showPresets);
		ImGui::Separator();
		ImGui::MenuItem("Authoring Overlay", nullptr, &m_showAuthoring);
		if (ImGui::MenuItem("Reset Layout")) {
			m_resetLayout = true;
			m_showHierarchy = m_showAssets = m_showInspector = m_showMaterials = m_showConsole = m_showBuild = true;
			m_showRace = m_showDrive = m_showHud = m_showLegacyRendering = m_showLegacyLighting = m_showLegacyPerformance = true;
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
	if (ui::ToolIcon("##save", RacingHUD::Get().IconId("editor/save"), solace::Icon::Save, "Save", false, !m_core.IsPlaying(), "Save scene (Ctrl+S)")) {
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
	if (ui::ToolIcon("##play", RacingHUD::Get().IconId("editor/play"), solace::Icon::Play, nullptr, playing, !playing, "Play (F5)")) {
		m_core.RequestPlay();
		FocusGame();
	}
	ImGui::SameLine();
	if (ui::ToolIcon("##pause", RacingHUD::Get().IconId("editor/pause"), solace::Icon::Pause, nullptr, m_core.IsPaused(), playing, "Pause (F6)")) m_core.TogglePause();
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
		if (GameSession* game = m_core.GetGame()) {
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(m_game.pos, ImVec2(m_game.pos.x + m_game.size.x, m_game.pos.y + m_game.size.y), true);
			game->DrawHUD(dl, glm::vec2(m_game.pos.x, m_game.pos.y), glm::vec2(m_game.size.x, m_game.size.y), m_dt);
			if (m_core.IsPlaying() && !m_game.focused) {
				const char* hint = "Click to drive";
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
	if (m_showAuthoring) DrawAuthoringOverlay(m_scene);
	DrawGizmo(m_scene);

	// Click selection (not on gizmo or authoring handles).
	if (m_scene.hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver() &&
		m_dragHandle < 0 && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0.0f && !m_core.IsEditorCameraActive()) {
		bool handleHit = false;
		for (const Handle& h : m_handles) {
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

void EditorWorkspace::DrawAuthoringOverlay(const Viewport& vp)
{
	GameSession* game = m_core.GetGame();
	if (!game || !game->IsAvailable()) return;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImGuiIO& io = ImGui::GetIO();
	m_handles.clear();
	const ImU32 roadCol = IM_COL32(120, 170, 255, 110);
	const RoadNetwork& roads = game->Roads();
	// Large generated networks (file-backed) are not drawn or edited as handles.
	static const std::vector<RoadNetwork::Edge> kNoEdges;
	for (const auto& e : roads.IsExternal() ? kNoEdges : roads.Edges()) {
		const glm::vec2 a = roads.Nodes()[e.a].pos, b = roads.Nodes()[e.b].pos;
		// Draw in short pieces so lines behind the camera are clipped correctly.
		const int pieces = std::max(1, static_cast<int>(glm::length(b - a) / 10.0f));
		for (int i = 0; i < pieces; ++i) {
			const glm::vec2 p0 = glm::mix(a, b, static_cast<float>(i) / pieces), p1 = glm::mix(a, b, static_cast<float>(i + 1) / pieces);
			ImVec2 s0, s1;
			if (ProjectToViewport(vp, glm::vec3(p0.x, 0.05f, p0.y), s0) && ProjectToViewport(vp, glm::vec3(p1.x, 0.05f, p1.y), s1)) dl->AddLine(s0, s1, roadCol, 1.5f);
		}
	}
	for (size_t i = 0; i < roads.Nodes().size() && !roads.IsExternal(); ++i) {
		const glm::vec2 p = roads.Nodes()[i].pos;
		m_handles.push_back({ Handle::Kind::RoadNode, static_cast<int>(i), -1, glm::vec3(p.x, 0.05f, p.y), roadCol });
	}
	// Spawn.
	const glm::vec2 sp = game->Drive().SpawnPosition();
	m_handles.push_back({ Handle::Kind::Spawn, -1, -1, glm::vec3(sp.x, 0.1f, sp.y), IM_COL32(90, 255, 140, 255) });
	{
		const float h = game->Drive().SpawnHeading();
		ImVec2 a, b;
		if (ProjectToViewport(vp, glm::vec3(sp.x, 0.1f, sp.y), a) && ProjectToViewport(vp, glm::vec3(sp.x + std::sin(h) * 4.0f, 0.1f, sp.y + std::cos(h) * 4.0f), b))
			dl->AddLine(a, b, IM_COL32(90, 255, 140, 255), 2.5f);
	}
	// Events, checkpoints and the route between them.
	auto& events = game->Events();
	for (size_t ei = 0; ei < events.size(); ++ei) {
		const auto& e = events[ei];
		const bool selected = static_cast<int>(ei) == game->SelectedEvent();
		const ImU32 evCol = IM_COL32(205, 160, 255, selected ? 255 : 150);
		m_handles.push_back({ Handle::Kind::EventStart, static_cast<int>(ei), -1, glm::vec3(e.start.x, 0.1f, e.start.y), evCol });
		ImVec2 s;
		if (ProjectToViewport(vp, glm::vec3(e.start.x, 0.1f, e.start.y), s)) dl->AddText(ImVec2(s.x + 8, s.y - 18), evCol, e.name.c_str());
		glm::vec2 prev = e.start;
		for (size_t ci = 0; ci < e.checkpoints.size(); ++ci) {
			const auto& c = e.checkpoints[ci];
			const bool last = ci + 1 == e.checkpoints.size();
			const ImU32 col = last ? IM_COL32(255, 230, 90, selected ? 255 : 140) : IM_COL32(80, 220, 255, selected ? 255 : 140);
			m_handles.push_back({ Handle::Kind::Checkpoint, static_cast<int>(ei), static_cast<int>(ci), glm::vec3(c.pos.x, 0.1f, c.pos.y), col });
			if (selected) {
				const float hd = game->GateHeading(e, static_cast<int>(ci));
				const glm::vec2 side(std::cos(hd), -std::sin(hd));
				ImVec2 g0, g1, g0t, g1t;
				const glm::vec2 a = c.pos - side * c.width * 0.5f, b = c.pos + side * c.width * 0.5f;
				if (ProjectToViewport(vp, glm::vec3(a.x, 0.1f, a.y), g0) && ProjectToViewport(vp, glm::vec3(b.x, 0.1f, b.y), g1) &&
					ProjectToViewport(vp, glm::vec3(a.x, 3.0f, a.y), g0t) && ProjectToViewport(vp, glm::vec3(b.x, 3.0f, b.y), g1t)) {
					dl->AddLine(g0, g1, col, 2.0f);
					dl->AddLine(g0, g0t, col, 2.0f);
					dl->AddLine(g1, g1t, col, 2.0f);
					dl->AddLine(g0t, g1t, col, 1.0f);
					char num[16];
					std::snprintf(num, sizeof(num), last ? "FINISH" : "CP %d", static_cast<int>(ci) + 1);
					dl->AddText(ImVec2((g0t.x + g1t.x) * 0.5f - 12, (g0t.y + g1t.y) * 0.5f - 16), col, num);
				}
				const std::vector<glm::vec2> route = roads.FindRoute(prev, c.pos);
				for (size_t k = 1; k < route.size(); ++k) {
					const int pieces = std::max(1, static_cast<int>(glm::length(route[k] - route[k - 1]) / 8.0f));
					for (int i = 0; i < pieces; ++i) {
						const glm::vec2 p0 = glm::mix(route[k - 1], route[k], static_cast<float>(i) / pieces), p1 = glm::mix(route[k - 1], route[k], static_cast<float>(i + 1) / pieces);
						ImVec2 s0, s1;
						if (ProjectToViewport(vp, glm::vec3(p0.x, 0.3f, p0.y), s0) && ProjectToViewport(vp, glm::vec3(p1.x, 0.3f, p1.y), s1)) dl->AddLine(s0, s1, IM_COL32(205, 160, 255, 200), 3.0f);
					}
				}
			}
			prev = c.pos;
		}
	}
	// Handles: draw, hover, drag on the ground plane (undoable as one game edit).
	int hovered = -1;
	for (size_t i = 0; i < m_handles.size(); ++i) {
		ImVec2 s;
		if (!ProjectToViewport(vp, m_handles[i].world, s)) continue;
		const bool isHover = vp.hovered && std::hypot(s.x - io.MousePos.x, s.y - io.MousePos.y) < 10.0f;
		if (isHover && hovered < 0) hovered = static_cast<int>(i);
		const float r = m_handles[i].kind == Handle::Kind::RoadNode ? 5.0f : 7.0f;
		dl->AddCircleFilled(s, r + (isHover || static_cast<int>(i) == m_dragHandle ? 2.0f : 0.0f), m_handles[i].color);
		dl->AddCircle(s, r + 2.0f, IM_COL32(0, 0, 0, 160), 0, 1.5f);
	}
	if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() && !m_core.IsPlaying()) {
		m_dragHandle = hovered;
		m_dragBefore = game->SaveGameJson();
		const Handle& h = m_handles[hovered];
		if (h.kind == Handle::Kind::EventStart || h.kind == Handle::Kind::Checkpoint) game->SetSelectedEvent(h.a);
		m_selectedCheckpoint = h.kind == Handle::Kind::Checkpoint ? h.b : -1;
		m_selectedRoadNode = h.kind == Handle::Kind::RoadNode ? h.a : -1;
		m_showRace = true;
	}
	if (m_dragHandle >= 0 && m_dragHandle < static_cast<int>(m_handles.size())) {
		const Handle h = m_handles[m_dragHandle];
		glm::vec3 ground;
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && ViewportRayToGround(vp, io.MousePos, ground)) {
			const glm::vec2 p(ground.x, ground.z);
			switch (h.kind) {
			case Handle::Kind::Spawn: game->Drive().SetSpawn(p, game->Drive().SpawnHeading()); game->Drive().ResetToSpawn(); break;
			case Handle::Kind::EventStart: game->Events()[h.a].start = p; break;
			case Handle::Kind::Checkpoint: game->Events()[h.a].checkpoints[h.b].pos = p; break;
			case Handle::Kind::RoadNode: game->Roads().MutableNodes()[h.a].pos = p; break;
			}
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			const json before = m_dragBefore, after = game->SaveGameJson();
			if (before != after) {
				Push("Move handle", [this, before] { m_core.ApplyGameJson(before); }, [this, after] { m_core.ApplyGameJson(after); });
				m_core.ApplyGameJson(after);
				m_core.MarkDirty();
			}
			m_dragHandle = -1;
		}
	}
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
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnDoubleClick;
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
		if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !m_core.IsPlaying())) { if (auto c = m_core.DuplicateNode(node)) SelectNode(c); }
		if (ImGui::MenuItem("Delete", "Del", false, !m_core.IsPlaying())) { m_core.DeleteNode(node); SelectNode(nullptr); ImGui::EndPopup(); if (open) ImGui::TreePop(); return; }
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
	if (auto graph = m_core.GetSceneGraph()) {
		if (auto root = graph->GetRoot()) {
			for (const auto& c : root->children) DrawHierarchyNode(c);
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

void EditorWorkspace::DrawAssets()
{
	if (!ImGui::Begin(kWinAssets, &m_showAssets)) { ImGui::End(); return; }
	static const char* folders[] = { "scenes", "models", "textures", "hdrs", "sounds", "fonts", "config", "shaders", "generated" };
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
		if (fs::exists(m_assetFolder, ec)) {
			for (auto it = fs::recursive_directory_iterator(m_assetFolder, fs::directory_options::skip_permission_denied, ec);
				it != fs::recursive_directory_iterator() && m_assetCache.size() < 4000; it.increment(ec)) {
				if (ec) break;
				if (it->is_regular_file(ec)) m_assetCache.push_back(it->path().generic_string());
			}
		}
		std::sort(m_assetCache.begin(), m_assetCache.end());
	}
	ImGui::BeginChild("##assets");
	for (const std::string& path : m_assetCache) {
		if (!ContainsNoCase(path, m_assetFilter)) continue;
		const std::string ext = fs::path(path).extension().string();
		ImGui::PushID(path.c_str());
		ImGui::Selectable(path.c_str());
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			if (ext == ".json" && m_assetFolder == "scenes") RequestScene(path);
		}
		if (ImGui::BeginPopupContextItem()) {
			if (ext == ".json" && m_assetFolder == "scenes" && ImGui::MenuItem("Open Scene")) RequestScene(path);
			if ((ext == ".glb" || ext == ".gltf" || ext == ".fbx") && ImGui::MenuItem("Add to Scene", nullptr, false, !m_core.IsPlaying())) {
				glm::vec3 pos(0.0f);
				if (auto cam = m_core.GetCamera()) pos = cam->GetCameraPosition() + cam->GetCameraFrontVector() * 8.0f;
				pos.y = 0.0f;
				if (auto n = m_core.AddModelNode(path, pos)) SelectNode(n);
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
	auto node = m_legacy.GetSelectedNode();
	if (!node) {
		ImGui::TextDisabled("Select a node in the Hierarchy or the Scene view.");
		ImGui::TextDisabled("Spawn, events and checkpoints are edited in Race & Routes / Drive.");
		ImGui::End();
		return;
	}
	char name[128];
	std::snprintf(name, sizeof(name), "%s", node->GetName().c_str());
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue)) {
		const std::string before = node->GetName(), after = name;
		std::weak_ptr<SceneNode> weak = node;
		node->SetName(after);
		Push("Rename", [weak, before] { if (auto n = weak.lock()) n->SetName(before); }, [weak, after] { if (auto n = weak.lock()) n->SetName(after); });
		m_core.MarkDirty();
	}
	if (ui::Section("Transform", true)) {
		glm::vec3 p = node->GetPosition();
		glm::vec3 s = node->GetScale();
		glm::vec3 e = glm::degrees(glm::eulerAngles(node->GetOrientation()));
		bool changed = false;
		ui::Grid("##xf");
		ui::Label("Position"); changed |= ImGui::DragFloat3("##p", &p.x, 0.05f);
		ui::Label("Rotation"); changed |= ImGui::DragFloat3("##r", &e.x, 0.5f);
		ui::Label("Scale"); changed |= ImGui::DragFloat3("##s", &s.x, 0.01f);
		ui::EndGrid();
		if (changed) {
			node->SetLocalTRS(p, glm::quat(glm::radians(e)), s);
			m_core.InvalidateSceneCaches();
		}
	}
	if (auto lightNode = std::dynamic_pointer_cast<LightNode>(node)) {
		if (auto light = lightNode->GetLight(); light && ui::Section("Light", true)) {
			glm::vec3 c = light->GetColor();
			float intensity = light->GetIntensity();
			float range = light->GetRange();
			bool enabled = light->IsEnabled();
			bool shadows = light->CastsShadows();
			ui::Grid("##light");
			ui::Label("Enabled"); if (ImGui::Checkbox("##en", &enabled)) { light->SetEnabled(enabled); m_core.MarkDirty(); }
			ui::Label("Color"); if (ImGui::ColorEdit3("##c", &c.x)) { light->SetColor(c); m_core.MarkDirty(); }
			ui::Label("Intensity"); if (ImGui::DragFloat("##i", &intensity, 0.01f, 0.0f, 1000.0f)) { light->SetIntensity(intensity); m_core.MarkDirty(); }
			ui::Label("Range"); if (ImGui::DragFloat("##rg", &range, 0.1f, 0.1f, 500.0f)) { light->SetRange(range); m_core.MarkDirty(); }
			ui::Label("Shadows"); if (ImGui::Checkbox("##sh", &shadows)) { light->SetCastsShadows(shadows); m_core.MarkDirty(); }
			ui::EndGrid();
		}
	}
	if (auto model = node->GetModel(); model && ui::Section("Model", true)) {
		ImGui::TextDisabled("%s", model->m_model_name.c_str());
		ImGui::Text("%d meshes", static_cast<int>(model->meshes.size()));
		bool exclude = model->excludeFromReflectionCapture;
		if (ImGui::Checkbox("Exclude from reflection capture", &exclude)) model->excludeFromReflectionCapture = exclude;
	}
	if (ui::Section("Info", false)) {
		ImGui::Text("Entity %u", node->GetEntityID());
		ImGui::Text("Children %d", static_cast<int>(node->children.size()));
		const glm::vec3 w = node->GetWorldPosition();
		ImGui::Text("World (%.2f, %.2f, %.2f)", w.x, w.y, w.z);
		if (node->IsGenerated()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "Generated: rebuilt on load, not saved");
	}
	ImGui::End();
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
		std::ofstream out(kPresetsPath);
		if (out) { out << m_presets.dump(2); ui::Toast("Launch presets saved", ui::ToastKind::Success); }
	}
	ImGui::SameLine();
	if (ui::Btn("Run selected", false)) RunStandalone();
	ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Game authoring panels
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::DrawRaceTools()
{
	if (!ImGui::Begin(kWinRace, &m_showRace)) { ImGui::End(); return; }
	GameSession* game = m_core.GetGame();
	if (!game || !game->IsAvailable()) {
		ImGui::TextDisabled("This scene has no \"game\" section (player car).");
		ImGui::End();
		return;
	}
	auto& events = game->Events();
	const bool editable = !m_core.IsPlaying();
	ImGui::BeginChild("##eventlist", ImVec2(220, 0), true);
	ImGui::TextDisabled("EVENTS");
	for (int i = 0; i < static_cast<int>(events.size()); ++i) {
		ImGui::PushID(i);
		if (ImGui::Selectable(events[i].name.c_str(), game->SelectedEvent() == i)) game->SetSelectedEvent(i);
		ImGui::PopID();
	}
	ImGui::BeginDisabled(!editable);
	if (ui::Btn("+ Event", false)) {
		GameSession::RaceEvent e;
		e.id = "event_" + std::to_string(events.size() + 1);
		e.name = "New Sprint";
		e.start = game->Drive().Position() + glm::vec2(std::sin(game->Drive().Heading()), std::cos(game->Drive().Heading())) * 20.0f;
		e.startHeadingDeg = glm::degrees(game->Drive().Heading());
		e.checkpoints.push_back({ e.start + glm::vec2(std::sin(game->Drive().Heading()), std::cos(game->Drive().Heading())) * 120.0f });
		events.push_back(e);
		game->SetSelectedEvent(static_cast<int>(events.size()) - 1);
		CommitGameEdit("Add event");
	}
	ImGui::SameLine();
	if (ui::Btn("Duplicate", false) && !events.empty()) {
		GameSession::RaceEvent e = events[game->SelectedEvent()];
		e.id += "_copy";
		e.name += " (copy)";
		e.start += glm::vec2(6.0f, 0.0f);
		events.push_back(e);
		game->SetSelectedEvent(static_cast<int>(events.size()) - 1);
		CommitGameEdit("Duplicate event");
	}
	ImGui::EndDisabled();
	ImGui::Spacing();
	ImGui::TextDisabled("SPEEDWALL");
	for (const auto& e : events) ImGui::Text("%s  %s", e.id.c_str(), FormatTime(game->BestTime(e.id)).c_str());
	if (ImGui::SmallButton("Clear best times")) game->ClearBestTimes();
	ImGui::EndChild();
	ImGui::SameLine();

	ImGui::BeginChild("##eventprops", ImVec2(0, 0), false);
	if (!events.empty()) {
		auto& e = events[std::clamp(game->SelectedEvent(), 0, static_cast<int>(events.size()) - 1)];
		ImGui::BeginDisabled(!editable);
		auto text = [&](const char* label, std::string& s) {
			char buf[128];
			std::snprintf(buf, sizeof(buf), "%s", s.c_str());
			ui::Label(label);
			if (ImGui::InputText((std::string("##") + label).c_str(), buf, sizeof(buf))) s = buf;
		};
		ui::Grid("##ev");
		text("Id", e.id);
		text("Type", e.type);
		text("Name", e.name);
		text("Difficulty", e.difficulty);
		ui::Label("Start (x, z)"); ImGui::DragFloat2("##st", &e.start.x, 0.25f);
		ui::Label("Start heading"); ImGui::DragFloat("##sh", &e.startHeadingDeg, 1.0f, -180.0f, 180.0f, "%.0f deg");
		ui::Label("Trigger radius"); ImGui::DragFloat("##tr", &e.triggerRadius, 0.1f, 2.0f, 40.0f, "%.1f m");
		ui::Label("Countdown"); ImGui::DragFloat("##cd", &e.countdown, 0.1f, 1.0f, 10.0f, "%.1f s");
		ui::EndGrid();
		if (ui::Btn("Start = car position", false)) {
			e.start = game->Drive().Position();
			e.startHeadingDeg = glm::degrees(game->Drive().Heading());
			CommitGameEdit("Set event start");
		}
		ImGui::SameLine();
		if (ui::Btn("Delete event", false)) {
			events.erase(events.begin() + game->SelectedEvent());
			game->SetSelectedEvent(0);
			CommitGameEdit("Delete event");
			ImGui::EndDisabled();
			ImGui::EndChild();
			ImGui::End();
			return;
		}
		ImGui::Separator();
		ImGui::TextDisabled("CHECKPOINTS (ordered; the last one is the finish)");
		if (ImGui::BeginTable("##cps", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28);
			ImGui::TableSetupColumn("Position (x, z)");
			ImGui::TableSetupColumn("Width");
			ImGui::TableSetupColumn("Heading");
			ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 56);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableHeadersRow();
			int remove = -1, moveUp = -1, moveDown = -1, dup = -1;
			for (int i = 0; i < static_cast<int>(e.checkpoints.size()); ++i) {
				auto& c = e.checkpoints[i];
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				if (ImGui::Selectable(std::to_string(i + 1).c_str(), m_selectedCheckpoint == i)) m_selectedCheckpoint = i;
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::DragFloat2("##p", &c.pos.x, 0.25f);
				ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); ImGui::DragFloat("##w", &c.width, 0.1f, 4.0f, 60.0f, "%.1f m");
				ImGui::TableNextColumn();
				ImGui::Checkbox("auto", &c.autoHeading);
				if (!c.autoHeading) { ImGui::SameLine(); ImGui::SetNextItemWidth(-1); ImGui::DragFloat("##h", &c.headingDeg, 1.0f, -180.0f, 180.0f, "%.0f"); }
				ImGui::TableNextColumn();
				if (ImGui::ArrowButton("##up", ImGuiDir_Up)) moveUp = i;
				ImGui::SameLine();
				if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) moveDown = i;
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("dup")) dup = i;
				ImGui::SameLine();
				if (ImGui::SmallButton("del")) remove = i;
				ImGui::PopID();
			}
			ImGui::EndTable();
			if (moveUp > 0) { std::swap(e.checkpoints[moveUp], e.checkpoints[moveUp - 1]); CommitGameEdit("Reorder checkpoint"); }
			if (moveDown >= 0 && moveDown + 1 < static_cast<int>(e.checkpoints.size())) { std::swap(e.checkpoints[moveDown], e.checkpoints[moveDown + 1]); CommitGameEdit("Reorder checkpoint"); }
			if (dup >= 0) { auto c = e.checkpoints[dup]; c.pos += glm::vec2(0, 10); e.checkpoints.insert(e.checkpoints.begin() + dup + 1, c); CommitGameEdit("Duplicate checkpoint"); }
			if (remove >= 0 && e.checkpoints.size() > 1) { e.checkpoints.erase(e.checkpoints.begin() + remove); CommitGameEdit("Delete checkpoint"); }
		}
		if (ui::Btn("+ Checkpoint at car", false)) {
			e.checkpoints.push_back({ game->Drive().Position() });
			CommitGameEdit("Add checkpoint");
		}
		ImGui::EndDisabled();
		ImGui::Separator();
		ImGui::TextDisabled("TEST");
		if (ui::Btn(m_core.IsPlaying() ? "Restart event" : "Play this event", true)) {
			if (!m_core.IsPlaying()) { m_core.RequestPlay(); FocusGame(); }
			game->StartEvent(game->SelectedEvent());
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Phase: %s  time %s  next CP %d", game->GetPhase() == GameSession::Phase::Racing ? "racing" :
			game->GetPhase() == GameSession::Phase::Countdown ? "countdown" : game->GetPhase() == GameSession::Phase::Finished ? "finished" : "free roam",
			FormatTime(game->RaceTime()).c_str(), game->NextCheckpoint() + 1);
	}
	ImGui::Separator();
	if (ui::Section("Road network", false)) {
		RoadNetwork& roads = game->Roads();
		ImGui::BeginDisabled(!editable);
		float hw = roads.DefaultHalfWidth(), sw = roads.SidewalkWidth();
		ui::Grid("##roads");
		ui::Label("Road half width"); if (ImGui::DragFloat("##hw", &hw, 0.05f, 2.0f, 15.0f, "%.2f m")) roads.SetDefaultHalfWidth(hw);
		ui::Label("Sidewalk width"); if (ImGui::DragFloat("##sw", &sw, 0.05f, 0.0f, 10.0f, "%.2f m")) roads.SetSidewalkWidth(sw);
		ui::EndGrid();
		if (roads.IsExternal()) {
			ImGui::TextDisabled("Generated network: %s (read-only)", roads.SourceFile().c_str());
		}
		for (size_t i = 0; i < roads.Nodes().size() && !roads.IsExternal(); ++i) {
			auto& n = roads.MutableNodes()[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::SetNextItemWidth(220);
			ImGui::DragFloat2(n.id.c_str(), &n.pos.x, 0.5f);
			ImGui::PopID();
		}
		ImGui::TextDisabled("%d streets. Moving nodes updates routes, minimap and collision immediately;", static_cast<int>(roads.Edges().size()));
		ImGui::TextDisabled("Save + reopen the scene to regenerate the district geometry.");
		ImGui::EndDisabled();
	}
	ImGui::EndChild();
	ImGui::End();
}

void EditorWorkspace::DrawDriveTools()
{
	if (!ImGui::Begin(kWinDrive, &m_showDrive)) { ImGui::End(); return; }
	GameSession* game = m_core.GetGame();
	if (!game || !game->IsAvailable()) {
		ImGui::TextDisabled("No drivable car in this scene.");
		ImGui::End();
		return;
	}
	DriveMode& d = game->Drive();
	if (m_core.IsPlaying()) {
		solace::Badge("RUNTIME", solace::Status::Warning);
		ImGui::SameLine();
		ImGui::TextDisabled("Changes made while playing are discarded on Stop unless applied.");
		ImGui::SameLine();
		if (ui::Btn("Apply to authored", true)) {
			m_core.ApplyRuntimeChangesToAuthored();
			ui::Toast("Runtime tuning applied to the scene (save to keep)", ui::ToastKind::Success);
		}
	}
	ImGui::Columns(3, "##drivecols", false);
	DriveMode::Handling& h = d.MutableHandling();
	ImGui::TextDisabled("HANDLING");
	ui::Grid("##hand");
	ui::Label("Top speed"); ImGui::DragFloat("##ts", &h.topSpeedKmh, 1.0f, 60.0f, 400.0f, "%.0f km/h");
	ui::Label("Acceleration"); ImGui::DragFloat("##ac", &h.acceleration, 0.1f, 2.0f, 30.0f, "%.1f m/s2");
	ui::Label("Braking"); ImGui::DragFloat("##br", &h.braking, 0.1f, 4.0f, 40.0f, "%.1f m/s2");
	ui::Label("Steer lock low"); ImGui::DragFloat("##sl", &h.steerLockLow, 0.2f, 10.0f, 50.0f, "%.1f deg");
	ui::Label("Steer lock high"); ImGui::DragFloat("##sh", &h.steerLockHigh, 0.1f, 1.0f, 20.0f, "%.1f deg");
	ui::Label("Steer rate"); ImGui::DragFloat("##sr", &h.steerRate, 0.1f, 1.0f, 30.0f);
	ui::Label("Grip"); ImGui::DragFloat("##gr", &h.grip, 0.05f, 0.5f, 20.0f);
	ui::Label("Drift grip"); ImGui::DragFloat("##dg", &h.driftGrip, 0.05f, 0.1f, 10.0f);
	ui::Label("Drift yaw gain"); ImGui::DragFloat("##dy", &h.driftYawGain, 0.02f, 1.0f, 4.0f);
	ui::Label("Redline"); ImGui::DragFloat("##rl", &h.rpmRedline, 10.0f, 4000.0f, 9800.0f, "%.0f rpm");
	ui::Label("Limiter"); ImGui::DragFloat("##lm", &h.rpmLimiter, 10.0f, 4500.0f, 10000.0f, "%.0f rpm");
	ui::EndGrid();
	ImGui::NextColumn();
	ImGui::TextDisabled("NITROUS");
	ui::Grid("##nos");
	ui::Label("Thrust x"); ImGui::DragFloat("##nt", &h.nitroThrust, 0.01f, 1.0f, 3.0f);
	ui::Label("Top speed x"); ImGui::DragFloat("##nv", &h.nitroTopSpeed, 0.01f, 1.0f, 1.5f);
	ui::Label("Drain /s"); ImGui::DragFloat("##nd", &h.nitroDrain, 0.005f, 0.02f, 1.0f);
	ui::Label("Recharge /s"); ImGui::DragFloat("##nr", &h.nitroRecharge, 0.005f, 0.0f, 1.0f);
	ui::Label("Recharge delay"); ImGui::DragFloat("##ny", &h.nitroRechargeDelay, 0.05f, 0.0f, 10.0f, "%.2f s");
	ui::EndGrid();
	ImGui::TextDisabled("SPAWN");
	glm::vec2 sp = d.SpawnPosition();
	float sh = glm::degrees(d.SpawnHeading());
	ui::Grid("##spawn");
	ui::Label("Position (x, z)");
	bool spawnChanged = ImGui::DragFloat2("##sp", &sp.x, 0.1f);
	ui::Label("Heading"); spawnChanged |= ImGui::DragFloat("##sph", &sh, 1.0f, -180.0f, 180.0f, "%.0f deg");
	ui::EndGrid();
	if (spawnChanged) { d.SetSpawn(sp, glm::radians(sh)); if (!m_core.IsPlaying()) d.ResetToSpawn(); }
	if (ui::Btn("Spawn = car", false)) { d.SetSpawn(d.Position(), d.Heading()); CommitGameEdit("Set spawn"); }
	ImGui::NextColumn();
	ImGui::TextDisabled("CHASE CAMERA");
	DriveMode::CameraRig& c = d.MutableCamera();
	ui::Grid("##cam");
	ui::Label("Distance"); ImGui::DragFloat("##cdist", &c.distance, 0.02f, 2.0f, 12.0f, "%.2f m");
	ui::Label("Height"); ImGui::DragFloat("##chei", &c.height, 0.01f, 0.4f, 5.0f, "%.2f m");
	ui::Label("Look ahead"); ImGui::DragFloat("##cla", &c.lookAhead, 0.05f, 0.0f, 20.0f, "%.2f m");
	ui::Label("Look height"); ImGui::DragFloat("##clh", &c.lookHeight, 0.01f, 0.0f, 3.0f, "%.2f m");
	ui::Label("FOV (vertical)"); ImGui::DragFloat("##cfov", &c.fov, 0.1f, 25.0f, 90.0f, "%.1f deg");
	ui::Label("FOV speed gain"); ImGui::DragFloat("##cfs", &c.fovSpeedGain, 0.1f, 0.0f, 30.0f);
	ui::Label("FOV nitrous"); ImGui::DragFloat("##cfn", &c.fovNitro, 0.1f, 0.0f, 20.0f);
	ui::Label("Pull back"); ImGui::DragFloat("##cpb", &c.pullBack, 0.01f, 0.0f, 4.0f, "%.2f m");
	ui::Label("Stiffness"); ImGui::DragFloat("##cst", &c.stiffness, 0.1f, 1.0f, 30.0f);
	ui::Label("Heading lag"); ImGui::DragFloat("##chl", &c.headingLag, 0.05f, 0.5f, 20.0f);
	ui::EndGrid();
	ImGui::TextDisabled("HEADLIGHTS");
	if (auto graph = m_core.GetSceneGraph()) {
		for (const char* n : { "Headlight_L", "Headlight_R" }) {
			std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<SceneNode>&)> find = [&](const std::shared_ptr<SceneNode>& s) -> std::shared_ptr<SceneNode> {
				if (!s) return nullptr;
				if (s->GetName() == n) return s;
				for (const auto& ch : s->children) if (auto f = find(ch)) return f;
				return nullptr;
			};
			if (auto ln = std::dynamic_pointer_cast<LightNode>(find(graph->GetRoot()))) {
				if (auto light = ln->GetLight()) {
					bool on = light->IsEnabled();
					float in = light->GetIntensity();
					ImGui::PushID(n);
					if (ImGui::Checkbox(n, &on)) { light->SetEnabled(on); m_core.MarkDirty(); }
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-1);
					if (ImGui::DragFloat("##int", &in, 0.01f, 0.0f, 10.0f, "x%.2f")) { light->SetIntensity(in); m_core.MarkDirty(); }
					ImGui::PopID();
				}
			}
		}
	}
	ImGui::Columns(1);
	ImGui::Separator();
	ImGui::Text("Live: %.0f km/h  gear %d  %.0f rpm  load %.2f  N2O %.0f%%", d.SpeedKmh(), d.Gear(), d.Rpm(), d.Load(), d.Nitro() * 100.0f);
	ImGui::End();
}

void EditorWorkspace::DrawHudTools()
{
	if (!ImGui::Begin(kWinHud, &m_showHud)) { ImGui::End(); return; }
	ImGui::TextDisabled("Racing HUD and minimap (%s). Edits apply live in the Game view.", RacingHUD::Get().ConfigPath().c_str());
	RacingHUD::Get().DrawEditorPanel();
	ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Modals, shortcuts, undo
// ---------------------------------------------------------------------------------------------
void EditorWorkspace::DrawModals()
{
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
	if (GameInputFocus()) return; // keys belong to the car
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !m_core.IsPlaying()) { m_core.SaveCurrentScene(); ui::Toast("Scene saved", ui::ToastKind::Success); }
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Undo();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
	auto sel = m_legacy.GetSelectedNode();
	if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && sel && !m_core.IsPlaying()) { if (auto c = m_core.DuplicateNode(sel)) SelectNode(c); }
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && sel && !m_core.IsPlaying() && (m_scene.focused || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))) {
		if (m_scene.focused) { m_core.DeleteNode(sel); SelectNode(nullptr); }
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

void EditorWorkspace::CommitGameEdit(const std::string& label)
{
	GameSession* game = m_core.GetGame();
	if (!game) return;
	const json before = m_gameSnapshot, after = game->SaveGameJson();
	if (before.is_null() || before == after) return;
	Push(label, [this, before] { m_core.ApplyGameJson(before); }, [this, after] { m_core.ApplyGameJson(after); });
	m_core.ApplyGameJson(after);
	m_gameSnapshot = after;
	m_core.MarkDirty();
}

void EditorWorkspace::TrackGameEdits()
{
	// Widgets edit the live game data; an edit becomes one undo step when the widget is released.
	GameSession* game = m_core.GetGame();
	if (!game || m_core.IsPlaying()) { m_gameSnapshot = json(); return; }
	const bool active = ImGui::IsAnyItemActive();
	if (!active && m_gameEditActive) {
		CommitGameEdit("Edit game data");
	}
	if (!active || m_gameSnapshot.is_null()) {
		if (!active) m_gameSnapshot = game->SaveGameJson();
	}
	m_gameEditActive = active;
}

void EditorWorkspace::TrackTransformEdits()
{
	auto node = m_legacy.GetSelectedNode();
	const bool active = ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing();
	if (m_transformEditActive && !active) {
		if (auto n = m_transformNode.lock()) {
			const auto before = m_transformSnapshot, after = CaptureTRS(n);
			if (before != after) {
				std::weak_ptr<SceneNode> weak = n;
				Push("Transform " + n->GetName(), [weak, before] { if (auto x = weak.lock()) ApplyTRS(x, before); },
					[weak, after] { if (auto x = weak.lock()) ApplyTRS(x, after); });
				m_core.MarkDirty();
			}
		}
	}
	if (!active && node) {
		m_transformNode = node;
		m_transformSnapshot = CaptureTRS(node);
	}
	m_transformEditActive = active;
}

// NOX_EDITOR_SMOKE=1: scripted editor checks for unattended validation (undo/redo of a transform
// edit, Play/Stop restoring the pre-play state, viewport resize, save). Results go to the console.
void EditorWorkspace::RunSmokeScript()
{
	static const bool enabled = [] { const char* v = std::getenv("NOX_EDITOR_SMOKE"); return v && v[0] == '1'; }();
	if (!enabled) return;
	static int frame = 0;
	static std::shared_ptr<SceneNode> car;
	static glm::vec3 original(0.0f);
	static int texW0 = 0;
	++frame;
	auto check = [](bool ok, const char* what) { std::cout << "[Smoke] " << (ok ? "PASS " : "FAIL ") << what << std::endl; };
	auto graph = m_core.GetSceneGraph();
	if (frame == 60 && graph) {
		std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<SceneNode>&)> find = [&](const std::shared_ptr<SceneNode>& n) -> std::shared_ptr<SceneNode> {
			if (!n) return nullptr;
			if (n->GetName() == "StreetKit") return n;
			for (const auto& c : n->children) if (auto f = find(c)) return f;
			return nullptr;
		};
		car = find(graph->GetRoot());
		check(car != nullptr, "select node");
		if (car) {
			SelectNode(car);
			original = car->GetPosition();
			const auto before = CaptureTRS(car);
			car->SetLocalTRS(original + glm::vec3(0.0f, 0.0f, 0.0f), car->GetOrientation(), car->GetScale() * 1.0f);
			ApplyTRS(car, { original.x + 0.5f, original.y, original.z, before[3], before[4], before[5], before[6], before[7], before[8], before[9] });
			const auto after = CaptureTRS(car);
			std::weak_ptr<SceneNode> weak = car;
			Push("Smoke transform", [weak, before] { if (auto n = weak.lock()) ApplyTRS(n, before); }, [weak, after] { if (auto n = weak.lock()) ApplyTRS(n, after); });
			m_core.MarkDirty();
			check(m_core.IsDirty(), "dirty after edit");
		}
	}
	if (frame == 70 && car) {
		Undo();
		check(glm::length(car->GetPosition() - original) < 1e-4f, "undo restores transform");
		Redo();
		check(std::abs(car->GetPosition().x - original.x - 0.5f) < 1e-4f, "redo reapplies transform");
		Undo();
	}
	if (frame == 90) {
		texW0 = m_game.texWidth;
		m_core.RequestPlay();
		FocusGame();
		check(m_core.IsPlaying(), "play");
	}
	if (frame == 120) {
		extern SDL_Window* NoxMainWindow();
		if (SDL_Window* w = NoxMainWindow()) SDL_SetWindowSize(w, 1600, 900);
	}
	if (frame == 300) {
		check(m_game.texWidth != texW0 && m_game.texWidth > 0, "viewport framebuffer follows window resize");
		GameSession* game = m_core.GetGame();
		const glm::vec2 moved = game ? game->Drive().Position() : glm::vec2(0.0f);
		m_core.RequestStop();
		check(!m_core.IsPlaying(), "stop");
		if (game) {
			check(glm::length(game->Drive().Position() - game->Drive().SpawnPosition()) < 0.01f, "stop restores car to spawn");
			std::cout << "[Smoke] car travelled to (" << moved.x << ", " << moved.y << ") during play" << std::endl;
		}
	}
	if (frame == 330) {
		const std::string path = m_core.GetCurrentScenePath();
		std::ifstream in(path);
		const json before = json::parse(in, nullptr, false);
		in.close();
		m_core.SaveCurrentScene();
		std::ifstream in2(path);
		const json after = json::parse(in2, nullptr, false);
		check(after.is_object() && after.contains("game") && after.contains("nodes"), "save writes a valid scene");
		check(before.is_object() && after["game"]["events"].size() == before["game"]["events"].size(), "save keeps events");
		check(!m_core.IsDirty(), "clean after save");
	}
	if (frame == 360) m_quit = true;
}
