#pragma once
// Editor integration for project Lua (src/EditorExtensions.h): the "Lua project" panel (diagnostics, faults, Reload),
// the "Lua declared data" inspector, project panels declared with nox.editor_panel, and the viewport overlay that
// draws ed.handle points.
#include <string>

namespace NoxLua {
	class LuaWorld;

	// Registers the built-in panels and the viewport overlay once (idempotent). Called when a Lua project module is created.
	void RegisterLuaEditorPanel();
	// Registers (or replaces) one editor panel per nox.editor_panel declared by the world. Called on every world build.
	// The panels are available only while the project with `projectPath` (its descriptor) is the active module.
	void RegisterLuaProjectPanels(const LuaWorld& world, const std::string& projectPath);
}
