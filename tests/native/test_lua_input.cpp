// Lua input action maps (projects/<name>/input.json): parsing and validation, per-frame sampling with
// explicit device readers (no SDL window needed), rebinding with persistence, listen capture, conflicts,
// player routing, and the input.* / ctx:action_* API from Lua.
#include "TestFramework.h"

#include "SceneGraph.h"
#include "assets/AssetIO.h"
#include "scripting/LuaInput.h"
#include "scripting/LuaWorld.h"

#include "lua.h"
#include "lauxlib.h"
#include "json.hpp"

#include <SDL.h>

#include <filesystem>
#include <fstream>
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

	// Fake keyboard, mouse and up to kMaxPads gamepads. Button and axis maps are per pad slot.
	struct FakeDevices {
		std::set<int> keys;
		std::set<int> mouse;
		std::map<int, std::set<int>> buttons;     // pad -> buttons down
		std::map<int, std::map<int, float>> axes; // pad -> axis -> raw value

		NoxLua::InputDevices Devices() {
			NoxLua::InputDevices d;
			d.key = [this](int code) { return keys.count(code) != 0; };
			d.mouse = [this](int code) { return mouse.count(code) != 0; };
			d.padButton = [this](int pad, int code) {
				const auto it = buttons.find(pad);
				return it != buttons.end() && it->second.count(code) != 0;
			};
			d.padAxis = [this](int pad, int code) {
				const auto it = axes.find(pad);
				if (it == axes.end()) return 0.0f;
				const auto a = it->second.find(code);
				return a == it->second.end() ? 0.0f : a->second;
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

	// A temporary asset root with one project input map and a user directory, for the save/reload tests.
	struct AssetRoot {
		std::filesystem::path root;
		explicit AssetRoot(const char* tag) {
			root = std::filesystem::temp_directory_path() / (std::string("nox_input_test_") + tag);
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "projects" / "t");
			std::filesystem::create_directories(root / "user");
		}
		~AssetRoot() {
			AssetIO::Shutdown();
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}
		void Write(const std::string& relative, const std::string& text) const {
			std::ofstream out(root / relative, std::ios::binary);
			out << text;
		}
		bool Start() const {
			AssetIO::Config config;
			config.mode = AssetIO::Mode::Loose;
			config.looseRoot = root;
			config.cacheDir = root / "cache";
			config.userDir = root / "user";
			config.projectName = "t";
			std::string error;
			return AssetIO::Initialize(config, &error);
		}
	};

	const char* kJumpMap =
		"{ \"format\": 1, \"actions\": {"
		"  \"jump\": { \"type\": \"button\", \"keys\": [\"Space\"], \"pad_buttons\": [\"a\"] },"
		"  \"steer\": { \"type\": \"axis\", \"keys_negative\": [\"A\"], \"keys_positive\": [\"D\"], \"pad_axis\": \"leftx\", \"deadzone\": 0.2 }"
		"} }";
}

NOX_TEST(LuaInput, ParsesValidActionMap) {
	std::vector<std::string> errors;
	const NoxLua::InputMap map = LoadMap(
		"{ \"format\": 1, \"actions\": {"
		"  \"accelerate\": { \"type\": \"button\", \"keys\": [\"W\", \"Up\"], \"pad_buttons\": [\"a\"] },"
		"  \"steer\": { \"type\": \"axis\", \"keys_negative\": [\"A\"], \"keys_positive\": [\"D\"], \"pad_axis\": \"leftx\", \"deadzone\": 0.2 },"
		"  \"look\": { \"type\": \"button\", \"mouse_buttons\": [\"right\"] }"
		"} }", errors);
	NOX_CHECK(errors.empty());
	NOX_CHECK_EQ(map.Count(), 3u);
	NOX_CHECK(map.Has("accelerate"));
	NOX_CHECK(map.Has("steer"));
	NOX_CHECK(!map.Has("brake"));
	// Actions are stored in name order (the JSON object's order): accelerate, look, steer.
	NOX_CHECK(map.Actions()[2].kind == NoxLua::InputActionDecl::Kind::Axis);
	NOX_CHECK(map.Actions()[2].deadzone == 0.2f);
	NOX_CHECK(map.Actions()[1].mouseButtons.size() == 1u);
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
		"  \"bad_mouse\": { \"mouse_buttons\": [\"thumb\"] },"
		"  \"ok\": { \"keys\": [\"Space\"] }"
		"} }", errors);
	NOX_CHECK(errors.size() >= 8u);
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
	pad.buttons[0].insert(PadCode("a"));   // the pad button counts too
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

	pad.axes[0][AxisCode("leftx")] = 0.1f;   // inside the deadzone
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") == 0.0f);
	NOX_CHECK(!map.Down("steer"));

	pad.axes[0][AxisCode("leftx")] = 0.6f;   // (0.6 - 0.2) / 0.8 = 0.5
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") > 0.49f && map.Axis("steer") < 0.51f);
	NOX_CHECK(map.Pressed("steer"));

	pad.axes[0][AxisCode("leftx")] = -1.0f;
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

NOX_TEST(LuaInput, RebindPadButtonAndInvertedAxis) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(kJumpMap, errors);
	NOX_REQUIRE(errors.empty());

	std::vector<std::string> conflicts;
	std::string error;
	NoxLua::InputBinding padB;
	padB.type = NoxLua::InputBinding::Type::PadButton;
	padB.name = "b";
	NOX_REQUIRE(map.Rebind("jump", padB, conflicts, error));
	NoxLua::InputActionDecl decl;
	NOX_REQUIRE(map.Bindings("jump", decl));
	NOX_REQUIRE(decl.padButtons.size() == 1u);
	NOX_CHECK_EQ(decl.padButtons[0], std::string("b"));
	NOX_CHECK_EQ(decl.keys.size(), 1u);             // the keyboard binding is kept

	FakeDevices pad;
	pad.buttons[0].insert(PadCode("a"));           // the old pad button no longer jumps
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(!map.Down("jump"));
	pad.buttons[0].clear();
	pad.buttons[0].insert(PadCode("b"));
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Down("jump"));

	// Inverted axis: the stick pushed left now steers right.
	NoxLua::InputBinding axis;
	axis.type = NoxLua::InputBinding::Type::PadAxis;
	axis.name = "leftx";
	axis.invert = true;
	NOX_REQUIRE(map.Rebind("steer", axis, conflicts, error));
	pad.buttons.clear();
	pad.axes.clear();
	pad.axes[0][AxisCode("leftx")] = -0.6f;
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") > 0.49f && map.Axis("steer") < 0.51f);
	pad.axes[0][AxisCode("leftx")] = 0.6f;
	map.SampleWith(pad.Devices(), true);
	NOX_CHECK(map.Axis("steer") < -0.49f && map.Axis("steer") > -0.51f);

	// Composite side: the keyboard key of one side only.
	NoxLua::InputBinding left;
	left.type = NoxLua::InputBinding::Type::Key;
	left.name = "Left";
	left.role = NoxLua::InputBinding::Role::Negative;
	NOX_REQUIRE(map.Rebind("steer", left, conflicts, error));
	NOX_REQUIRE(map.Bindings("steer", decl));
	NOX_REQUIRE(decl.keysNegative.size() == 1u);
	NOX_CHECK_EQ(decl.keysNegative[0], std::string("Left"));

	// Kinds that do not fit the action are refused, with the action kept as it was.
	NOX_CHECK(!map.Rebind("steer", padB, conflicts, error));
	NOX_CHECK(!map.Rebind("jump", axis, conflicts, error));
	NOX_CHECK(!map.Rebind("jump", left, conflicts, error));
	NOX_CHECK(!map.Rebind("missing", padB, conflicts, error));
}

