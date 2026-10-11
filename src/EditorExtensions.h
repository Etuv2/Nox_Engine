#pragma once
// Editor extension points. The application (game layer) contributes editor panels, a viewport
// overlay with clickable handles, and toolbar icons through this registry; the engine editor
// (ImGui/EditorWorkspace) iterates it and never names a game type.
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>
#include <IMGUI/imgui.h>
#include "editor/solace/SolaceUI.h"
#include "json.hpp"

class IGameModule;
class SceneGraph;
class ModularRenderer;

// Clickable marker in the Scene viewport, rebuilt every frame by the viewport overlay.
struct ViewportHandle {
	glm::vec3 world{ 0.0f };                                 // ground point (y = marker height)
	ImU32 color = IM_COL32_WHITE;
	float radius = 7.0f;
	std::function<void()> onPress;                           // optional: click selects / reveals data
	std::function<void(const glm::vec2& ground)> onDrag;     // optional: dragged on the ground plane
};

// What editor panels may use from the editor. Implemented by EditorWorkspace.
class EditorHost {
public:
	virtual ~EditorHost() = default;

	virtual bool IsPlaying() const = 0;
	virtual void RequestPlay() = 0;                          // starts play and focuses the Game view
	virtual void MarkDirty() = 0;
	virtual void PushUndo(const std::string& label, std::function<void()> undo, std::function<void()> redo) = 0;
	virtual void ApplyGameJson(const nlohmann::json& game) = 0;
	// The module's authoring data (empty object when no module is active).
	virtual nlohmann::json GetAuthoredGameJson() const = 0;
	// Turns the widget edits since the last commit into one undo step.
	virtual void CommitGameEdit(const std::string& label) = 0;
	virtual void ApplyRuntimeChangesToAuthored() = 0;
	virtual IGameModule* Game() = 0;                         // null when no module is active
	virtual SceneGraph* GetSceneGraph() = 0;
	virtual ModularRenderer* GetRenderer() = 0;             // null when no renderer is connected

	// Scene viewport: world point to ImGui screen position; false when behind the camera.
	virtual bool ProjectToViewport(const glm::vec3& world, ImVec2& out) const = 0;
	// Valid only while the viewport overlay is being drawn.
	virtual void AddViewportHandle(const ViewportHandle& handle) = 0;
	virtual void ShowPanel(const std::string& id) = 0;
};

// A dockable editor panel. draw() submits the panel body only; the editor owns Begin/End.
struct EditorPanel {
	std::string id;
	std::string title;
	std::function<void(EditorHost&)> draw;
	bool openByDefault = true;
};

namespace EditorExtensions {
	// Registering an id that already exists replaces that panel.
	void RegisterPanel(EditorPanel panel);
	const std::vector<EditorPanel>& Panels();

	using ViewportOverlay = std::function<void(EditorHost&, ImDrawList*)>;
	void SetViewportOverlay(ViewportOverlay overlay);
	const ViewportOverlay& GetViewportOverlay();

	// Maps an editor role ("editor/save", "editor/play", "editor/pause") to a UiIconAtlas id.
	// Returning an empty string keeps the built-in vector glyph.
	using ToolbarIconResolver = std::function<std::string(const char* role)>;
	void SetToolbarIconResolver(ToolbarIconResolver resolver);
	std::string ResolveToolbarIcon(const char* role);
}

// Editor widget helpers shared by the engine editor and the editor panels (EditorExtensions.cpp).
namespace ui {
	enum class ToastKind { Info, Success, Warning, Error };
	void Grid(const char* id);
	void Label(const char* label);
	void EndGrid();
	bool Btn(const char* label, bool primary);
	bool Tool(const char* id, solace::Icon icon, const char* label, bool active, bool enabled, const char* tip);
	// Same toolbar button with a UiIconAtlas picture in place of the vector glyph; falls back to
	// Tool() when atlasId is empty or not in the atlas.
	bool ToolIcon(const char* id, const std::string& atlasId, solace::Icon fallback, const char* label, bool active,
		bool enabled, const char* tip);
	bool Section(const char* title, bool defaultOpen);
	bool Seg(const char* id, int* value, std::initializer_list<std::pair<const char*, int>> items);
	void Toast(const std::string& text, ToastKind kind);
	void Progress(float p, float width);
}
