// Input Actions model (src/editor/InputActionsModel.*): load / edit / save round trip of an action map file, the
// stable format, undo through snapshots, validation (runtime parser errors, duplicate names, shared inputs), and the
// engine-owned editor action map (config/editor_input.json).
#include "TestFramework.h"

#include "editor/InputActionsModel.h"
#include "scripting/LuaInput.h"

#include <SDL.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
	namespace fs = std::filesystem;
	using nox::editor::InputActionsModel;
	using nox::editor::InputIssue;

	const char* kArena =
		"{\n"
		"  \"format\": 1,\n"
		"  \"actions\": {\n"
		"    \"accelerate\": { \"type\": \"button\", \"keys\": [\"W\", \"Up\"], \"pad_buttons\": [\"a\"] },\n"
		"    \"steer\": { \"type\": \"axis\", \"keys_negative\": [\"A\", \"Left\"], \"keys_positive\": [\"D\", \"Right\"], \"pad_axis\": \"leftx\", \"deadzone\": 0.15 }\n"
		"  }\n"
		"}\n";

	size_t CountIssues(const std::vector<InputIssue>& issues, InputIssue::Severity severity) {
		size_t n = 0;
		for (const InputIssue& i : issues) n += i.severity == severity ? 1 : 0;
		return n;
	}

	bool Contains(const std::string& text, const std::string& part) {
		return text.find(part) != std::string::npos;
	}
}

NOX_TEST(InputActionsModel, RoundTripKeepsOrderAndFormat) {
	InputActionsModel model;
	std::string error;
	NOX_REQUIRE(model.Load(kArena, error));
	const std::string first = model.Serialize();
	NOX_CHECK(Contains(first, "    \"accelerate\": { \"type\": \"button\", \"keys\": [\"W\", \"Up\"], \"pad_buttons\": [\"a\"] }"));
	NOX_CHECK(first.find("\"accelerate\"") < first.find("\"steer\""));

	// Reloading the written text and writing it again changes nothing.
	InputActionsModel again;
	NOX_REQUIRE(again.Load(first, error));
	NOX_CHECK_EQ(again.Serialize(), first);
	NOX_CHECK(again.Validate().empty());
}

NOX_TEST(InputActionsModel, RejectsFilesThatAreNotActionMaps) {
	InputActionsModel model;
	std::string error;
	NOX_CHECK(!model.Load("[1, 2]", error));
	NOX_CHECK(!model.Load("{ \"format\": 2, \"actions\": {} }", error));
	NOX_CHECK(!model.Load("{ \"format\": 1, \"actions\": [] }", error));
	NOX_CHECK(!model.Load("{ \"format\": 1, ", error));
}

NOX_TEST(InputActionsModel, AddRenameRemoveAction) {
	InputActionsModel model;
	std::string error;
	NOX_REQUIRE(model.Load(kArena, error));
	NOX_CHECK(model.AddAction("brake", "button", error));
	NOX_CHECK(!model.AddAction("brake", "button", error));          // duplicate
	NOX_CHECK(!model.AddAction("1st", "button", error));            // identifier rules
	NOX_CHECK(!model.AddAction("x.y", "button", error));
	NOX_CHECK(!model.AddAction("ok", "hold", error));               // type
	NOX_CHECK(model.RenameAction("accelerate", "gas", error));
	NOX_CHECK(!model.RenameAction("gas", "steer", error));          // taken
	NOX_CHECK(!model.RenameAction("missing", "other", error));
	// Rename keeps the action's place in the file.
	const std::vector<std::string> names = model.ActionNames();
	NOX_REQUIRE(names.size() == 3);
	NOX_CHECK_EQ(names[0], std::string("gas"));
	NOX_CHECK_EQ(names[2], std::string("brake"));
	NOX_CHECK(model.RemoveAction("steer", error));
	NOX_CHECK(!model.HasAction("steer"));
	NOX_CHECK(!model.RemoveAction("steer", error));
}

NOX_TEST(InputActionsModel, BindingsAreCheckedAgainstTheEngineNames) {
	InputActionsModel model;
	std::string error;
	NOX_REQUIRE(model.Load(kArena, error));
	NOX_CHECK(model.AddBinding("accelerate", "keys", "Space", error));
	NOX_CHECK(!model.AddBinding("accelerate", "keys", "space", error));       // already bound (case-insensitive)
	NOX_CHECK(!model.AddBinding("accelerate", "keys", "NoSuchKey", error));
	NOX_CHECK(!model.AddBinding("accelerate", "pad_buttons", "turbo", error));
	NOX_CHECK(!model.AddBinding("accelerate", "keys_positive", "Q", error));  // axis field on a button
	NOX_CHECK(model.AddBinding("accelerate", "mouse_buttons", "left", error));
	NOX_CHECK(!model.AddBinding("accelerate", "mouse_buttons", "wheel", error));
	NOX_CHECK(model.AddBinding("steer", "pad_axis", "righttrigger", error));  // replaces leftx
	NOX_CHECK(!model.AddBinding("steer", "keys", "Q", error));                 // button field on an axis
	NOX_CHECK(model.RemoveBinding("accelerate", "keys", 0, error));
	NOX_CHECK(!model.RemoveBinding("accelerate", "keys", 5, error));
	NOX_CHECK(model.RemoveBinding("steer", "pad_axis", 0, error));
	NOX_CHECK(!model.Action("steer").contains("pad_axis"));

	const std::string text = model.Serialize();
	NOX_CHECK(Contains(text, "\"keys\": [\"Up\", \"Space\"]"));
	NOX_CHECK(Contains(text, "\"mouse_buttons\": [\"left\"]"));
}