NOX_TEST(LuaInput, RebindsSurviveSaveAndReload) {
	AssetRoot assets("reload");
	assets.Write("projects/t/input.json", kJumpMap);
	NOX_REQUIRE(assets.Start());

	std::vector<std::string> errors;
	NoxLua::InputMap map;
	NOX_REQUIRE(map.LoadFile("projects/t/input.json", errors));
	NOX_REQUIRE(errors.empty());
	std::vector<std::string> conflicts;
	std::string error;
	NoxLua::InputBinding padB;
	padB.type = NoxLua::InputBinding::Type::PadButton;
	padB.name = "b";
	NOX_REQUIRE(map.Rebind("jump", padB, conflicts, error));
	NoxLua::InputBinding inverted;
	inverted.type = NoxLua::InputBinding::Type::PadAxis;
	inverted.name = "leftx";
	inverted.invert = true;
	inverted.deadzone = 0.3f;
	NOX_REQUIRE(map.Rebind("steer", inverted, conflicts, error));
	NOX_CHECK(map.SaveOverrides());

	// A fresh map from the same project file gets the player's bindings back.
	NoxLua::InputMap again;
	errors.clear();
	NOX_REQUIRE(again.LoadFile("projects/t/input.json", errors));
	NOX_CHECK(errors.empty());
	NoxLua::InputActionDecl jump, steer;
	NOX_REQUIRE(again.Bindings("jump", jump));
	NOX_REQUIRE(again.Bindings("steer", steer));
	NOX_REQUIRE(jump.padButtons.size() == 1u);
	NOX_CHECK_EQ(jump.padButtons[0], std::string("b"));
	NOX_CHECK(steer.invert);
	NOX_CHECK(steer.deadzone > 0.29f && steer.deadzone < 0.31f);

	// Reset drops the overrides of one action, and the saved file follows.
	NOX_CHECK(again.ResetBindings("jump", error));
	NOX_CHECK(again.SaveOverrides());
	NoxLua::InputMap third;
	errors.clear();
	NOX_REQUIRE(third.LoadFile("projects/t/input.json", errors));
	NOX_REQUIRE(third.Bindings("jump", jump));
	NOX_CHECK_EQ(jump.padButtons.size(), 1u);
	NOX_CHECK_EQ(jump.padButtons[0], std::string("a"));
	NOX_REQUIRE(third.Bindings("steer", steer));
	NOX_CHECK(steer.invert);
}

