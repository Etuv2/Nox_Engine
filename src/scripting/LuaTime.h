#pragma once
// Lua time control (docs/lua_api.md, "Time"): ctx:time_scale([v]) reads or sets the global game time scale
// (src/TimeScale.h). The scale applies to game, animation, physics and the fixed/variable Lua systems; the editor
// UI and debug draw lifetimes keep real time.

struct lua_State;

namespace NoxLua {

// Adds time_scale to the ctx table at ctxIdx.
void InstallTimeApi(lua_State* L, int ctxIdx);

} // namespace NoxLua
