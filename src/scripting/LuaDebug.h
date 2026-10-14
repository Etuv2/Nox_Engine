#pragma once
// Lua debug drawing (docs/lua_api.md, "Debug draw"): debug.line, debug.box, debug.sphere, debug.text, debug.clear and
// debug.count, a global table that joins the standard library entry (the Lua runtime does not open the standard
// debug library, so the name is free). Primitives are world-space and drawn after the scene in the editor viewport
// and in play mode (src/diagnostics/DebugDraw.h).

struct lua_State;

namespace NoxLua {

void InstallDebugApi(lua_State* L);

} // namespace NoxLua
