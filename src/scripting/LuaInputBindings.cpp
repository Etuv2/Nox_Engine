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

void SetString(lua_State* L, const char* field, const std::string& value) {
	lua_pushstring(L, value.c_str());
	lua_setfield(L, -2, field);
}

// Binding table <-> InputBinding. Shape: { key = "Space", role = "negative" } | { mouse = "left" } |
// { pad_button = "a" } | { pad_axis = "leftx", invert = true, deadzone = 0.2 }; a plain string is a key name.
bool ReadBinding(lua_State* L, int idx, InputBinding& out, std::string& error) {
	if (lua_type(L, idx) == LUA_TSTRING) {
		out = InputBinding{};
		out.name = lua_tostring(L, idx);
		return true;
	}
	if (lua_type(L, idx) != LUA_TTABLE) {
		error = "binding must be a key name or a table with key, mouse, pad_button or pad_axis";
		return false;
	}
	out = InputBinding{};
	const auto field = [L, idx](const char* name) {
		lua_getfield(L, idx, name);
		return lua_gettop(L);
	};
	int top = field("key");
	const bool hasKey = lua_type(L, top) == LUA_TSTRING;
	const std::string key = hasKey ? lua_tostring(L, top) : std::string();
	lua_pop(L, 1);
	top = field("mouse");
	const bool hasMouse = lua_type(L, top) == LUA_TSTRING;
	const std::string mouse = hasMouse ? lua_tostring(L, top) : std::string();
	lua_pop(L, 1);
	top = field("pad_button");
	const bool hasPadButton = lua_type(L, top) == LUA_TSTRING;
	const std::string padButton = hasPadButton ? lua_tostring(L, top) : std::string();
	lua_pop(L, 1);
	top = field("pad_axis");
	const bool hasPadAxis = lua_type(L, top) == LUA_TSTRING;
	const std::string padAxis = hasPadAxis ? lua_tostring(L, top) : std::string();
	lua_pop(L, 1);
	const int count = (hasKey ? 1 : 0) + (hasMouse ? 1 : 0) + (hasPadButton ? 1 : 0) + (hasPadAxis ? 1 : 0);
	if (count != 1) {
		error = "binding table needs exactly one of key, mouse, pad_button, pad_axis (as a string)";
		return false;
	}
	if (hasKey) {
		out.type = InputBinding::Type::Key;
		out.name = key;
		top = field("role");
		const std::string role = lua_type(L, top) == LUA_TSTRING ? lua_tostring(L, top) : std::string();
		lua_pop(L, 1);
		if (role == "negative") out.role = InputBinding::Role::Negative;
		else if (role == "positive") out.role = InputBinding::Role::Positive;
		else if (!role.empty()) {
			error = "role must be \"negative\" or \"positive\"";
			return false;
		}
	}
	else if (hasMouse) {
		out.type = InputBinding::Type::Mouse;
		out.name = mouse;
	}
	else if (hasPadButton) {
		out.type = InputBinding::Type::PadButton;
		out.name = padButton;
	}
	else {
		out.type = InputBinding::Type::PadAxis;
		out.name = padAxis;
		top = field("invert");
		out.invert = lua_toboolean(L, top) != 0;
		lua_pop(L, 1);
		top = field("deadzone");
		if (lua_type(L, top) == LUA_TNUMBER) out.deadzone = static_cast<float>(lua_tonumber(L, top));
		lua_pop(L, 1);
	}
	return true;
}

void PushBinding(lua_State* L, const InputBinding& b) {
	lua_createtable(L, 0, 4);
	switch (b.type) {
	case InputBinding::Type::Key:
		SetString(L, "key", b.name);
		if (b.role != InputBinding::Role::Single) {
			SetString(L, "role", b.role == InputBinding::Role::Negative ? "negative" : "positive");
		}
		break;
	case InputBinding::Type::Mouse: SetString(L, "mouse", b.name); break;
	case InputBinding::Type::PadButton: SetString(L, "pad_button", b.name); break;
	case InputBinding::Type::PadAxis:
		SetString(L, "pad_axis", b.name);
		lua_pushboolean(L, b.invert ? 1 : 0);
		lua_setfield(L, -2, "invert");
		break;
	}
	SetString(L, "label", BindingLabel(b));
}

