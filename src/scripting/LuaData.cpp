#include "LuaData.h"

#include "LuaServices.h"
#include "../assets/AssetIO.h"
#include "lauxlib.h"
#include "lua.h"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <system_error>
#include <utility>

namespace NoxLua {

	namespace {
		DataState* StateOf(lua_State* L) {
			return static_cast<DataState*>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		// "dir/x" is inside "dir" (dir has no trailing slash).
		bool InDir(const std::string& path, const std::string& dir) {
			return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
		}

		std::string TrimSlash(std::string s) {
			while (!s.empty() && s.back() == '/') s.pop_back();
			return s;
		}

		bool EndsWithJson(const std::string& path) {
			return path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0;
		}

		// Pushes a copy of a configured JSON value, or nil with a reason when it exceeds the copy bounds.
		int PushCopy(lua_State* L, const nlohmann::json& value) {
			std::string reason;
			if (!JsonWithinBounds(value, data_limits::kMaxJsonDepth, data_limits::kMaxJsonNodes, reason)) {
				lua_pushnil(L);
				lua_pushstring(L, reason.c_str());
				return 2;
			}
			PushJsonValue(L, value);
			return 1;
		}

		// nox.json(logical) -> table | nil, reason
		int NoxJson(lua_State* L) {
			DataState* state = StateOf(L);
			const std::string requested = luaL_checkstring(L, 1);
			auto fail = [L](const std::string& reason) {
				lua_pushnil(L);
				lua_pushstring(L, reason.c_str());
				return 2;
			};
			if (!AssetIO::IsInitialized()) return fail("the asset service is not initialized");
			std::string reason;
			const auto normalized = AssetIO::Normalize(requested, &reason);
			if (!normalized) return fail(reason);
			if (!DataReadAllowed(*state, *normalized, reason)) return fail(reason);
			const auto stat = AssetIO::Stat(*normalized);
			if (!stat || stat->isDirectory) return fail("no such file: " + *normalized);
			if (stat->size > data_limits::kMaxJsonFileBytes) return fail("file is larger than 1 MiB");
			const auto text = AssetIO::ReadText(*normalized, &reason);
			if (!text) return fail(reason);
			const nlohmann::json value = nlohmann::json::parse(*text, nullptr, false);
			if (value.is_discarded()) return fail("not valid JSON: " + *normalized);
			if (!JsonWithinBounds(value, data_limits::kMaxJsonDepth, data_limits::kMaxJsonNodes, reason)) return fail(reason);
			PushJsonValue(L, value);
			return 1;
		}

		// nox.project() -> copy of the project descriptor (project.json)
		int NoxProject(lua_State* L) {
			DataState* state = StateOf(L);
			return PushCopy(L, state->project.is_null() ? nlohmann::json::object() : state->project);
		}

		// nox.game() -> copy of the scene "game" object, nil when the scene has none
		int NoxGame(lua_State* L) {
			return PushCopy(L, StateOf(L)->game);
		}

		// nox.array(t) -> t, marked as a JSON array (an empty one saves as [] and not {}); nox.array() -> new array.
		int NoxArray(lua_State* L) {
			if (lua_isnoneornil(L, 1)) {
				lua_settop(L, 0);
				lua_newtable(L);
			}
			else {
				luaL_checktype(L, 1, LUA_TTABLE);
				lua_settop(L, 1);
			}
			MarkJsonArray(L, 1);
			return 1;
		}

		// nox.scene() -> copy of the whole scene document
		int NoxScene(lua_State* L) {
			return PushCopy(L, StateOf(L)->root);
		}

		// nox.save_authoring_json(logical, value) -> true | false, reason
		int NoxSaveAuthoringJson(lua_State* L) {
			DataState* state = StateOf(L);
			const std::string requested = luaL_checkstring(L, 1);
			luaL_checkany(L, 2);
			auto result = [L](bool ok, const std::string& reason) {
				lua_pushboolean(L, ok ? 1 : 0);
				if (ok) lua_pushnil(L);
				else lua_pushstring(L, reason.c_str());
				return 2;
			};
			if (!AssetIO::IsInitialized()) return result(false, "the asset service is not initialized");
			if (AssetIO::GetMode() == AssetIO::Mode::Packed) {
				return result(false, "authoring writes are disabled in packed distributions");
			}
			if (!state->editorWrite) return result(false, "authoring writes are only allowed from editor panels and viewports");
			if (state->staging) return result(false, "authoring writes are blocked while a reload is staging");
			std::string reason;
			const auto normalized = AssetIO::Normalize(requested, &reason);
			if (!normalized) return result(false, reason);
			if (!AuthoringPathAllowed(*state, *normalized, reason)) return result(false, reason);
			nlohmann::json value;
			if (!LuaValueToJson(L, 2, value, reason)) return result(false, reason);
			const auto physical = AssetIO::SourcePath(*normalized);
			if (!physical) return result(false, "no loose source file maps to this path");
			std::string text;
			try {
				text = value.dump(2);
			}
			catch (const std::exception& e) {
				return result(false, std::string("the value cannot be encoded as JSON (") + e.what() + ")");
			}
			text.push_back('\n');
			if (!WriteTextAtomic(*physical, text, reason)) return result(false, reason);
			return result(true, std::string());
		}

		// ctx:env(name) -> string | nil, reason. Only NOX_ variables; a test/diagnostic facility, not configuration.
		int CtxEnv(lua_State* L) {
			const std::string name = luaL_checkstring(L, 2);
			if (name.compare(0, 4, "NOX_") != 0) {
				lua_pushnil(L);
				lua_pushstring(L, "only NOX_ variables can be read");
				return 2;
			}
			if (name.size() > data_limits::kMaxEnvNameLength) return luaL_error(L, "env: name is longer than 64 characters");
			for (char c : name) {
				const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
				if (!ok) return luaL_error(L, "env: name may contain only A-Z, 0-9 and _");
			}
			const char* value = std::getenv(name.c_str());
			if (!value) {
				lua_pushnil(L);
				return 1;
			}
			lua_pushstring(L, value);
			return 1;
		}

		// ctx:request_exit(): the module reports ExitApplication from its next Update.
		int CtxRequestExit(lua_State* L) {
			StateOf(L)->exitRequested = true;
			return 0;
		}
	}

