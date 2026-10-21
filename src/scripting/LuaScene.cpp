#include "LuaScene.h"

#include "../ComponentManager.h"
#include "../SceneGraph.h"
#include "../SceneNode.h"
#include "../reflect/ReflectServices.h"
#include "../scene/Prefab.h"
#include "LuaWorld.h"
#include "lauxlib.h"
#include "lua.h"

#include <cstring>
#include <string>

namespace NoxLua {

	namespace {
		constexpr const char* kHandleMeta = "nox.entity";   // the handle metatable LuaWorld installs
		constexpr size_t kMaxPrefabPath = 256;

		LuaWorld* WorldOf(lua_State* L) {
			return static_cast<LuaWorld*>(lua_touserdata(L, lua_upvalueindex(1)));
		}

		void SetFn(lua_State* L, LuaWorld* w, int tableIdx, const char* name, lua_CFunction fn) {
			lua_pushlightuserdata(L, w);
			lua_pushcclosure(L, fn, 1);
			lua_setfield(L, tableIdx, name);
		}

		void PushHandle(lua_State* L, const EntityHandle& h) {
			void* mem = lua_newuserdatauv(L, sizeof(EntityHandle), 0);
			std::memcpy(mem, &h, sizeof(EntityHandle));
			luaL_setmetatable(L, kHandleMeta);
		}

		const EntityHandle* TestHandle(lua_State* L, int idx) {
			return static_cast<const EntityHandle*>(luaL_testudata(L, idx, kHandleMeta));
		}

		// Reads {x=, y=, z=} or a 1-based array of n numbers.
		bool ReadNumbers(lua_State* L, int idx, int n, double* out) {
			idx = lua_absindex(L, idx);
			static const char* const keys[3] = { "x", "y", "z" };
			for (int i = 0; i < n; ++i) {
				lua_getfield(L, idx, keys[i]);
				if (lua_type(L, -1) != LUA_TNUMBER) {
					lua_pop(L, 1);
					lua_geti(L, idx, i + 1);
				}
				if (lua_type(L, -1) != LUA_TNUMBER) {
					lua_pop(L, 1);
					return false;
				}
				out[i] = lua_tonumber(L, -1);
				lua_pop(L, 1);
			}
			return true;
		}

		// Logical prefab path: relative, '/'-separated, [A-Za-z0-9_./-] only, no parent segments, *.prefab.json.
		bool ValidPrefabPath(const std::string& path, std::string& error) {
			if (path.size() > kMaxPrefabPath) { error = "path is too long"; return false; }
			if (!nox::prefab::IsPrefabPath(path)) { error = "expected a path ending in .prefab.json"; return false; }
			if (path.find("..") != std::string::npos || path.find("//") != std::string::npos || path[0] == '/') {
				error = "parent or repeated separators are not allowed";
				return false;
			}
			for (char c : path) {
				const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
					c == '_' || c == '-' || c == '.' || c == '/';
				if (!ok) { error = "only letters, digits, _ - . / are allowed"; return false; }
			}
			return true;
		}

		// ctx:instantiate(path, opts) -> handle. opts (all optional): parent (handle; default scene root), position,
		// rotation (degrees, Euler), scale, name. The instance is created immediately, like the scene loader creates it,
		// and is removed at Stop.
		int CtxInstantiate(lua_State* L) {
			LuaWorld* w = WorldOf(L);
			if (!w->WorldWritable()) {
				return luaL_error(L, "instantiate is only available inside a system, behavior or event");
			}
			const std::string path = luaL_checkstring(L, 2);
			std::string error;
			if (!ValidPrefabPath(path, error)) return luaL_argerror(L, 2, error.c_str());

			nox::reflect::EngineServices& services = nox::reflect::Services();
			if (!services.instantiatePrefab) return luaL_error(L, "instantiate: prefab instancing is not available in this host");

			nlohmann::json placement = nlohmann::json::object();
			std::shared_ptr<SceneNode> parent;
			if (!lua_isnoneornil(L, 3)) {
				luaL_checktype(L, 3, LUA_TTABLE);
				lua_getfield(L, 3, "parent");
				if (!lua_isnil(L, -1)) {
					const EntityHandle* ph = TestHandle(L, -1);
					if (!ph) return luaL_argerror(L, 3, "opts.parent must be an entity handle");
					parent = w->FindSceneNode(ph->id);
					if (!parent) return luaL_error(L, "instantiate: parent has no scene node");
				}
				lua_pop(L, 1);
				const char* const vectorKeys[3] = { "position", "rotation", "scale" };
				for (const char* key : vectorKeys) {
					lua_getfield(L, 3, key);
					if (!lua_isnil(L, -1)) {
						double v[3];
						if (!ReadNumbers(L, -1, 3, v)) return luaL_argerror(L, 3, (std::string("opts.") + key + " must be 3 numbers").c_str());
						placement[key] = { v[0], v[1], v[2] };
					}
					lua_pop(L, 1);
				}
				lua_getfield(L, 3, "name");
				if (!lua_isnil(L, -1)) {
					if (lua_type(L, -1) != LUA_TSTRING) return luaL_argerror(L, 3, "opts.name must be a string");
					placement["name"] = lua_tostring(L, -1);
				}
				lua_pop(L, 1);
			}

			std::shared_ptr<SceneNode> node = services.instantiatePrefab(path, parent, placement, error);
			if (!node) return luaL_error(L, "instantiate '%s': %s", path.c_str(), error.c_str());
			ComponentManager* cm = w->Manager();
			if (!cm || node->GetEntityID() == INVALID_ENTITY) return luaL_error(L, "instantiate: the instance has no entity");
			PushHandle(L, cm->MakeHandle(node->GetEntityID()));
			return 1;
		}
	}

	namespace {
		std::optional<nlohmann::json> g_pendingPayload;   // one slot: the latest load_scene request wins
	}

	void SetPendingScenePayload(std::optional<nlohmann::json> payload) {
		g_pendingPayload = std::move(payload);
	}

	std::optional<nlohmann::json> TakePendingScenePayload() {
		std::optional<nlohmann::json> out = std::move(g_pendingPayload);
		g_pendingPayload.reset();
		return out;
	}

	void InstallSceneApi(lua_State* L, int ctxIdx, LuaWorld* w) {
		ctxIdx = lua_absindex(L, ctxIdx);
		SetFn(L, w, ctxIdx, "instantiate", CtxInstantiate);
	}
}
