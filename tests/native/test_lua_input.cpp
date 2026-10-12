// Lua input action maps (projects/<name>/input.json): parsing and validation, per-frame sampling with
// explicit device readers (no SDL window needed), and the ctx:action_* / ctx:axis queries from Lua.
#include "TestFramework.h"

#include "SceneGraph.h"
#include "scripting/LuaInput.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "json.hpp"

#include <SDL.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
	class MemorySource : public NoxLua::ScriptSource {
	public:
		std::map<std::string, std::string> files;
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override {
			const auto it = files.find(logicalPath);
			if (it == files.end()) {
				error = "no such file";
				return false;
			}
			text = it->second;
			return true;
		}
	};

	const char* kEntry = "projects/t/scripts/main.lua";

	// Fake keyboard and pad: the test sets which codes are down and the axis values.
	struct FakeDevices {
		std::set<int> keys;
		std::set<int> buttons;
		std::map<int, float> axes;

		NoxLua::InputDevices Devices() {
			NoxLua::InputDevices d;
			d.key = [this](int code) { return keys.count(code) != 0; };
			d.padButton = [this](int code) { return buttons.count(code) != 0; };
			d.padAxis = [this](int code) {
				const auto it = axes.find(code);
				return it == axes.end() ? 0.0f : it->second;
			};
			return d;
		}
	};

	NoxLua::InputMap LoadMap(const std::string& json, std::vector<std::string>& errors) {
		NoxLua::InputMap map;
		map.Parse(json, errors);
		return map;
	}

	// SDL codes for the names used below (resolved through the same SDL name functions the engine uses).
	int KeyCode(const char* name);
	int PadCode(const char* name);
	int AxisCode(const char* name);
}

NOX_TEST(LuaInput, ParsesValidActionMap) {
	std::vector<std::string> errors;
	const NoxLua::InputMap map = LoadMap(
		"{ \"format\": 1, \"actions\": {"
		"  \"accelerate\": { \"type\": \"button\", \"keys\": [\"W\", \"Up\"], \"pad_buttons\": [\"a\"] },"
		"  \"steer\": { \"type\": \"axis\", \"keys_negative\": [\"A\"], \"keys_positive\": [\"D\"], \"pad_axis\": \"leftx\", \"deadzone\": 0.2 }"
		"} }", errors);
	NOX_CHECK(errors.empty());
	NOX_CHECK_EQ(map.Count(), 2u);
	NOX_CHECK(map.Has("accelerate"));
	NOX_CHECK(map.Has("steer"));
	NOX_CHECK(!map.Has("brake"));
	NOX_CHECK(map.Actions()[1].kind == NoxLua::InputActionDecl::Kind::Axis);
	NOX_CHECK(map.Actions()[1].deadzone == 0.2f);
}

NOX_TEST(LuaInput, ReportsInvalidEntries) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(
		"{ \"format\": 1, \"actions\": {"
		"  \"bad_key\": { \"keys\": [\"NotAKey\"] },"
		"  \"bad_pad\": { \"pad_buttons\": [\"notabutton\"] },"
		"  \"bad_axis\": { \"type\": \"axis\", \"pad_axis\": \"nope\" },"
		"  \"mixed\": { \"keys\": [\"W\"], \"keys_positive\": [\"D\"] },"
		"  \"no_binding\": { },"
		"  \"bad_dz\": { \"keys\": [\"W\"], \"deadzone\": 0.95 },"
		"  \"bad_name!\": { \"keys\": [\"W\"] },"
		"  \"ok\": { \"keys\": [\"Space\"] }"
		"} }", errors);
	NOX_CHECK(errors.size() >= 7u);
	NOX_CHECK(map.Has("ok"));
	NOX_CHECK(!map.Has("bad_key"));
	NOX_CHECK(!map.Has("bad_name!"));
	NOX_CHECK_EQ(map.Count(), 1u);

	errors.clear();
	NoxLua::InputMap wrongFormat = LoadMap("{ \"format\": 2, \"actions\": {} }", errors);
	NOX_CHECK(!errors.empty());
	errors.clear();
	NoxLua::InputMap notJson = LoadMap("{ nope", errors);
	NOX_CHECK(!errors.empty());
	NOX_CHECK_EQ(notJson.Count(), 0u);
}

NOX_TEST(LuaInput, ButtonPressedAndReleasedAreEdgeTriggered) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap("{ \"format\": 1, \"actions\": { \"fire\": { \"keys\": [\"Space\"], \"pad_buttons\": [\"a\"] } } }", errors);
	NOX_REQUIRE(errors.empty());
	FakeDevices pad;
	const NoxLua::InputDevices devices = pad.Devices();

	map.SampleWith(devices, true);
	NOX_CHECK(!map.Down("fire"));
	NOX_CHECK(!map.Pressed("fire"));

	pad.keys.insert(KeyCode("Space"));
	map.SampleWith(devices, true);
	NOX_CHECK(map.Down("fire"));
	NOX_CHECK(map.Pressed("fire"));
	NOX_CHECK(!map.Released("fire"));
	NOX_CHECK(map.Axis("fire") == 1.0f);

	map.SampleWith(devices, true);   // held: down but not pressed again
	NOX_CHECK(map.Down("fire"));
	NOX_CHECK(!map.Pressed("fire"));

	pad.keys.clear();
	pad.buttons.insert(PadCode("a"));   // the pad button counts too
	map.SampleWith(devices, true);
	NOX_CHECK(map.Down("fire"));
	NOX_CHECK(!map.Released("fire"));

	pad.buttons.clear();
	map.SampleWith(devices, true);
	NOX_CHECK(!map.Down("fire"));
	NOX_CHECK(map.Released("fire"));
}

