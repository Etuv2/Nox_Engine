#include "LuaTime.h"

#include "../TimeScale.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>

namespace NoxLua {

namespace {

// ctx:time_scale([v]): returns the scale; with v, sets it first (clamped to the range, non-finite is an error).
int CtxTimeScale(lua_State* L) {
	if (!lua_isnoneornil(L, 2)) {
		const lua_Number v = luaL_checknumber(L, 2);
		if (!std::isfinite(static_cast<double>(v))) return luaL_error(L, "time_scale must be a finite number");
		TimeScale::Set(static_cast<float>(v));
	}
	lua_pushnumber(L, static_cast<lua_Number>(TimeScale::Get()));
	return 1;
}

} // namespace

void InstallTimeApi(lua_State* L, int ctxIdx) {
	const int idx = lua_absindex(L, ctxIdx);
	lua_pushcfunction(L, CtxTimeScale);
	lua_setfield(L, idx, "time_scale");
}

} // namespace NoxLua
