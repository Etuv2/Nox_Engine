#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <IMGUI/imgui.h>
#include <map>
#include "../reflect/Reflection.h"
#include "rhi/RHI.h"
#include "../editor/InspectorUndo.h"
#include "../editor/HierarchyModel.h"
#include "../json.hpp"
#include "../EditorExtensions.h"

class Core;
class SceneNode;
class ImGuiInterface;
class ProcessRunner;
namespace nox::content { class ContentBrowser; }

/*
* Docked editor workspace (docs/nfs2015/game.md, "Editor").
*
* Dear ImGui docking layout: Scene / Game viewports in the centre, Hierarchy + Assets on the left,
* Inspector / Materials / Lighting / Rendering on the right, Console / Build Output / Profiler and
* application panels at the bottom, toolbar with scene selection, Save, Play, Pause, Stop,
* Build, Run Standalone and Package. Layout persists in config/editor_layout.ini; Window > Reset
* Layout rebuilds the default arrangement.
*
* The renderer draws one viewport per frame into an offscreen texture (PresentTarget): the focused
* viewport, else Game while playing, else Scene. Viewport content bounds and framebuffer size drive
* the camera aspect, picking, gizmos, authoring handles and game overlays (clipped to the Game
* viewport).
*
* The inspector is generic: every component registered in the reflection registry (reflect/Reflection.h)
* is drawn from its property descriptors, and every edit goes through the undo helpers in editor/InspectorUndo.h.
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

	// Selection outline style (Scene viewport "Outline" popup; persisted under "selection_outline").
	struct SelectionOutlineSettings {
		float color[3] = { 1.0f, 140.0f / 255.0f, 26.0f / 255.0f };   // #FF8C1A
		int width = 2;            // pixels, 1..4
		bool showOccluded = true; // parts behind other geometry, drawn dashed
		bool hover = true;        // thinner highlight under the mouse
	};

	EditorWorkspace(Core& core, ImGuiInterface& legacyUi);
	~EditorWorkspace();

	void Draw(float dt);
	// Which viewport the renderer should draw this frame, with its framebuffer and pixel size.
	Target RenderTarget() const { return m_renderTarget; }
	Viewport& GetViewport(Target t) { return t == Target::Game ? m_game : m_scene; }
	bool GameInputFocus() const;          // keyboard/pad belong to the game
	bool WantsQuit() const { return m_quit; }
	const SelectionOutlineSettings& OutlineSettings() const { return m_outline; }
	std::shared_ptr<SceneNode> HoveredNode() const { return m_hoverNode.lock(); }   // null when hover is off
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
	void DrawHierarchyRow(const nox::editor::HierarchyRow& row);
	void DrawAssets();
	void DrawInspector();
	void DrawComponentSection(const std::shared_ptr<SceneNode>& node, const nox::reflect::ComponentDesc& desc,
		const std::vector<std::string>& assets, const std::vector<std::string>& nodeNames);
	void DrawAddComponentButton(const std::shared_ptr<SceneNode>& node);
	void DrawPlayBanner();
	void DrawAddNodeItems();
	void AddNodeFromTemplate(const std::string& kind);
	// Models and prefabs from assets. at = world point (ground hit under the cursor) or null for in front of the camera.
	void AddModelFromAsset(const std::string& path, const std::shared_ptr<SceneNode>& parent, const glm::vec3* at = nullptr);
	bool PlaceAssetInScene(const std::string& path, const std::shared_ptr<SceneNode>& parent, const glm::vec3* at);
	void DuplicateNodeWithUndo(const std::shared_ptr<SceneNode>& node);
	void DeleteNodeWithUndo(const std::shared_ptr<SceneNode>& node);
	// index < 0 appends to the parent; otherwise the node lands at that position among the parent's other children.
	void ReparentWithUndo(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<SceneNode>& parent, int index = -1);
	// Moves node next to target (before it, or after it when after is true) under target's parent.
	void MoveNodeNextTo(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<SceneNode>& target, bool after);
	// Folders: new empty folder under parent (null = scene root), and grouping a node into a new folder in its slot.
	void CreateFolderUnder(const std::shared_ptr<SceneNode>& parent);
	void GroupIntoFolderWithUndo(const std::shared_ptr<SceneNode>& node);
	nlohmann::json HierarchySettingsJson() const;
	void BeginRename(const std::shared_ptr<SceneNode>& node);
	// Prefabs: save a subtree as a *.prefab.json asset, instantiate an asset, unpack an instance (undoable).
	void BeginSavePrefab(const std::shared_ptr<SceneNode>& node);
	void InstantiatePrefabFromAsset(const std::string& path, const std::shared_ptr<SceneNode>& parent, const glm::vec3* at = nullptr);
	void UnpackPrefabWithUndo(const std::shared_ptr<SceneNode>& node);
	void InvokeEditorButton(const std::shared_ptr<SceneNode>& node, const nox::reflect::ComponentDesc& desc,
		const nox::reflect::MethodDesc& method);
	nox::editor::UndoPush UndoSink();                 // null while playing (no undo entries)
	nox::editor::NodeOps CoreNodeOps();
	std::shared_ptr<SceneNode> RootNode() const;
	void NoteEdit();                                  // dirty flag + cache invalidation after an edit
	void DrawMaterials();
	void DrawConsole();
	void DrawBuildOutput();
	void DrawLaunchPresets();
	void DrawPreferences();
	void DrawModals();
	void DrawAuthoringOverlay(ImDrawList* dl);   // registered viewport overlay + its handles
	void DrawGizmo(const Viewport& vp);
	void DrawOutlineStylePopup();
	void UpdateHover(const ImGuiIO& io);
	void DrawSelectionBounds(ImDrawList* dl);
	void HandleShortcuts();
	void EnsureTarget(Viewport& vp, int width, int height);
	void ChooseRenderTarget();
	void RunSmokeScript();

	// Undo / redo.
	void Push(const std::string& label, std::function<void()> undo, std::function<void()> redo);
	void Undo();
	void Redo();
	void TrackGameEdits();               // snapshot game JSON around widget edits

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
	bool m_showPreferences = false;   // Edit > Preferences (external script editor)

	// Hierarchy / assets (the Content Browser owns its folder, search and thumbnail state).
	char m_hierarchyFilter[128] = "";
	std::unique_ptr<nox::content::ContentBrowser> m_contentBrowser;
	nox::editor::HierarchyView m_hierarchyView;                  // collapse state of the loaded scene
	nox::editor::HierarchySort m_hierarchySort = nox::editor::HierarchySort::SceneOrder;
	nlohmann::json m_hierarchySaved = nlohmann::json::object();  // {"sort", "scenes": {path: collapse state}} from settings
	std::string m_hierarchyScenePath;                             // scene whose collapse state m_hierarchyView holds
	std::weak_ptr<SceneNode> m_hierarchyRevealed;                 // selection last expanded in the tree

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

	// Selection outline (SelectionOutlinePass): style, and the node under the mouse (throttled pick).
	SelectionOutlineSettings m_outline;
	std::weak_ptr<SceneNode> m_hoverNode;
	float m_hoverTimer = 0.0f;

	// Undo.
	std::vector<UndoAction> m_undo, m_redo;
	nlohmann::json m_gameSnapshot;
	bool m_gameEditActive = false;
	nox::editor::NodeTable m_nodeTable;          // stable node handles for undo steps
	nox::editor::PropertyEdit m_propEdit;        // continuous inspector edit being coalesced

	// Rename (F2 / double-click in the Hierarchy) and add-model popups.
	std::weak_ptr<SceneNode> m_renameTarget;
	char m_renameBuf[128] = "";
	bool m_openRename = false;
	std::weak_ptr<SceneNode> m_prefabTarget;
	char m_prefabPathBuf[256] = "";
	bool m_openSavePrefab = false;
	bool m_openAddModel = false;
	char m_modelFilter[128] = "";
	std::vector<std::string> m_modelChoices;

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
