#pragma once
// Lua input control (docs/lua_api.md, "Input control"): a global `input` table next to the action queries of ctx.
//   input.actions()                  -> { name, ... }
//   input.rebind(action, binding)    -> true, conflicts | nil, error   (binding: see docs/lua_api.md, Input)
//   input.reset_bindings([action])   -> true | nil, error
//   input.bindings(action)           -> { keys, mouse_buttons, pad_buttons, keys_negative, keys_positive, pad_axis,
//                                         invert, deadzone, labels } | nil, error
//   input.conflicts(action)          -> { other_action, ... } | nil, error
//   input.binding_name(binding)      -> "Space", "Mouse Left", "Pad A", ...
//   input.listen_next() / listen_result() / listen_cancel()   (capture of the next press, polled)
//   input.pads() / input.device_events()                      (gamepad slots and hot-plug changes)
//   input.save_bindings()            -> true | false          (writes the rebinds to the user directory)
//   input.rumble(low, high, ms, pad) -> true | false          (gamepad motors 0..1; false without a pad)
// The functions act on the action map of the running project (LuaWorld::InputMapPtr).

struct lua_State;

namespace NoxLua {

class LuaWorld;

void InstallInputBindingsApi(lua_State* L, LuaWorld* world);

} // namespace NoxLua
