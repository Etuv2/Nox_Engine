#include "LuaDebug.h"

#include "../diagnostics/DebugDraw.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>
#include <cstdint>

namespace NoxLua {

namespace {

// A point is {x, y, z} or {x = , y = , z = } (any table with three numbers).
glm::vec3 ReadVec3(lua_State* L, int idx, const char* what) {
	if (!lua_istable(L, idx)) luaL_error(L, "%s must be a table {x, y, z}", what);
	float v[3] = { 0.0f, 0.0f, 0.0f };
	static const char* kKeys[3] = { "x", "y", "z" };
	for (int i = 0; i < 3; ++i) {
		lua_rawgeti(L, idx, i + 1);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_getfield(L, idx, kKeys[i]);
		}
		v[i] = static_cast<float>(luaL_checknumber(L, -1));
		lua_pop(L, 1);
	}
	return glm::vec3(v[0], v[1], v[2]);
}

// Colour: 0xAARRGGBB like ed.line3. Values up to 0xFFFFFF are opaque RGB. Default white.
uint32_t ReadColor(lua_State* L, int idx) {
	if (lua_isnoneornil(L, idx)) return 0xFFFFFFFFu;
	const lua_Integer v = luaL_checkinteger(L, idx);
	if (v < 0 || v > 0xFFFFFFFFLL) luaL_error(L, "colour must be 0x000000..0xFFFFFFFF (0xAARRGGBB)");
	const uint32_t c = static_cast<uint32_t>(v);
	return c <= 0xFFFFFFu ? (0xFF000000u | c) : c;
}

// Lifetime in real seconds; 0 (default) is this frame only.
float ReadDuration(lua_State* L, int idx) {
	if (lua_isnoneornil(L, idx)) return 0.0f;
	const lua_Number d = luaL_checknumber(L, idx);
	if (!std::isfinite(static_cast<double>(d))) luaL_error(L, "duration must be a finite number of seconds");
	return static_cast<float>(d);
}

// debug.line(a, b, colour, duration)
int DebugLine(lua_State* L) {
	const glm::vec3 a = ReadVec3(L, 1, "debug.line start");
	const glm::vec3 b = ReadVec3(L, 2, "debug.line end");
	DebugDraw::AddLine(a, b, ReadColor(L, 3), ReadDuration(L, 4));
	return 0;
}

// debug.box(center, half, colour, duration): half is {x, y, z} or a single number (cube half size).
int DebugBox(lua_State* L) {
	const glm::vec3 center = ReadVec3(L, 1, "debug.box center");
	glm::vec3 half(0.5f);
	if (lua_type(L, 2) == LUA_TNUMBER) {
		half = glm::vec3(static_cast<float>(lua_tonumber(L, 2)));
	}
	else {
		half = ReadVec3(L, 2, "debug.box half extents");
	}
	DebugDraw::AddBox(center, half, ReadColor(L, 3), ReadDuration(L, 4));
	return 0;
}

// debug.sphere(center, radius, colour, duration)
int DebugSphere(lua_State* L) {
	const glm::vec3 center = ReadVec3(L, 1, "debug.sphere center");
	const float radius = static_cast<float>(luaL_checknumber(L, 2));
	DebugDraw::AddSphere(center, radius, ReadColor(L, 3), ReadDuration(L, 4));
	return 0;
}

// debug.text(position, text, colour, duration)
int DebugText(lua_State* L) {
	const glm::vec3 p = ReadVec3(L, 1, "debug.text position");
	const char* text = luaL_checkstring(L, 2);
	DebugDraw::AddText(p, text, ReadColor(L, 3), ReadDuration(L, 4));
	return 0;
}

int DebugClear(lua_State* L) {
	(void)L;
	DebugDraw::Clear();
	return 0;
}

int DebugCount(lua_State* L) {
	lua_pushinteger(L, static_cast<lua_Integer>(DebugDraw::Count()));
	return 1;
}

} // namespace

void InstallDebugApi(lua_State* L) {
	lua_getglobal(L, "debug");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
	}
	const struct { const char* name; lua_CFunction fn; } fns[] = {
		{ "line", DebugLine }, { "box", DebugBox }, { "sphere", DebugSphere }, { "text", DebugText },
		{ "clear", DebugClear }, { "count", DebugCount },
	};
	for (const auto& f : fns) {
		lua_pushcfunction(L, f.fn);
		lua_setfield(L, -2, f.name);
	}
	lua_setglobal(L, "debug");
}

} // namespace NoxLua