NOX_TEST(LuaInput, ResetBindingsRestoresTheAuthoredMap) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(kJumpMap, errors);
	NOX_REQUIRE(errors.empty());
	std::vector<std::string> conflicts;
	std::string error;
	NoxLua::InputBinding key;
	key.name = "I";
	NOX_REQUIRE(map.Rebind("jump", key, conflicts, error));
	NOX_CHECK(!map.ResetBindings("missing", error));
	NOX_CHECK(map.ResetBindings("", error));   // empty name: every action
	NoxLua::InputActionDecl decl;
	NOX_REQUIRE(map.Bindings("jump", decl));
	NOX_REQUIRE(decl.keys.size() == 1u);
	NOX_CHECK_EQ(decl.keys[0], std::string("Space"));
}

NOX_TEST(LuaInput, ListenCapturesTheNextPressNotTheHeldOne) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(kJumpMap, errors);
	NOX_REQUIRE(errors.empty());
	FakeDevices pad;
	pad.keys.insert(KeyCode("Left Shift"));   // already held when the capture starts: does not count
	map.SampleWith(pad.Devices(), true);

	map.BeginListen();
	NOX_CHECK(map.Listening());
	map.SampleWith(pad.Devices(), true);      // baseline sample
	NoxLua::InputMap::Captured captured;
	NOX_CHECK(!map.TakeCapture(captured));

	map.SampleWith(pad.Devices(), true);      // nothing new
	NOX_CHECK(!map.TakeCapture(captured));

	pad.keys.insert(KeyCode("W"));            // a fresh key press
	map.SampleWith(pad.Devices(), true);
	NOX_REQUIRE(map.TakeCapture(captured));
	NOX_CHECK(captured.binding.type == NoxLua::InputBinding::Type::Key);
	NOX_CHECK_EQ(captured.binding.name, std::string("W"));
	NOX_CHECK(!map.Listening());
	NOX_CHECK(!map.TakeCapture(captured));    // one capture per listen

	// Pad buttons and axes are captured too, with the pad slot they came from.
	map.BeginListen();
	map.SampleWith(pad.Devices(), true);
	pad.buttons[2].insert(PadCode("x"));
	map.SampleWith(pad.Devices(), true);
	NOX_REQUIRE(map.TakeCapture(captured));
	NOX_CHECK(captured.binding.type == NoxLua::InputBinding::Type::PadButton);
	NOX_CHECK_EQ(captured.binding.name, std::string("x"));
	NOX_CHECK_EQ(captured.pad, 2);

	map.BeginListen();
	map.SampleWith(pad.Devices(), true);
	pad.axes[1][AxisCode("righttrigger")] = 0.9f;
	map.SampleWith(pad.Devices(), true);
	NOX_REQUIRE(map.TakeCapture(captured));
	NOX_CHECK(captured.binding.type == NoxLua::InputBinding::Type::PadAxis);
	NOX_CHECK_EQ(captured.pad, 1);
	NOX_CHECK_EQ(captured.sign, 1);

	map.CancelListen();
	NOX_CHECK(!map.Listening());
}