NOX_TEST(InputActionsModel, DeadzoneAndInvertNeedAnAxis) {
	InputActionsModel model;
	std::string error;
	NOX_REQUIRE(model.Load(kArena, error));
	NOX_CHECK(model.SetDeadzone("steer", 0.25, error));
	NOX_CHECK(!model.SetDeadzone("steer", 0.95, error));
	NOX_CHECK(!model.SetDeadzone("accelerate", 0.1, error));
	NOX_CHECK(model.SetInvert("steer", true, error));
	NOX_CHECK(!model.SetInvert("accelerate", true, error));
	NOX_CHECK(Contains(model.Serialize(), "\"invert\": true"));
	NOX_CHECK(Contains(model.Serialize(), "\"deadzone\": 0.25"));
}

NOX_TEST(InputActionsModel, UndoRestoresTheSnapshot) {
	InputActionsModel model;
	std::string error;
	NOX_REQUIRE(model.Load(kArena, error));
	const std::string before = model.Serialize();
	const auto snapshot = model.Snapshot();
	NOX_CHECK(model.RemoveAction("accelerate", error));
	NOX_CHECK(model.AddBinding("steer", "keys_negative", "S", error));
	NOX_CHECK(model.Serialize() != before);
	model.Restore(snapshot);
	NOX_CHECK_EQ(model.Serialize(), before);
}

NOX_TEST(InputActionsModel, ValidateReportsErrorsAndSharedInputs) {
	InputActionsModel model;
	std::string error;
	NOX_REQUIRE(model.Load(kArena, error));
	NOX_CHECK(model.Validate().empty());

	// Shared input is a warning: both actions fire.
	NOX_CHECK(model.AddAction("boost", "button", error));
	NOX_CHECK(model.AddBinding("boost", "keys", "w", error));
	auto issues = model.Validate();
	NOX_CHECK_EQ(CountIssues(issues, InputIssue::Severity::Warning), size_t(1));
	NOX_CHECK_EQ(CountIssues(issues, InputIssue::Severity::Error), size_t(0));
	NOX_CHECK(Contains(issues[0].message, "'accelerate'") && Contains(issues[0].message, "'boost'"));

	// The runtime parser rejects a bad name written into the file by hand.
	InputActionsModel hand;
	NOX_REQUIRE(hand.Load("{ \"format\": 1, \"actions\": { \"jump\": { \"type\": \"button\", \"keys\": [\"NoSuchKeyName\"] } } }", error));
	issues = hand.Validate();
	NOX_CHECK(CountIssues(issues, InputIssue::Severity::Error) >= 1);

	// Duplicate names in the text are errors (the later definition wins).
	InputActionsModel dup;
	NOX_REQUIRE(dup.Load("{ \"format\": 1, \"actions\": { \"a\": { \"type\": \"button\", \"keys\": [\"A\"] }, "
		"\"a\": { \"type\": \"button\", \"keys\": [\"B\"] } } }", error));
	NOX_REQUIRE(dup.DuplicateNames().size() == 1);
	NOX_CHECK_EQ(dup.DuplicateNames()[0], std::string("a"));
	NOX_CHECK(CountIssues(dup.Validate(), InputIssue::Severity::Error) >= 1);
}

NOX_TEST(InputActionsModel, SaveWritesTheSerializedText) {
	InputActionsModel model;
	std::string error;
	NOX_REQUIRE(model.Load(kArena, error));
	NOX_CHECK(model.AddBinding("accelerate", "keys", "Space", error));

	const fs::path dir = fs::temp_directory_path() / "nox_input_actions_test";
	std::error_code ec;
	fs::create_directories(dir, ec);
	const fs::path file = dir / "input.json";
	NOX_REQUIRE(model.Save(file.string(), error));

	std::ifstream in(file, std::ios::binary);
	std::stringstream buffer;
	buffer << in.rdbuf();
	NOX_CHECK_EQ(buffer.str(), model.Serialize());

	// The saved file is a valid action map for the runtime parser.
	NoxLua::InputMap map;
	std::vector<std::string> errors;
	NOX_CHECK(map.Parse(buffer.str(), errors));
	NOX_CHECK(map.Has("accelerate") && map.Has("steer"));
	fs::remove_all(dir, ec);
}

NOX_TEST(InputActionsModel, EngineEditorActionMapLoads) {
	const fs::path file = NoxTest::RepoRoot() / "config" / "editor_input.json";
	std::ifstream in(file, std::ios::binary);
	NOX_REQUIRE(in.good());
	std::stringstream buffer;
	buffer << in.rdbuf();

	InputActionsModel model;
	std::string error;
	NOX_REQUIRE(model.Load(buffer.str(), error));
	NOX_CHECK(model.Validate().empty());
	for (const char* name : { "editor_move_forward", "editor_move_strafe", "editor_move_vertical", "editor_look_x", "editor_look_y" }) {
		NOX_CHECK(model.HasAction(name));
	}
	NOX_CHECK_EQ(model.ActionType("editor_move_forward"), std::string("axis"));

	// The fly camera's actions are real axes of the runtime map.
	NoxLua::InputMap map;
	std::vector<std::string> errors;
	NOX_CHECK(map.Parse(buffer.str(), errors));
	NoxLua::InputDevices devices;
	devices.key = [](int scancode) { return scancode == SDL_SCANCODE_W; };
	devices.padAxis = [](int, int) { return 0.0f; };
	devices.padButton = [](int, int) { return false; };
	devices.mouse = [](int) { return false; };
	map.SampleWith(devices, true);
	NOX_CHECK(map.Axis("editor_move_forward") == 1.0f);
	NOX_CHECK(map.Axis("editor_move_strafe") == 0.0f);
}