	void ApplyProjectPolicy(DataState& state, const std::string& projectDir, const std::vector<std::string>& scriptRoots,
		const std::vector<std::string>& roadFiles, const std::vector<std::string>& authoringRoots, const nlohmann::json& descriptor) {
		state.projectDir = TrimSlash(projectDir);
		state.readRoots = { "config", "data", "scenes" };
		for (const std::string& root : scriptRoots) state.readRoots.push_back(TrimSlash(root));
		state.readFiles = roadFiles;
		state.authoringRoots.clear();
		for (const std::string& root : authoringRoots) state.authoringRoots.push_back(TrimSlash(root));
		state.project = descriptor.is_object() ? descriptor : nlohmann::json::object();
	}

	bool DataReadAllowed(const DataState& state, const std::string& path, std::string& reason, bool media) {
		for (const std::string& file : state.readFiles) {
			if (path == file) return true;
		}
		if (!state.projectDir.empty() && InDir(path, state.projectDir)) return true;
		for (const std::string& root : state.readRoots) {
			if (InDir(path, root)) return true;
		}
		if (media) {
			for (const char* root : { "textures", "images", "fonts" }) {
				if (InDir(path, root)) return true;
			}
		}
		reason = "path is not readable by this project (use its project directory, config/, data/, scenes/ or a declared file)";
		return false;
	}

	bool AuthoringPathAllowed(const DataState& state, const std::string& path, std::string& reason) {
		if (!EndsWithJson(path)) {
			reason = "authoring files must end with .json";
			return false;
		}
		for (const std::string& root : state.authoringRoots) {
			if (path == root || InDir(path, root)) return true;
		}
		reason = "path is not an authoring root of this project (project.json authoring_roots)";
		return false;
	}

	bool WriteTextAtomic(const std::filesystem::path& target, const std::string& text, std::string& error) {
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path dir = target.parent_path();
		if (!dir.empty()) {
			fs::create_directories(dir, ec);
			if (ec) {
				error = "cannot create the folder: " + ec.message();
				return false;
			}
		}
		fs::path temp = target;
		temp += ".tmp";   // same directory, so the rename stays on one volume
		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			if (!out) {
				error = "cannot open a temporary file next to the target";
				return false;
			}
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			out.flush();
			if (!out) {
				out.close();
				fs::remove(temp, ec);
				error = "writing the temporary file failed";
				return false;
			}
		}
		fs::rename(temp, target, ec);
		if (ec) {
			fs::remove(temp, ec);
			error = "cannot replace the file: " + ec.message();
			return false;
		}
		return true;
	}

	bool JsonWithinBounds(const nlohmann::json& value, int maxDepth, int maxNodes, std::string& reason) {
		int nodes = 0;
		std::vector<std::pair<const nlohmann::json*, int>> stack;
		stack.emplace_back(&value, 1);
		while (!stack.empty()) {
			const auto [node, depth] = stack.back();
			stack.pop_back();
			if (++nodes > maxNodes) {
				reason = "the value has more than " + std::to_string(maxNodes) + " values";
				return false;
			}
			if (depth > maxDepth) {
				reason = "the value is nested deeper than " + std::to_string(maxDepth) + " levels";
				return false;
			}
			if (node->is_structured()) {
				for (const nlohmann::json& child : *node) stack.emplace_back(&child, depth + 1);
			}
		}
		return true;
	}

	void InstallDataApi(lua_State* L, int ctxIdx, int noxIdx, DataState* state) {
		ctxIdx = lua_absindex(L, ctxIdx);
		noxIdx = lua_absindex(L, noxIdx);
		auto set = [L, state](int tableIdx, const char* name, lua_CFunction fn) {
			lua_pushlightuserdata(L, state);
			lua_pushcclosure(L, fn, 1);
			lua_setfield(L, tableIdx, name);
		};
		set(noxIdx, "json", NoxJson);
		set(noxIdx, "project", NoxProject);
		set(noxIdx, "game", NoxGame);
		set(noxIdx, "array", NoxArray);
		set(noxIdx, "scene", NoxScene);
		set(noxIdx, "save_authoring_json", NoxSaveAuthoringJson);
		set(ctxIdx, "env", CtxEnv);
		set(ctxIdx, "request_exit", CtxRequestExit);
	}
}
