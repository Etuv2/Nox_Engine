#pragma once
// Editor binding for project Lua (docs/lua_api.md "Editor"): immediate-mode widgets for project-defined editor panels
// (nox.editor_panel), viewport handles for authoring points (nox.editor_viewport + ed.handle) and undo for their
// edits through the editor's undo stack (EditorHost::PushUndo). Scripts draw with `ed`; the engine owns the widgets.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <glm/glm.hpp>

struct lua_State;
class EditorHost;

namespace NoxLua {
	class LuaWorld;
}

namespace NoxLua {

	namespace editor_limits {
		constexpr size_t kMaxPanels = 16;
		constexpr size_t kMaxHandles = 256;
		constexpr size_t kMaxUndoSteps = 4096;      // per world (ed.undo); cleared with the world
		constexpr int kMaxColumns = 16;
		constexpr size_t kMaxTextInput = 512;
		constexpr size_t kMaxComboItems = 256;
		constexpr int kMaxTreeDepth = 8;
	}

	// Undo steps registered by a world's editor code (ed.undo). The steps are Lua functions in that world's VM: the
	// world owns the references and clears `world` when it is destroyed (reload, Reconfigure, scene load). A step whose
	// world is gone does nothing, so closures never outlive the VM they call into.
	struct EditorUndoRegistry {
		NoxLua::LuaWorld* world = nullptr;
		std::map<int, std::pair<int, int>> entries;   // step id -> (undo ref, redo ref)
		int next = 1;
	};

	// Points authored in the viewport. Positions are owned here (scripts read them through ed.handle). A drag moves a
	// point on the ground plane; ending the drag records one undo step with the start and end positions.
	class EditorHandles : public std::enable_shared_from_this<EditorHandles> {
	public:
		using UndoSink = std::function<void(const std::string& label, std::function<void()> undo, std::function<void()> redo)>;

		void SetUndoSink(UndoSink sink) { m_undo = std::move(sink); }
		// Creates or replaces a point. Returns false when the handle budget is used up.
		bool Set(const std::string& id, const glm::vec3& position);
		bool Get(const std::string& id, glm::vec3& out) const;
		bool Has(const std::string& id) const { return m_points.count(id) != 0; }

		// Drag protocol (driven by the viewport overlay): Drag moves the point to the ground position (x, z);
		// EndDrag closes the drag and pushes one undo step when the point moved.
		void Drag(const std::string& id, const glm::vec2& ground);
		void EndDrag(const std::string& id, const std::string& label = "Move point");
		bool Dragging(const std::string& id) const { return m_dragStart.count(id) != 0; }
		size_t Count() const { return m_points.size(); }
		void Clear();

		// Press protocol: the overlay reports a press after it has drawn the handles, so ed.handle returns the
		// clicked flag on the next frame.
		void MarkClicked(const std::string& id) { m_clicked.insert(id); }
		bool TakeClick(const std::string& id) { return m_clicked.erase(id) != 0; }

	private:
		void Apply(const std::string& id, const glm::vec3& position);
		std::map<std::string, glm::vec3> m_points;
		std::map<std::string, glm::vec3> m_dragStart;   // start positions of points being dragged
		std::set<std::string> m_clicked;
		UndoSink m_undo;
	};

	// Per-call binding of the `ed` object (the module sets it while a panel or the viewport overlay runs).
	struct EditorBinding {
		EditorHost* host = nullptr;          // null in headless tests
		EditorHandles* handles = nullptr;
		bool inViewport = false;             // ed.handle is valid only while the viewport overlay is drawn
		int tableRef = 0;                    // registry reference of the `ed` table (passed to ed.tree callbacks)
		int treeDepth = 0;
		std::shared_ptr<EditorUndoRegistry> undo;   // owned by the world; see EditorUndoRegistry
	};

	// Creates the `ed` table bound to `binding` (the binding must outlive the VM).
	void InstallEditorApi(lua_State* L, EditorBinding* binding);
}