// input.rebind(action, binding) -> true, conflicts | nil, error
int InputRebind(lua_State* L) {
	const std::string action = luaL_checkstring(L, 1);
	InputBinding binding;
	std::string error;
	if (!ReadBinding(L, 2, binding, error)) return NilError(L, error);
	InputMap* map = MapOf(L);
	if (!map) return NilError(L, "no action map is loaded");
	std::vector<std::string> conflicts;
	if (!map->Rebind(action, binding, conflicts, error)) return NilError(L, error);
	lua_pushboolean(L, 1);
	PushNames(L, conflicts);
	return 2;
}

// input.actions() -> { name, ... } in the order of the action map (name order: the JSON object's order)
int InputActions(lua_State* L) {
	InputMap* map = MapOf(L);
	std::vector<std::string> names;
	if (map) {
		for (const auto& decl : map->Actions()) names.push_back(decl.name);
	}
	PushNames(L, names);
	return 1;
}

// input.reset_bindings([action]) -> true | nil, error   (no action: every action)
int InputResetBindings(lua_State* L) {
	const std::string action = lua_isnoneornil(L, 1) ? std::string() : luaL_checkstring(L, 1);
	InputMap* map = MapOf(L);
	if (!map) return NilError(L, "no action map is loaded");
	std::string error;
	if (!map->ResetBindings(action, error)) return NilError(L, error);
	lua_pushboolean(L, 1);
	return 1;
}

// input.bindings(action) -> { keys, mouse_buttons, pad_buttons, keys_negative, keys_positive, pad_axis, invert,
//                             deadzone, labels } | nil, error
int InputBindings(lua_State* L) {
	const std::string action = luaL_checkstring(L, 1);
	InputMap* map = MapOf(L);
	if (!map) return NilError(L, "no action map is loaded");
	InputActionDecl decl;
	if (!map->Bindings(action, decl)) return NilError(L, "unknown input action '" + action + "'");
	lua_createtable(L, 0, 9);
	PushNames(L, decl.keys);
	lua_setfield(L, -2, "keys");
	PushNames(L, decl.mouseButtons);
	lua_setfield(L, -2, "mouse_buttons");
	PushNames(L, decl.padButtons);
	lua_setfield(L, -2, "pad_buttons");
	PushNames(L, decl.keysNegative);
	lua_setfield(L, -2, "keys_negative");
	PushNames(L, decl.keysPositive);
	lua_setfield(L, -2, "keys_positive");
	if (!decl.padAxis.empty()) SetString(L, "pad_axis", decl.padAxis);
	lua_pushboolean(L, decl.invert ? 1 : 0);
	lua_setfield(L, -2, "invert");
	lua_pushnumber(L, decl.deadzone);
	lua_setfield(L, -2, "deadzone");
	std::vector<std::string> labels;
	const auto add = [&labels](InputBinding::Type type, const std::string& name, InputBinding::Role role = InputBinding::Role::Single) {
		InputBinding b;
		b.type = type;
		b.name = name;
		b.role = role;
		labels.push_back(BindingLabel(b));
	};
	for (const auto& k : decl.keys) add(InputBinding::Type::Key, k);
	for (const auto& k : decl.keysNegative) add(InputBinding::Type::Key, k, InputBinding::Role::Negative);
	for (const auto& k : decl.keysPositive) add(InputBinding::Type::Key, k, InputBinding::Role::Positive);
	for (const auto& m : decl.mouseButtons) add(InputBinding::Type::Mouse, m);
	for (const auto& b : decl.padButtons) add(InputBinding::Type::PadButton, b);
	if (!decl.padAxis.empty()) {
		InputBinding b;
		b.type = InputBinding::Type::PadAxis;
		b.name = decl.padAxis;
		b.invert = decl.invert;
		labels.push_back(BindingLabel(b));
	}
	PushNames(L, labels);
	lua_setfield(L, -2, "labels");
	return 1;
}

// input.conflicts(action) -> { other_action, ... }
int InputConflicts(lua_State* L) {
	const std::string action = luaL_checkstring(L, 1);
	InputMap* map = MapOf(L);
	if (!map) return NilError(L, "no action map is loaded");
	InputActionDecl decl;
	if (!map->Bindings(action, decl)) return NilError(L, "unknown input action '" + action + "'");
	PushNames(L, map->Conflicts(action));
	return 1;
}

// input.binding_name(binding) -> "Space", "Mouse Left", "Pad A", "Pad Left Stick X (inverted)" ...
int InputBindingName(lua_State* L) {
	InputBinding binding;
	std::string error;
	if (!ReadBinding(L, 1, binding, error)) return NilError(L, error);
	lua_pushstring(L, BindingLabel(binding).c_str());
	return 1;
}

