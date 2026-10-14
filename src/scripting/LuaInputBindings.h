#pragma once
// Lua input control (docs/lua_api.md, "Input control"): a global `input` table next to the action queries of ctx.
//   input.rebind(action, key_name)   -> true | nil, error   (button actions; keys are SDL key names)
//   input.bindings(action)           -> { keys = {...}, pad_buttons = {...} } | nil, error
//   input.save_bindings()            -> true | false          (writes the rebinds to the user directory)
//   input.rumble(low, high, ms)      -> true | false          (gamepad motors 0..1; false without a pad)
// The functions act on the action map of the running project (LuaWorld::InputMapPtr).

struct lua_State;

namespace NoxLua {

class LuaWorld;

void InstallInputBindingsApi(lua_State* L, LuaWorld* world);

} // namespace NoxLua
