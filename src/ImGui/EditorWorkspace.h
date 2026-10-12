#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <IMGUI/imgui.h>
#include <map>
#include "rhi/RHI.h"
#include "../json.hpp"
#include "../EditorExtensions.h"

class Core;
class SceneNode;
class ImGuiInterface;
class ProcessRunner;

/*
* Docked editor workspace (docs/nfs2015/game.md, "Editor").
*
* Dear ImGui docking layout: Scene / Game viewports in the centre, Hierarchy + Assets on the left,
* Inspector / Materials / Lighting / Rendering on the right, Console / Build Output / Profiler /
* Race & Routes / Drive / HUD at the bottom, toolbar with scene selection, Save, Play, Pause, Stop,
* Build, Run Standalone and Package. Layout persists in config/editor_layout.ini; Window > Reset
* Layout rebuilds the default arrangement.
*
* The renderer draws one viewport per frame into an offscreen texture (PresentTarget): the focused
* viewport, else Game while playing, else Scene. Viewport content bounds and framebuffer size drive
* the camera aspect, picking, gizmos, authoring handles and the racing HUD (which is clipped to the
* Game viewport).
*/
class EditorWorkspace : public EditorHost {
public:
	struct Viewport {
		const char* name = "";
		rhi::FramebufferHandle fbo;
		rhi::TextureHandle color;
		rhi::TextureHandle depth;   // depth-stencil target (D24S8)
		int texWidth = 0, texHeight = 0;     // framebuffer pixels
		ImVec2 pos{ 0, 0 }, size{ 0, 0 };     // content rect in ImGui coordinates
		bool visible = false, hovered = false, focused = false;
		bool rendered = false;                // texture holds a frame
	};
	enum class Target { None, Scene, Game };

	EditorWorkspace(Core& core, ImGuiInterface& legacyUi);
	~EditorWorkspace();

	void Draw(float dt);
	// Which viewport the renderer should draw this frame, with its framebuffer and pixel size.
	Target RenderTarget() const { return m_renderTarget; }
	Viewport& GetViewport(Target t) { return t == Target::Game ? m_game : m_scene; }
	bool GameInputFocus() const;          // keyboard/pad belong to the game
	bool WantsQuit() const { return m_quit; }
	void FocusGame();
	void OnSceneLoaded();
	void SaveSettings() const;

	// EditorHost (panels and overlays of the registered application extensions).
	bool IsPlaying() const override;
	void RequestPlay() override;
	void RequestStop() override;
	void MarkDirty() override;
	void PushUndo(const std::string& label, std::function<void()> undo, std::function<void()> redo) override;
	void ApplyGameJson(const nlohmann::json& game) override;
	nlohmann::json GetAuthoredGameJson() const override;
	void CommitGameEdit(const std::string& label) override;
	void ApplyRuntimeChangesToAuthored() override;
	IGameModule* Game() override;
	SceneGraph* GetSceneGraph() override;
	ModularRenderer* GetRenderer() override;
	bool ProjectToViewport(const glm::vec3& world, ImVec2& out) const override;   // scene viewport
	void AddViewportHandle(const ViewportHandle& handle) override { m_handles.push_back(handle); }
	void ShowPanel(const std::string& id) override;

private:
	struct UndoAction { std::string label; std::function<void()> undo, redo; };

	bool& PanelOpen(const EditorPanel& panel);
	void BuildDefaultLayout(unsigned int dockspaceId);
	void DrawMenuBar();
	void DrawToolbar();
	void DrawSceneViewport();
	void DrawGameViewport();
	void DrawViewportImage(Viewport& vp);
	void DrawHierarchy();
	void DrawHierarchyNode(const std::shared_ptr<SceneNode>& node);
	void DrawAssets();
	void DrawInspector();
	void DrawMaterials();
	void DrawConsole();
	void DrawBuildOutput();
	void DrawLaunchPresets();
	void DrawModals();
	void DrawAuthoringOverlay(ImDrawList* dl);   // registered viewport overlay + its handles
	void DrawGizmo(const Viewport& vp);
	void HandleShortcuts();
	void EnsureTarget(Viewport& vp, int width, int height);
	void ChooseRenderTarget();
	void RunSmokeScript();