NOX_TEST(LuaInput, ConflictsNameTheOtherActionsSharingAnInput) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(
		"{ \"format\": 1, \"actions\": {"
		"  \"jump\": { \"keys\": [\"Space\"], \"pad_buttons\": [\"a\"] },"
		"  \"fire\": { \"keys\": [\"F\"], \"pad_buttons\": [\"x\"] },"
		"  \"crouch\": { \"keys\": [\"Space\"] },"
		"  \"steer\": { \"type\": \"axis\", \"keys_negative\": [\"A\"], \"keys_positive\": [\"D\"] } } }", errors);
	NOX_REQUIRE(errors.empty());
	const std::vector<std::string> jumpConflicts = map.Conflicts("jump");
	NOX_REQUIRE(jumpConflicts.size() == 1u);
	NOX_CHECK_EQ(jumpConflicts[0], std::string("crouch"));
	NOX_CHECK(map.Conflicts("fire").empty());
	NOX_CHECK(map.Conflicts("steer").empty());
	NOX_CHECK(map.Conflicts("missing").empty());

	// Rebinding onto a used key reports the conflict and still applies it.
	std::vector<std::string> conflicts;
	std::string error;
	NoxLua::InputBinding key;
	key.name = "A";
	NOX_REQUIRE(map.Rebind("fire", key, conflicts, error));
	NOX_REQUIRE(conflicts.size() == 1u);
	NOX_CHECK_EQ(conflicts[0], std::string("steer"));
}

NOX_TEST(LuaInput, PlayersRouteGamepadSlotsAndKeyboardStaysPlayerZero) {
	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(
		"{ \"format\": 1, \"actions\": {"
		"  \"fire\": { \"keys\": [\"Space\"], \"pad_buttons\": [\"a\"], \"mouse_buttons\": [\"left\"] },"
		"  \"steer\": { \"type\": \"axis\", \"pad_axis\": \"leftx\", \"keys_positive\": [\"D\"] } } }", errors);
	NOX_REQUIRE(errors.empty());
	FakeDevices pads;
	pads.buttons[1].insert(PadCode("a"));          // slot 1 presses a
	pads.axes[2][AxisCode("leftx")] = 0.9f;        // slot 2 pushes the stick
	map.SampleWith(pads.Devices(), true);
	NOX_CHECK(!map.Down("fire", nullptr, 0));
	NOX_CHECK(map.Down("fire", nullptr, 1));
	NOX_CHECK(!map.Down("fire", nullptr, 2));
	NOX_CHECK(!map.Pressed("fire", nullptr, 3));
	NOX_CHECK(map.Axis("steer", nullptr, 2) > 0.85f);
	NOX_CHECK(map.Axis("steer", nullptr, 0) == 0.0f);
	NOX_CHECK(map.Axis("steer", nullptr, 1) == 0.0f);

	// Keyboard and mouse are player 0 only.
	pads.buttons.clear();
	pads.axes.clear();
	pads.keys.insert(KeyCode("Space"));
	pads.keys.insert(KeyCode("D"));
	pads.mouse.insert(SDL_BUTTON_LEFT);
	map.SampleWith(pads.Devices(), true);
	NOX_CHECK(map.Down("fire", nullptr, 0));
	NOX_CHECK(map.Pressed("fire", nullptr, 0));
	NOX_CHECK(!map.Down("fire", nullptr, 1));
	NOX_CHECK(map.Axis("steer", nullptr, 0) == 1.0f);
	NOX_CHECK(map.Axis("steer", nullptr, 1) == 0.0f);

	// An out-of-range player reads as released.
	NOX_CHECK(!map.Down("fire", nullptr, 4));
	NOX_CHECK(map.Axis("steer", nullptr, -1) == 0.0f);
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
		"  seen_p1 = ctx:action_down('fire', 1)\n"
		"end }\n"
		"nox.system{ name = 'typo', update = function(ctx, dt)\n"
		"  ctx:action_down('fier')\n"
		"end }\n"
		"nox.system{ name = 'badplayer', update = function(ctx, dt)\n"
		"  ctx:action_down('fire', 4)\n"
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
	lua_getglobal(L, "seen_p1");
	NOX_CHECK(lua_toboolean(L, -1) == 0);
	lua_pop(L, 1);

	// The unknown action name is a fault of the system that asked for it, reported with its name.
	bool reported = false;
	bool badPlayer = false;
	for (const auto& d : world.Diagnostics()) {
		if (d.source == "system 'typo'" && d.message.find("unknown input action 'fier'") != std::string::npos) reported = true;
		if (d.source == "system 'badplayer'" && d.message.find("input player must be 0..3") != std::string::npos) badPlayer = true;
	}
	NOX_CHECK(reported);
	NOX_CHECK(badPlayer);
}

