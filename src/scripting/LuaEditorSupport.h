#pragma once
// Editor support for project Lua in an external IDE (docs/lua_api.md, "Editor support"). Two files per project:
//   <project>/.nox/types/nox.lua   LuaLS annotations (---@meta): the engine API (nox.*, ctx, ed, ui) written by hand,
//                                  plus one nox.comp.<Name> class per reflection descriptor, generated from the registry.
//                                  Git-ignored: the engine rewrites it whenever its text changes.
//   <project>/.luarc.json          LuaLS config: Lua 5.5 runtime, workspace library ./.nox/types, engine globals.
// .luarc.json is merged: keys the user set are kept and missing keys are added. A file with comments is left alone
// (rewriting it would lose them). The project folder is the VS Code workspace (see docs/editor.md).
#include <string>

class IGameModule;

namespace NoxLua {
	// The annotation text: the hand-written API plus the components of the reflection registry as it is now.
	std::string LuaApiStubText();

	// Writes both files into projectDir (physical path of the project folder). `report` gets one line per file.
	// False when a file could not be written or .luarc.json is not valid JSON.
	bool WriteLuaEditorSupport(const std::string& projectDir, std::string& report);

	// Same for the project whose descriptor is the logical path "projects/<name>/project.json" (loose source tree only).
	bool EnsureLuaEditorSupport(const std::string& descriptorPath, std::string& report);

	// File > Generate Lua Editor Support: for the active game module when it is a Lua project.
	// False with a reason in `report` when there is no active Lua project.
	bool GenerateLuaEditorSupport(IGameModule* game, std::string& report);

	// Logical folder of the nearest project.json at or above logicalPath ("" when none).
	std::string ProjectRootOf(const std::string& logicalPath);
}