	// Undo / redo.
	void Push(const std::string& label, std::function<void()> undo, std::function<void()> redo);
	void Undo();
	void Redo();
	void TrackGameEdits();               // snapshot game JSON around widget edits
	void TrackTransformEdits();

	// Build / run / package.
	void StartTool(const std::string& title, const std::string& script, const std::string& args);
	void RunStandalone();

	bool ProjectToViewport(const Viewport& vp, const glm::vec3& world, ImVec2& out) const;   // scene viewport: see EditorHost
	bool ViewportRayToGround(const Viewport& vp, const ImVec2& mouse, glm::vec3& out) const;
	void SelectNode(const std::shared_ptr<SceneNode>& node);
	void RequestScene(const std::string& path);

	Core& m_core;
	ImGuiInterface& m_legacy;
	Viewport m_scene{ "Scene" };
	Viewport m_game{ "Game" };
	Target m_renderTarget = Target::Scene;
	Target m_lastFocused = Target::Scene;
	bool m_layoutBuilt = false;
	bool m_resetLayout = false;
	bool m_quit = false;
	bool m_focusGameNextFrame = false;

	// Panels.
	struct PanelToggle { const char* name; bool* open; };
	bool m_showHierarchy = true, m_showAssets = true, m_showInspector = true, m_showMaterials = true;
	bool m_showConsole = true, m_showBuild = true;
	// Extension panels: open state by panel id (persisted under "extension_panels").
	std::map<std::string, bool> m_extOpen;
	nlohmann::json m_extSaved = nlohmann::json::object();
	bool m_showPresets = false, m_showHelp = false, m_showLegacyRendering = true, m_showLegacyLighting = true;
	bool m_showLegacyPerformance = true, m_showLegacyAnimation = false, m_showLegacyState = false, m_showLegacyCamera = false;
	bool m_showDemo = false;

	// Hierarchy / assets.
	char m_hierarchyFilter[128] = "";
	char m_assetFilter[128] = "";
	std::string m_assetFolder = "scenes";
	std::vector<std::string> m_assetCache;
	float m_assetCacheTimer = 0.0f;

	// Console.
	std::vector<std::pair<std::string, int>> m_consoleLines;
	size_t m_consoleCursor = 0;
	char m_consoleFilter[128] = "";
	bool m_consoleShowInfo = true, m_consoleShowWarn = true, m_consoleShowError = true;
	bool m_consoleAutoScroll = true;

	// Authoring overlay / handles.
	bool m_showAuthoring = true;
	int m_dragHandle = -1;
	std::vector<ViewportHandle> m_handles;
	nlohmann::json m_dragBefore;

	// Gizmo.
	int m_gizmoOp = 7;   // ImGuizmo::TRANSLATE
	int m_gizmoMode = 0; // LOCAL
	bool m_snap = false;
	float m_snapValues[3] = { 0.5f, 15.0f, 0.1f };
	bool m_wasUsingGizmo = false;
	std::array<float, 10> m_gizmoBefore{};

	// Undo.
	std::vector<UndoAction> m_undo, m_redo;
	nlohmann::json m_gameSnapshot;
	bool m_gameEditActive = false;
	std::weak_ptr<SceneNode> m_transformNode;
	std::array<float, 10> m_transformSnapshot{};
	bool m_transformEditActive = false;

	// Modals.
	std::string m_pendingScene;
	bool m_openUnsavedModal = false;
	std::string m_status;
	float m_statusTimer = 0.0f;

	// Tools.
	std::unique_ptr<ProcessRunner> m_runner;
	std::vector<std::string> m_buildLines;
	size_t m_buildCursor = 0;
	nlohmann::json m_presets;
	std::string m_selectedPreset = "game";
	float m_fpsSmoothed = 0.0f;
	float m_dt = 0.0f;
};