// input.save_bindings() -> true | false
int InputSaveBindings(lua_State* L) {
	InputMap* map = MapOf(L);
	lua_pushboolean(L, map && map->SaveOverrides() ? 1 : 0);
	return 1;
}

// input.listen_next()  starts a capture of the next press (a key, mouse button, pad button or pad axis that is not
//                      already held). Returns true, or nil, error when no map is loaded.
int InputListenNext(lua_State* L) {
	InputMap* map = MapOf(L);
	if (!map) return NilError(L, "no action map is loaded");
	map->BeginListen();
	lua_pushboolean(L, 1);
	return 1;
}

// input.listen_result() -> binding table (with pad = slot and sign = +1/-1 for axes) once something was pressed,
//                          else nil. The capture ends when a result is taken.
int InputListenResult(lua_State* L) {
	InputMap* map = MapOf(L);
	InputMap::Captured captured;
	if (!map || !map->TakeCapture(captured)) {
		lua_pushnil(L);
		return 1;
	}
	PushBinding(L, captured.binding);
	if (captured.pad >= 0) {
		lua_pushinteger(L, captured.pad);
		lua_setfield(L, -2, "pad");
	}
	if (captured.sign != 0) {
		lua_pushinteger(L, captured.sign);
		lua_setfield(L, -2, "sign");
	}
	return 1;
}

// input.listen_cancel()
int InputListenCancel(lua_State* L) {
	if (InputMap* map = MapOf(L)) map->CancelListen();
	return 0;
}

// input.pads() -> { { slot = 0, connected = true, name = "Xbox..." }, ... } for the kMaxPads slots
int InputPads(lua_State* L) {
	lua_createtable(L, kMaxPads, 0);
	for (int slot = 0; slot < kMaxPads; ++slot) {
		lua_createtable(L, 0, 3);
		lua_pushinteger(L, slot);
		lua_setfield(L, -2, "slot");
		const bool connected = GamepadConnected(slot);
		lua_pushboolean(L, connected ? 1 : 0);
		lua_setfield(L, -2, "connected");
		SetString(L, "name", connected ? GamepadName(slot) : std::string());
		lua_rawseti(L, -2, slot + 1);
	}
	return 1;
}

// input.device_events() -> { { connected = true, player = 0, name = "..." }, ... } since the last call
int InputDeviceEvents(lua_State* L) {
	InputMap* map = MapOf(L);
	const std::vector<InputMap::DeviceEvent> events = map ? map->TakeDeviceEvents() : std::vector<InputMap::DeviceEvent>{};
	lua_createtable(L, static_cast<int>(events.size()), 0);
	for (size_t i = 0; i < events.size(); ++i) {
		lua_createtable(L, 0, 3);
		lua_pushboolean(L, events[i].connected ? 1 : 0);
		lua_setfield(L, -2, "connected");
		lua_pushinteger(L, events[i].player);
		lua_setfield(L, -2, "player");
		SetString(L, "name", events[i].name);
		lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
	}
	return 1;
}

// input.rumble(low, high, ms, pad) -> true | false          (motors 0..1; pad = slot, default 0)
int InputRumble(lua_State* L) {
	const float low = static_cast<float>(luaL_checknumber(L, 1));
	const float high = static_cast<float>(luaL_checknumber(L, 2));
	const int ms = static_cast<int>(luaL_optinteger(L, 3, 200));
	const int pad = static_cast<int>(luaL_optinteger(L, 4, 0));
	if (pad < 0 || pad >= kMaxPads) return luaL_error(L, "pad slot must be 0..%d", kMaxPads - 1);
	InputMap* map = MapOf(L);
	lua_pushboolean(L, map && map->Rumble(low, high, ms, pad) ? 1 : 0);
	return 1;
}

} // namespace

void InstallInputBindingsApi(lua_State* L, LuaWorld* world) {
	lua_newtable(L);
	const struct { const char* name; lua_CFunction fn; } fns[] = {
		{ "rebind", InputRebind }, { "actions", InputActions }, { "reset_bindings", InputResetBindings }, { "bindings", InputBindings },
		{ "conflicts", InputConflicts }, { "binding_name", InputBindingName }, { "save_bindings", InputSaveBindings },
		{ "listen_next", InputListenNext }, { "listen_result", InputListenResult },
		{ "listen_cancel", InputListenCancel }, { "pads", InputPads }, { "device_events", InputDeviceEvents },
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
