#include "LuaEditor.h"

#include "../EditorExtensions.h"
#include "../ui/Pointer.h"
#include "LuaWorld.h"
#include "lauxlib.h"
#include "lua.h"

#include <IMGUI/imgui.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace NoxLua {

	namespace {
		constexpr const char* kEditorStateKey = "nox.editor.state";

		EditorBinding* BindingOf(lua_State* L) {
			lua_getfield(L, LUA_REGISTRYINDEX, kEditorStateKey);
			EditorBinding* binding = static_cast<EditorBinding*>(lua_touserdata(L, -1));
			lua_pop(L, 1);
			if (!binding) luaL_error(L, "ed is only available while an editor panel or the viewport is drawn");
			return binding;
		}

		ImU32 OptColor(lua_State* L, int idx, ImU32 fallback) {
			if (lua_isnoneornil(L, idx)) return fallback;
			const lua_Integer v = luaL_checkinteger(L, idx);
			if (v < 0 || v > 0xFFFFFFFFLL) luaL_error(L, "color must be 0x00000000..0xFFFFFFFF (0xAARRGGBB)");
			const uint32_t c = static_cast<uint32_t>(v);
			return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF);
		}

		// ed.text(text)
		int EdText(lua_State* L) {
			(void)BindingOf(L);
			ImGui::TextUnformatted(luaL_checkstring(L, 1));
			return 0;
		}

		int EdSeparator(lua_State* L) {
			(void)BindingOf(L);
			ImGui::Separator();
			return 0;
		}

		int EdSameLine(lua_State* L) {
			(void)BindingOf(L);
			ImGui::SameLine();
			return 0;
		}

		// ed.button(label) -> pressed
		int EdButton(lua_State* L) {
			(void)BindingOf(L);
			lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1)) ? 1 : 0);
			return 1;
		}

		// ed.checkbox(label, value) -> value
		int EdCheckbox(lua_State* L) {
			(void)BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			luaL_checktype(L, 2, LUA_TBOOLEAN);
			bool value = lua_toboolean(L, 2) != 0;
			ImGui::Checkbox(label, &value);
			lua_pushboolean(L, value ? 1 : 0);
			return 1;
		}

		// ed.input_float(label, value) -> value
		int EdInputFloat(lua_State* L) {
			(void)BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			float value = static_cast<float>(luaL_checknumber(L, 2));
			ImGui::InputFloat(label, &value);
			lua_pushnumber(L, value);
			return 1;
		}

		// ed.input_int(label, value) -> value (integer)
		int EdInputInt(lua_State* L) {
			(void)BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			int value = static_cast<int>(luaL_checkinteger(L, 2));
			ImGui::InputInt(label, &value);
			lua_pushinteger(L, value);
			return 1;
		}

		// ed.input_text(label, value) -> value (at most 511 bytes are editable)
		int EdInputText(lua_State* L) {
			(void)BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			size_t length = 0;
			const char* text = luaL_checklstring(L, 2, &length);
			char buffer[editor_limits::kMaxTextInput] = {};
			std::strncpy(buffer, text, sizeof(buffer) - 1);
			ImGui::InputText(label, buffer, sizeof(buffer));
			lua_pushstring(L, buffer);
			return 1;
		}

		// ed.slider(label, value, min, max) -> value
		int EdSlider(lua_State* L) {
			(void)BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			float value = static_cast<float>(luaL_checknumber(L, 2));
			const float lo = static_cast<float>(luaL_checknumber(L, 3));
			const float hi = static_cast<float>(luaL_checknumber(L, 4));
			if (!(lo <= hi)) luaL_error(L, "slider: min must not exceed max");
			ImGui::SliderFloat(label, &value, lo, hi);
			lua_pushnumber(L, value);
			return 1;
		}

		// ed.combo(label, index, items) -> index (1-based)
		int EdCombo(lua_State* L) {
			(void)BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			int index = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
			luaL_checktype(L, 3, LUA_TTABLE);
			const size_t count = lua_rawlen(L, 3);
			if (count == 0 || count > editor_limits::kMaxComboItems) {
				luaL_error(L, "combo needs 1..%d items", static_cast<int>(editor_limits::kMaxComboItems));
			}
			std::vector<std::string> storage;
			storage.reserve(count);
			for (size_t i = 1; i <= count; ++i) {
				lua_rawgeti(L, 3, static_cast<lua_Integer>(i));
				if (!lua_isstring(L, -1)) luaL_error(L, "combo items must be strings");
				storage.emplace_back(lua_tostring(L, -1));
				lua_pop(L, 1);
			}
			std::vector<const char*> items;
			items.reserve(count);
			for (const std::string& s : storage) items.push_back(s.c_str());
			index = std::clamp(index, 0, static_cast<int>(count) - 1);
			ImGui::Combo(label, &index, items.data(), static_cast<int>(count));
			lua_pushinteger(L, index + 1);
			return 1;
		}

		// ed.tree(label, function(ed) ... end) -> open. The function draws the children while the node is open.
		int EdTree(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			luaL_checktype(L, 2, LUA_TFUNCTION);
			if (binding->treeDepth >= editor_limits::kMaxTreeDepth) return luaL_error(L, "ed.tree nested deeper than %d", editor_limits::kMaxTreeDepth);
			const bool open = ImGui::TreeNode(label);
			if (open) {
				++binding->treeDepth;
				lua_pushvalue(L, 2);
				lua_rawgeti(L, LUA_REGISTRYINDEX, binding->tableRef);
				lua_call(L, 1, 0);
				--binding->treeDepth;
				ImGui::TreePop();
			}
			lua_pushboolean(L, open ? 1 : 0);
			return 1;
		}

		// ed.handle(id, x, y, z [, {radius = 7, color = 0xFFFFFFFF, label = "Move point"}]) -> dragging, x, y, z, clicked
		// A point in the viewport. Its position is owned by EditorHandles (seeded from x, y, z the first time it is seen).
		int EdHandle(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			if (!binding->inViewport || !binding->handles) {
				return luaL_error(L, "ed.handle is only valid inside nox.editor_viewport");
			}
			const std::string id = luaL_checkstring(L, 1);
			const glm::vec3 seed(static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
				static_cast<float>(luaL_checknumber(L, 4)));
			float radius = 7.0f;
			ImU32 color = IM_COL32_WHITE;
			std::string label = "Move point";
			if (!lua_isnoneornil(L, 5)) {
				luaL_checktype(L, 5, LUA_TTABLE);
				lua_getfield(L, 5, "radius");
				if (!lua_isnil(L, -1)) radius = std::clamp(static_cast<float>(luaL_checknumber(L, -1)), 2.0f, 40.0f);
				lua_pop(L, 1);
				lua_getfield(L, 5, "color");
				color = OptColor(L, -1, color);
				lua_pop(L, 1);
				lua_getfield(L, 5, "label");
				if (!lua_isnil(L, -1)) label = luaL_checkstring(L, -1);
				lua_pop(L, 1);
			}
			EditorHandles& handles = *binding->handles;
			if (!handles.Has(id) && !handles.Set(id, seed)) {
				return luaL_error(L, "too many editor handles (max %d)", static_cast<int>(editor_limits::kMaxHandles));
			}
			// A drag ends when the left button is released (the overlay runs every frame).
			if (handles.Dragging(id) && !ui::Pointer::Get().Down(ui::PointerButton::Left)) handles.EndDrag(id, label);

			glm::vec3 current = seed;
			handles.Get(id, current);
			if (binding->host) {
				ViewportHandle handle;
				handle.world = current;
				handle.color = color;
				handle.radius = radius;
				std::weak_ptr<EditorHandles> weak = binding->handles->weak_from_this();
				handle.onDrag = [weak, id](const glm::vec2& ground) {
					if (const auto alive = weak.lock()) alive->Drag(id, ground);
				};
				handle.onPress = [weak, id] {
					if (const auto alive = weak.lock()) alive->MarkClicked(id);
				};
				binding->host->AddViewportHandle(handle);
			}
			lua_pushboolean(L, handles.Dragging(id) ? 1 : 0);
			lua_pushnumber(L, current.x);
			lua_pushnumber(L, current.y);
			lua_pushnumber(L, current.z);
			lua_pushboolean(L, handles.TakeClick(id) ? 1 : 0);
			return 5;
		}

		// ed.selectable(label, selected) -> clicked
		int EdSelectable(lua_State* L) {
			(void)BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			const bool selected = lua_toboolean(L, 2) != 0;
			lua_pushboolean(L, ImGui::Selectable(label, selected) ? 1 : 0);
			return 1;
		}

		// ed.drag_float(label, value, speed, min, max) -> value (min = max = 0 means unbounded, as in ImGui)
		int EdDragFloat(lua_State* L) {
			(void)BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			float value = static_cast<float>(luaL_checknumber(L, 2));
			const float speed = static_cast<float>(luaL_optnumber(L, 3, 0.1));
			const float lo = static_cast<float>(luaL_optnumber(L, 4, 0.0));
			const float hi = static_cast<float>(luaL_optnumber(L, 5, 0.0));
			if (lo > hi) luaL_error(L, "drag_float: min must not exceed max");
			ImGui::DragFloat(label, &value, speed, lo, hi);
			lua_pushnumber(L, value);
			return 1;
		}

		// Calls function(ed) between open() and close(), so ImGui state is closed even when the callback fails.
		int CallClosedScope(lua_State* L, EditorBinding* binding, const std::function<void()>& open,
			const std::function<void()>& close) {
			open();
			lua_pushvalue(L, 2);
			lua_rawgeti(L, LUA_REGISTRYINDEX, binding->tableRef);
			const int status = lua_pcall(L, 1, 0, 0);
			close();
			if (status != LUA_OK) return lua_error(L);
			return 0;
		}

		// ed.disabled(disabled, function(ed) ... end): draws the callback's widgets greyed out when disabled.
		int EdDisabled(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			const bool off = lua_toboolean(L, 1) != 0;
			luaL_checktype(L, 2, LUA_TFUNCTION);
			return CallClosedScope(L, binding, [off] { ImGui::BeginDisabled(off); }, [] { ImGui::EndDisabled(); });
		}

		// ed.columns(count, function(ed) ... end): the callback's widgets are laid out in `count` columns; call
		// ed.next_column() between cells.
		int EdColumns(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			const int count = static_cast<int>(luaL_checkinteger(L, 1));
			if (count < 1 || count > editor_limits::kMaxColumns) {
				return luaL_error(L, "columns needs 1..%d columns", editor_limits::kMaxColumns);
			}
			luaL_checktype(L, 2, LUA_TFUNCTION);
			return CallClosedScope(L, binding, [count] { ImGui::Columns(count, nullptr, false); }, [] { ImGui::Columns(1); });
		}

		int EdNextColumn(lua_State* L) {
			(void)BindingOf(L);
			ImGui::NextColumn();
			return 0;
		}

		// ed.tooltip(text): shows the text while the previous widget is hovered.
		int EdTooltip(lua_State* L) {
			(void)BindingOf(L);
			const char* text = luaL_checkstring(L, 1);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
			return 0;
		}

		// ed.playing() -> bool (the editor is playing the scene)
		int EdPlaying(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			lua_pushboolean(L, binding->host && binding->host->IsPlaying() ? 1 : 0);
			return 1;
		}

		// ed.request_play(): starts play (no-op without a host or while playing).
		int EdRequestPlay(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			if (binding->host && !binding->host->IsPlaying()) binding->host->RequestPlay();
			return 0;
		}

		// ed.request_stop(): stops play (no-op without a host or while stopped).
		int EdRequestStop(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			if (binding->host && binding->host->IsPlaying()) binding->host->RequestStop();
			return 0;
		}

		// A 3D point: {x, y, z} or {[1], [2], [3]}.
		glm::vec3 ReadPoint(lua_State* L, int idx) {
			luaL_checktype(L, idx, LUA_TTABLE);
			float v[3] = { 0.0f, 0.0f, 0.0f };
			const char* keys[3] = { "x", "y", "z" };
			for (int i = 0; i < 3; ++i) {
				lua_getfield(L, idx, keys[i]);
				if (lua_isnil(L, -1)) {
					lua_pop(L, 1);
					lua_rawgeti(L, idx, i + 1);
				}
				if (!lua_isnumber(L, -1)) luaL_error(L, "a point needs three numbers (x, y, z)");
				v[i] = static_cast<float>(lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
			return glm::vec3(v[0], v[1], v[2]);
		}

		void RequireViewport(lua_State* L, EditorBinding* binding, const char* name) {
			if (!binding->inViewport || !binding->host) {
				luaL_error(L, "%s is only valid inside nox.editor_viewport", name);
			}
		}

		// ed.line3(a, b, color, thickness): a line between two world points (viewport only).
		int EdLine3(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			RequireViewport(L, binding, "ed.line3");
			const glm::vec3 a = ReadPoint(L, 1);
			const glm::vec3 b = ReadPoint(L, 2);
			const ImU32 color = OptColor(L, 3, IM_COL32_WHITE);
			const float thickness = std::clamp(static_cast<float>(luaL_optnumber(L, 4, 1.0)), 0.5f, 16.0f);
			ImVec2 sa, sb;
			// A point behind the camera drops the whole segment (no clipping).
			if (binding->host->ProjectToViewport(a, sa) && binding->host->ProjectToViewport(b, sb)) {
				ImGui::GetWindowDrawList()->AddLine(sa, sb, color, thickness);
			}
			return 0;
		}

		// ed.text3(x, y, z, text, color): a label at a world point (viewport only).
		int EdText3(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			RequireViewport(L, binding, "ed.text3");
			const glm::vec3 p(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
				static_cast<float>(luaL_checknumber(L, 3)));
			const char* text = luaL_checkstring(L, 4);
			const ImU32 color = OptColor(L, 5, IM_COL32_WHITE);
			ImVec2 s;
			if (binding->host->ProjectToViewport(p, s)) {
				ImGui::GetWindowDrawList()->AddText(s, color, text);
			}
			return 0;
		}

		// Runs one undo or redo step of a world's registry (a no-op once that world is gone).
		void RunUndoStep(const std::weak_ptr<EditorUndoRegistry>& weak, int id, bool redo, const std::string& label) {
			const std::shared_ptr<EditorUndoRegistry> reg = weak.lock();
			if (!reg || !reg->world) return;
			const auto it = reg->entries.find(id);
			if (it == reg->entries.end()) return;
			reg->world->RunEditorUndo(redo ? it->second.second : it->second.first, label);
		}

		// ed.undo(label, undo_fn, redo_fn): one undo step on the editor's stack (Ctrl+Z / Ctrl+Y). The functions run in
		// this project's VM when the step is undone or redone. After a reload, Reconfigure or scene load the world is
		// rebuilt and the old steps do nothing.
		int EdUndo(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			const char* label = luaL_checkstring(L, 1);
			luaL_checktype(L, 2, LUA_TFUNCTION);
			luaL_checktype(L, 3, LUA_TFUNCTION);
			if (!binding->undo || !binding->undo->world) return luaL_error(L, "ed.undo has no owning world");
			EditorUndoRegistry& reg = *binding->undo;
			if (reg.entries.size() >= editor_limits::kMaxUndoSteps) {
				return luaL_error(L, "too many undo steps (max %d per project)", static_cast<int>(editor_limits::kMaxUndoSteps));
			}
			lua_pushvalue(L, 2);
			const int undoRef = luaL_ref(L, LUA_REGISTRYINDEX);
			lua_pushvalue(L, 3);
			const int redoRef = luaL_ref(L, LUA_REGISTRYINDEX);
			const int id = reg.next++;
			reg.entries[id] = { undoRef, redoRef };
			if (binding->host) {
				std::weak_ptr<EditorUndoRegistry> weak = binding->undo;
				const std::string text = label;
				binding->host->PushUndo(text,
					[weak, id, text] { RunUndoStep(weak, id, false, text); },
					[weak, id, text] { RunUndoStep(weak, id, true, text); });
			}
			return 0;
		}


		// ed.handle_position(id) -> x, y, z or nil
		int EdHandlePosition(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			glm::vec3 p(0.0f);
			if (!binding->handles || !binding->handles->Get(luaL_checkstring(L, 1), p)) {
				lua_pushnil(L);
				return 1;
			}
			lua_pushnumber(L, p.x);
			lua_pushnumber(L, p.y);
			lua_pushnumber(L, p.z);
			return 3;
		}

		// ed.handle_set(id, x, y, z): moves a point from the script (not while it is being dragged).
		int EdHandleSet(lua_State* L) {
			EditorBinding* binding = BindingOf(L);
			const std::string id = luaL_checkstring(L, 1);
			if (!binding->handles) return luaL_error(L, "no editor handles are available");
			if (binding->handles->Dragging(id)) return 0;
			const glm::vec3 p(static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
				static_cast<float>(luaL_checknumber(L, 4)));
			if (!binding->handles->Set(id, p)) return luaL_error(L, "too many editor handles (max %d)", static_cast<int>(editor_limits::kMaxHandles));
			return 0;
		}
	}

	// ---- EditorHandles -----------------------------------------------------------------------------

	bool EditorHandles::Set(const std::string& id, const glm::vec3& position) {
		if (!Has(id) && m_points.size() >= editor_limits::kMaxHandles) return false;
		m_points[id] = position;
		return true;
	}

	bool EditorHandles::Get(const std::string& id, glm::vec3& out) const {
		const auto it = m_points.find(id);
		if (it == m_points.end()) return false;
		out = it->second;
		return true;
	}

	void EditorHandles::Apply(const std::string& id, const glm::vec3& position) {
		const auto it = m_points.find(id);
		if (it != m_points.end()) it->second = position;
	}

	void EditorHandles::Drag(const std::string& id, const glm::vec2& ground) {
		const auto it = m_points.find(id);
		if (it == m_points.end()) return;
		if (!Dragging(id)) m_dragStart[id] = it->second;
		it->second = glm::vec3(ground.x, it->second.y, ground.y);
	}

	void EditorHandles::EndDrag(const std::string& id, const std::string& label) {
		const auto start = m_dragStart.find(id);
		if (start == m_dragStart.end()) return;
		const glm::vec3 from = start->second;
		m_dragStart.erase(start);
		const auto now = m_points.find(id);
		if (now == m_points.end() || glm::distance(from, now->second) <= 1e-6f || !m_undo) return;
		const glm::vec3 to = now->second;
		// The undo closures hold the handle table weakly: an editor undo stack may outlive the module that owns it.
		std::weak_ptr<EditorHandles> weak = weak_from_this();
		m_undo(label,
			[weak, id, from]() { if (const auto alive = weak.lock()) alive->Apply(id, from); },
			[weak, id, to]() { if (const auto alive = weak.lock()) alive->Apply(id, to); });
	}

	void EditorHandles::Clear() {
		m_points.clear();
		m_dragStart.clear();
		m_clicked.clear();
	}

	// ---- Binding -----------------------------------------------------------------------------------

	void InstallEditorApi(lua_State* L, EditorBinding* binding) {
		lua_pushlightuserdata(L, binding);
		lua_setfield(L, LUA_REGISTRYINDEX, kEditorStateKey);

		lua_newtable(L);
		struct Entry { const char* name; lua_CFunction fn; };
		const Entry entries[] = {
			{ "text", EdText }, { "separator", EdSeparator }, { "same_line", EdSameLine },
			{ "button", EdButton }, { "checkbox", EdCheckbox }, { "input_float", EdInputFloat },
			{ "input_int", EdInputInt }, { "input_text", EdInputText }, { "slider", EdSlider },
			{ "combo", EdCombo }, { "tree", EdTree }, { "handle", EdHandle },
			{ "handle_position", EdHandlePosition }, { "handle_set", EdHandleSet },
			{ "selectable", EdSelectable }, { "drag_float", EdDragFloat }, { "disabled", EdDisabled },
			{ "columns", EdColumns }, { "next_column", EdNextColumn }, { "tooltip", EdTooltip },
			{ "playing", EdPlaying }, { "request_play", EdRequestPlay }, { "request_stop", EdRequestStop },
			{ "line3", EdLine3 }, { "text3", EdText3 }, { "undo", EdUndo },
		};
		for (const Entry& e : entries) {
			lua_pushcfunction(L, e.fn);
			lua_setfield(L, -2, e.name);
		}
		binding->tableRef = luaL_ref(L, LUA_REGISTRYINDEX);
	}
}
