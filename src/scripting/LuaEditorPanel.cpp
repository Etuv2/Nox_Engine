#include "LuaEditorPanel.h"

#include "LuaGameModule.h"
#include "../EditorExtensions.h"

#include <IMGUI/imgui.h>

#include <cstdio>
#include <cstring>

namespace NoxLua {

	namespace {
		void DrawDiagnostic(const Diagnostic& d, int index) {
			const char* severity = d.severity == Diagnostic::Severity::Error ? "error" : "warning";
			std::string location;
			if (!d.file.empty()) location = " " + d.file + ":" + std::to_string(d.line);
			ImGui::PushID(index);
			ImGui::TextWrapped("[%s] %s%s", severity, d.source.c_str(), location.c_str());
			ImGui::TextWrapped("%s", d.message.c_str());
			if (!d.traceback.empty() && ImGui::TreeNode("traceback")) {
				ImGui::TextWrapped("%s", d.traceback.c_str());
				ImGui::TreePop();
			}
			ImGui::Separator();
			ImGui::PopID();
		}

		void DrawPanel(EditorHost& host) {
			LuaGameModule* lua = dynamic_cast<LuaGameModule*>(host.Game());
			if (!lua) {
				ImGui::TextUnformatted("No Lua project is active in this scene.");
				ImGui::TextUnformatted("Select one with the scene key \"project\" (projects/<name>/project.json).");
				return;
			}
			const LuaWorld* world = lua->World();
			ImGui::Text("Project: %s", lua->Project().content.name.c_str());
			ImGui::Text("Descriptor: %s", lua->Project().descriptorPath.c_str());
			ImGui::Text("State: %s   Playing: %s   Paused: %s",
				lua->IsAvailable() ? "loaded" : "unavailable",
				host.IsPlaying() ? "yes" : "no",
				lua->IsPaused() ? "yes" : "no");
			ImGui::Text("Reloads applied: %d   rejected: %d", lua->ReloadCount(), lua->RejectedReloadCount());
			ImGui::Text("Loose-file watcher: %s", lua->WatchEnabled() ? "on (debounced)" : "off");
			ImGui::Separator();

			if (ImGui::Button("Reload")) {
				// Through the engine interface (IGameModule::RequestReload), not a concrete-type call.
				std::string reason;
				if (!host.Game()->RequestReload(&reason)) {
					ImGui::SetTooltip("%s", reason.c_str());
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Retry faulted") && world) {
				LuaWorld* mutableWorld = lua->MutableWorld();
				if (mutableWorld) mutableWorld->RetryFaulted();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("Reload applies at the next frame boundary.");

			if (world) {
				const std::vector<std::string> faults = world->FaultedNames();
				ImGui::Text("Faulted: %d", static_cast<int>(faults.size()));
				for (const std::string& name : faults) ImGui::BulletText("%s", name.c_str());
			}
			ImGui::Separator();
			if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
				const std::vector<Diagnostic>& diagnostics = lua->Diagnostics();
				if (diagnostics.empty()) ImGui::TextDisabled("none");
				int index = 0;
				for (const Diagnostic& d : diagnostics) DrawDiagnostic(d, index++);
			}
			if (ImGui::CollapsingHeader("Reload log")) {
				const std::vector<Diagnostic>& log = lua->ReloadLog();
				if (log.empty()) ImGui::TextDisabled("no reloads yet");
				int index = 1000;
				for (const Diagnostic& d : log) DrawDiagnostic(d, index++);
			}
		}

		// Undo for a declared-data edit: both directions go through the world while the link is alive.
		void PushDeclaredEdit(EditorHost& host, std::weak_ptr<LuaEditorLink> link, const EntityHandle& handle,
			const std::string& schema, const std::string& field, const Value& before, const Value& after) {
			auto apply = [link, handle, schema, field](const Value& value) {
				if (const auto live = link.lock()) {
					if (live->world) {
						std::string error;
						live->world->SetDeclaredField(handle, schema, field, value, error);
					}
				}
			};
			host.PushUndo("Set " + schema + "." + field,
				[apply, before]() { apply(before); },
				[apply, after]() { apply(after); });
		}

		void DrawDeclaredEntity(EditorHost& host, LuaGameModule& lua, LuaWorld& world, const std::string& schemaName,
			const Schema& schema, const EntityHandle& handle) {
			const std::string label = "entity " + std::to_string(handle.id) + "##" + schemaName;
			if (!ImGui::TreeNode(label.c_str())) return;
			for (const auto& entry : schema.fields) {
				const std::string& field = entry.first;
				Value current;
				if (!world.GetDeclaredField(handle, schemaName, field, current)) continue;
				ImGui::PushID(field.c_str());
				Value after = current;
				bool changed = false;
				switch (entry.second) {
				case FieldType::Number: {
					double d = std::holds_alternative<double>(current) ? std::get<double>(current)
						: static_cast<double>(std::get<int64_t>(current));
					if (ImGui::InputDouble(field.c_str(), &d, 0.0, 0.0, "%.6g")) {
						after = d;
						changed = true;
					}
					break;
				}
				case FieldType::Integer: {
					int v = static_cast<int>(std::get<int64_t>(current));
					if (ImGui::InputInt(field.c_str(), &v)) {
						after = static_cast<int64_t>(v);
						changed = true;
					}
					break;
				}
				case FieldType::Boolean: {
					bool b = std::get<bool>(current);
					if (ImGui::Checkbox(field.c_str(), &b)) {
						after = b;
						changed = true;
					}
					break;
				}
				case FieldType::String: {
					char buffer[256] = {};
					const std::string& text = std::get<std::string>(current);
					std::strncpy(buffer, text.c_str(), sizeof(buffer) - 1);
					if (ImGui::InputText(field.c_str(), buffer, sizeof(buffer))) {
						after = std::string(buffer);
						changed = true;
					}
					break;
				}
				}
				if (changed) {
					std::string error;
					if (world.SetDeclaredField(handle, schemaName, field, after, error)) {
						PushDeclaredEdit(host, lua.EditorLinkRef(), handle, schemaName, field, current, after);
					}
					else {
						ImGui::SetTooltip("%s", error.c_str());
					}
				}
				ImGui::PopID();
			}
			ImGui::TreePop();
		}

		// The generic inspector: every declared schema, every entity that has it, every field with a typed widget.
		void DrawInspector(EditorHost& host) {
			LuaGameModule* lua = dynamic_cast<LuaGameModule*>(host.Game());
			LuaWorld* world = lua ? lua->MutableWorld() : nullptr;
			if (!world) {
				ImGui::TextUnformatted("No Lua project is active in this scene.");
				return;
			}
			ImGui::TextDisabled("Declared data of the active project. Edits are undoable.");
			if (world->Schemas().empty()) {
				ImGui::TextDisabled("The project declares no schemas.");
				return;
			}
			for (const auto& schemaEntry : world->Schemas()) {
				const std::string& name = schemaEntry.first;
				if (!ImGui::CollapsingHeader(name.c_str())) continue;
				const std::vector<EntityHandle> handles = world->DeclaredHandles(name);
				if (handles.empty()) ImGui::TextDisabled("no entities have this schema");
				for (const EntityHandle& handle : handles) {
					DrawDeclaredEntity(host, *lua, *world, name, schemaEntry.second, handle);
				}
			}
		}
	}

