#pragma once
// Project data bindings (docs/lua_api.md "Project data", "Environment"): read-only JSON through AssetIO, authoring
// saves (editor contexts only, loose distributions only), the read-only project descriptor and scene documents, and
// the NOX_* environment facility for test hooks. Everything here is generic: the project declares what it may read
// and what it may write; the engine enforces those declarations.
#include "json.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

struct lua_State;

namespace NoxLua {

	namespace data_limits {
		constexpr size_t kMaxJsonFileBytes = 1024u * 1024u;   // nox.json: file size bound
		constexpr int kMaxJsonDepth = 32;                     // nox.json / nox.game / nox.scene: nesting bound
		constexpr int kMaxJsonNodes = 200000;                 // values in one copy handed to Lua
		constexpr size_t kMaxEnvNameLength = 64;
	}

	// Per-world state of the data bindings. Owned by LuaWorld; the Lua closures keep a pointer to it.
	struct DataState {
		std::string projectDir;                  // logical directory of project.json ("" when unknown)
		std::vector<std::string> readRoots;      // logical directories (no trailing '/'), readable by nox.json
		std::vector<std::string> readFiles;      // exact logical files readable by nox.json (declared roads)
		std::vector<std::string> authoringRoots; // logical files or directories (no trailing '/') nox.save_authoring_json may write
		nlohmann::json project;                  // copy of the project descriptor (nox.project)
		nlohmann::json game;                     // scene "game" object (null when the scene has none)
		nlohmann::json root;                     // scene root document
		bool editorWrite = false;                // inside an editor panel or viewport callback
		bool staging = false;                    // candidate world of a reload: no authoring writes
		bool exitRequested = false;              // ctx:request_exit()
	};

	// Fills the read/write policy from the project declarations. Paths are logical and already normalized by the caller.
	void ApplyProjectPolicy(DataState& state, const std::string& projectDir, const std::vector<std::string>& scriptRoots,
		const std::vector<std::string>& roadFiles, const std::vector<std::string>& authoringRoots, const nlohmann::json& descriptor);

	// nox.json / nox.project / nox.game / nox.scene / nox.save_authoring_json (nox table) and ctx:env / ctx:request_exit.
	// The state pointer is captured as a closure upvalue and must outlive the VM.
	void InstallDataApi(lua_State* L, int ctxIdx, int noxIdx, DataState* state);

	// Read policy for nox.json (and ui.texture with media = true, which also allows textures/, images/ and fonts/).
	// `normalized` is an AssetIO-normalized logical path.
	bool DataReadAllowed(const DataState& state, const std::string& normalized, std::string& reason, bool media = false);
	// Authoring policy for nox.save_authoring_json (path and context only; the caller checks the packaging mode).
	bool AuthoringPathAllowed(const DataState& state, const std::string& normalized, std::string& reason);

	// Writes text to a physical file atomically: temp file in the same directory, then rename over the target.
	bool WriteTextAtomic(const std::filesystem::path& target, const std::string& text, std::string& error);

	// True when a decoded JSON value stays within the nesting and node bounds (used before handing copies to Lua).
	bool JsonWithinBounds(const nlohmann::json& value, int maxDepth, int maxNodes, std::string& reason);
}
