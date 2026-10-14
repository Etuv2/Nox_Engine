#include "LuaInputBindings.h"

#include "LuaInput.h"
#include "LuaWorld.h"

#include "lauxlib.h"
#include "lua.h"

#include <string>
#include <vector>

namespace NoxLua {

namespace {

// The module's map, or null when no project is loaded (every call then fails softly).
InputMap* MapOf(lua_State* L) {
	LuaWorld* world = static_cast<LuaWorld*>(lua_touserdata(L, lua_upvalueindex(1)));
	return world ? world->InputMapPtr() : nullptr;
}

int NilError(lua_State* L, const std::string& message) {
	lua_pushnil(L);
	lua_pushstring(L, message.c_str());
	return 2;
}

void PushNames(lua_State* L, const std::vector<std::string>& names) {
	lua_createtable(L, static_cast<int>(names.size()), 0);
	for (size_t i = 0; i < names.size(); ++i) {
		lua_pushstring(L, names[i].c_str());
		lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
	}
}

// input.rebind(action, key_name)
int InputRebind(lua_State* L) {
	const std::string action = luaL_checkstring(L, 1);
	const std::string key = luaL_checkstring(L, 2);
	InputMap* map = MapOf(L);
	if (!map) return NilError(L, "no action map is loaded");
	std::string error;
	if (!map->Rebind(action, key, error)) return NilError(L, error);
	lua_pushboolean(L, 1);
	return 1;
}

// input.bindings(action)
int InputBindings(lua_State* L) {
	const std::string action = luaL_checkstring(L, 1);
	InputMap* map = MapOf(L);
	if (!map) return NilError(L, "no action map is loaded");
	std::vector<std::string> keys, pads;
	if (!map->KeyNames(action, keys, pads)) return NilError(L, "unknown input action '" + action + "'");
	lua_createtable(L, 0, 2);
	PushNames(L, keys);
	lua_setfield(L, -2, "keys");
	PushNames(L, pads);
	lua_setfield(L, -2, "pad_buttons");
	return 1;
}

// input.save_bindings()
int InputSaveBindings(lua_State* L) {
	InputMap* map = MapOf(L);
	lua_pushboolean(L, map && map->SaveOverrides() ? 1 : 0);
	return 1;
}

// input.rumble(low, high, ms)
int InputRumble(lua_State* L) {
	const float low = static_cast<float>(luaL_checknumber(L, 1));
	const float high = static_cast<float>(luaL_checknumber(L, 2));
	const int ms = static_cast<int>(luaL_optinteger(L, 3, 200));
	InputMap* map = MapOf(L);
	lua_pushboolean(L, map && map->Rumble(low, high, ms) ? 1 : 0);
	return 1;
}

} // namespace

void InstallInputBindingsApi(lua_State* L, LuaWorld* world) {
	lua_newtable(L);
	const struct { const char* name; lua_CFunction fn; } fns[] = {
		{ "rebind", InputRebind }, { "bindings", InputBindings }, { "save_bindings", InputSaveBindings },
		{ "rumble", InputRumble },
	};
	for (const auto& f : fns) {
		lua_pushlightuserdata(L, world);
		lua_pushcclosure(L, f.fn, 1);
		lua_setfield(L, -2, f.name);
	}
	lua_setglobal(L, "input");
}

} // namespace NoxLua