	void RegisterLuaProjectPanels(const LuaWorld& world, const std::string& projectPath) {
		for (const LuaWorld::EditorPanelDecl& decl : world.EditorPanels()) {
			EditorPanel panel;
			panel.id = "lua.panel." + decl.id;
			panel.title = decl.title;
			panel.openByDefault = false;
			const std::string id = decl.id;
			panel.available = [projectPath](EditorHost& host) {
				const LuaGameModule* lua = dynamic_cast<const LuaGameModule*>(host.Game());
				return lua != nullptr && lua->Project().descriptorPath == projectPath;
			};
			panel.draw = [id](EditorHost& host) {
				if (LuaGameModule* lua = dynamic_cast<LuaGameModule*>(host.Game())) {
					lua->DrawEditorPanelById(id, host);
				}
				else {
					ImGui::TextDisabled("This panel belongs to a Lua project that is not active.");
				}
			};
			EditorExtensions::RegisterPanel(std::move(panel));
		}
	}

	void RegisterLuaEditorPanel() {
		static bool registered = false;
		if (registered) return;
		registered = true;

		// Shown only while a Lua project module is the active one (EditorPanel::available).
		const auto luaActive = [](EditorHost& host) { return dynamic_cast<const LuaGameModule*>(host.Game()) != nullptr; };

		EditorPanel panel;
		panel.id = "lua.project";
		panel.title = "Lua project";
		panel.draw = DrawPanel;
		panel.openByDefault = false;
		panel.available = luaActive;
		EditorExtensions::RegisterPanel(std::move(panel));

		EditorPanel inspector;
		inspector.id = "lua.inspector";
		inspector.title = "Lua declared data";
		inspector.draw = DrawInspector;
		inspector.openByDefault = false;
		inspector.available = luaActive;
		EditorExtensions::RegisterPanel(std::move(inspector));

		// Viewport handles (ed.handle), drawn alongside any other registered overlay.
		EditorExtensions::AddViewportOverlay("lua", [](EditorHost& host, ImDrawList* drawList) {
			(void)drawList;
			if (LuaGameModule* lua = dynamic_cast<LuaGameModule*>(host.Game())) lua->DrawEditorViewport(host);
		});
	}
}