NOX_TEST(LuaInput, LuaRebindListenAndConflictsApi) {
	MemorySource source;
	SceneGraph scene;
	NoxLua::LuaWorld world(NoxLua::RuntimeLimits{ 16u * 1024u * 1024u, 2000000 }, &source);
	source.files[kEntry] = "nox.system{ name = 'noop', update = function(ctx, dt) end }\n";
	NoxLua::ProjectContent project;
	project.name = "t";
	project.entry = kEntry;
	project.scriptRoots = { "projects/t/scripts" };
	NOX_REQUIRE(world.Load(project, scene, nlohmann::json::object()));

	std::vector<std::string> errors;
	NoxLua::InputMap map = LoadMap(kJumpMap, errors);
	NOX_REQUIRE(errors.empty());
	world.SetInputs(&map);
	lua_State* L = world.GetRuntime().State();
	const auto run = [L](const char* chunk) {
		const int status = luaL_dostring(L, chunk);
		if (status != LUA_OK) {
			const char* message = lua_tostring(L, -1);
			NoxTest::ReportFailure(__FILE__, __LINE__, message ? message : "lua error", false);
			lua_pop(L, 1);
			return false;
		}
		return true;
	};

	NOX_CHECK(run("ok, conflicts = input.rebind('jump', { pad_button = 'b' }); assert(ok == true and #conflicts == 0)"));
	NOX_CHECK(run("ok, err = input.rebind('jump', { key = 'Q', role = 'negative' }); assert(ok == nil and err ~= nil)"));
	NOX_CHECK(run("ok = input.rebind('steer', { pad_axis = 'righty', invert = true, deadzone = 0.25 }); assert(ok == true)"));
	NOX_CHECK(run("b = input.bindings('jump'); assert(b.pad_buttons[1] == 'b' and b.keys[1] == 'Space')"));
	NOX_CHECK(run("assert(input.binding_name({ pad_button = 'a' }) == 'Pad A')"));
	NOX_CHECK(run("assert(input.binding_name('Space') == 'Space')"));
	NOX_CHECK(run("assert(input.binding_name({ mouse = 'left' }) == 'Mouse Left')"));
	NOX_CHECK(run("assert(input.binding_name({ pad_axis = 'leftx', invert = true }) == 'Pad Left Stick X (inverted)')"));
	NOX_CHECK(run("assert(input.listen_next() == true)"));
	NOX_CHECK(run("assert(input.listen_result() == nil)"));
	NOX_CHECK(run("input.listen_cancel()"));
	NOX_CHECK(run("pads = input.pads(); assert(#pads == 4 and pads[1].slot == 0 and pads[4].slot == 3)"));
	NOX_CHECK(run("ev = input.device_events(); assert(type(ev) == 'table')"));
	NOX_CHECK(run("assert(input.rumble(0.5, 0.5, 100, 0) == false)"));
	NOX_CHECK(run("assert(select(1, pcall(input.rumble, 0.5, 0.5, 100, 9)) == false)"));
	NOX_CHECK(run("assert(input.reset_bindings('jump') == true)"));
	NOX_CHECK(run("assert(input.reset_bindings() == true)"));
	NOX_CHECK(run("assert(input.conflicts('jump') ~= nil and #input.conflicts('jump') == 0)"));

	NoxLua::InputActionDecl decl;
	NOX_REQUIRE(map.Bindings("jump", decl));
	NOX_CHECK_EQ(decl.padButtons[0], std::string("a"));
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