NOX_TEST(LuaInput, AxisUsesDeadzoneAndKeys) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(
		"{ \"format\": 1, \"actions\": { \"steer\": { \"type\": \"axis\", \"keys_negative\": [\"A\"], \"keys_positive\": [\"D\"], \"pad_axis\": \"leftx\", \"deadzone\": 0.2 } } }",
		errors);
	NOX_REQUIRE(errors.empty());
	FakeDevices pad;

	pad.axes[AxisCode("leftx")] = 0.1f;   // inside the deadzone
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") == 0.0f);
	NOX_CHECK(!map.Down("steer"));

	pad.axes[AxisCode("leftx")] = 0.6f;   // (0.6 - 0.2) / 0.8 = 0.5
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") > 0.49f && map.Axis("steer") < 0.51f);
	NOX_CHECK(map.Pressed("steer"));

	pad.axes[AxisCode("leftx")] = -1.0f;
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") == -1.0f);

	pad.axes.clear();
	pad.keys.insert(KeyCode("D"));
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") == 1.0f);
	pad.keys.insert(KeyCode("A"));   // opposite keys cancel
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") == 0.0f);
}

NOX_TEST(LuaInput, DisabledInputReadsReleased) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap("{ \"format\": 1, \"actions\": { \"fire\": { \"keys\": [\"Space\"] } } }", errors);
	NOX_REQUIRE(errors.empty());
	FakeDevices pad;
	pad.keys.insert(KeyCode("Space"));
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Down("fire"));
	map.SampleWith(pad.Devices(), false);   // no focus: released, and the release is reported once
	NOX_CHECK(!map.Down("fire"));
	NOX_CHECK(map.Released("fire"));
	map.SampleWith(pad.Devices(), false);
	NOX_CHECK(!map.Released("fire"));
}

NOX_TEST(LuaInput, ScriptsQueryLatchedActions) {
	MemorySource source;
	SceneGraph scene;
	NoxLua::LuaWorld world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source);
	source.files[kEntry] =
		"nox.system{ name = 'steer', update = function(ctx, dt)\n"
		"  seen_down = ctx:action_down('fire')\n"
		"  seen_pressed = ctx:action_pressed('fire')\n"
		"  seen_released = ctx:action_released('fire')\n"
		"  seen_axis = ctx:axis('steer')\n"
		"end }\n"
		"nox.system{ name = 'typo', update = function(ctx, dt)\n"
		"  ctx:action_down('fier')\n"
		"end }\n";
	NoxLua::ProjectContent project;
	project.name = "t";
	project.entry = kEntry;
	project.scriptRoots = { "projects/t/scripts" };
	NOX_REQUIRE(world.Load(project, scene, nlohmann::json::object()));

	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(
		"{ \"format\": 1, \"actions\": {"
		"  \"fire\": { \"keys\": [\"Space\"] },"
		"  \"steer\": { \"type\": \"axis\", \"keys_positive\": [\"D\"] } } }", errors);
	NOX_REQUIRE(errors.empty());
	world.SetInputs(&map);

	FakeDevices pad;
	pad.keys.insert(KeyCode("Space"));
	pad.keys.insert(KeyCode("D"));
	map.SampleWith(pad.Devices(), true);
	world.BeginPlay();
	world.Update(1.0f / 60.0f, true);

	lua_State* L = world.GetRuntime().State();
	lua_getglobal(L, "seen_down");
	NOX_CHECK(lua_toboolean(L, -1) != 0);
	lua_pop(L, 1);
	lua_getglobal(L, "seen_pressed");
	NOX_CHECK(lua_toboolean(L, -1) != 0);
	lua_pop(L, 1);
	lua_getglobal(L, "seen_axis");
	NOX_CHECK(lua_tonumber(L, -1) == 1.0);
	lua_pop(L, 1);

	// The unknown action name is a fault of the system that asked for it, reported with its name.
	bool reported = false;
	for (const auto& d : world.Diagnostics()) {
		if (d.source == "system 'typo'" && d.message.find("unknown input action 'fier'") != std::string::npos) reported = true;
	}
	NOX_CHECK(reported);
}

namespace {
	int KeyCode(const char* name) {
		return static_cast<int>(SDL_GetScancodeFromName(name));
	}
	int PadCode(const char* name) {
		return static_cast<int>(SDL_GameControllerGetButtonFromString(name));
	}
	int AxisCode(const char* name) {
		return static_cast<int>(SDL_GameControllerGetAxisFromString(name));
	}
}
