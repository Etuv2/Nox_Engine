// External script editor and Lua editor support (docs/editor.md, "External script editor"; docs/lua_api.md,
// "Editor support"): launch plans for VS Code, the OS handler and custom templates; the generated annotation stub and
// the merged .luarc.json. Nothing here starts a process: the plans are checked as text.
#include "TestFramework.h"

#include "editor/ExternalEditor.h"
#include "reflect/Reflection.h"
#include "scripting/LuaEditorSupport.h"

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using ExternalEditor::Mode;
using ExternalEditor::Settings;

namespace {
	const char* kCode = "C:/VS/Code.exe";

	Settings Custom(const std::string& command) {
		Settings s;
		s.mode = Mode::Custom;
		s.command = command;
		return s;
	}

	std::string ReadAll(const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		return text.str();
	}

	void WriteAll(const fs::path& path, const std::string& text) {
		fs::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
	}
}

NOX_TEST(ExternalEditor, DefaultTemplateOpensVSCodeAtTheLine)
{
	NOX_CHECK_EQ(ExternalEditor::DefaultCommand(), std::string("\"<exe>\" -g \"{file}:{line}\""));
	NOX_CHECK_EQ(ExternalEditor::ExpandTemplate(ExternalEditor::DefaultCommand(), kCode, "C:/p/scripts/main.lua", 12, "C:/p"),
		std::string("\"C:/VS/Code.exe\" -g \"C:/p/scripts/main.lua:12\""));
	// Line 0 (unknown) opens line 1.
	NOX_CHECK_EQ(ExternalEditor::ExpandTemplate("{line}", kCode, "f", 0, "r"), std::string("1"));
	NOX_CHECK_EQ(ExternalEditor::ExpandTemplate("{root}|{file}", kCode, "f", 1, "r"), std::string("r|f"));
}

NOX_TEST(ExternalEditor, AutoPlanUsesVSCodeWhenInstalled)
{
	const ExternalEditor::LaunchPlan plan = ExternalEditor::PlanFile(Settings{}, kCode, "C:/nope/scripts/main.lua", 7);
	NOX_CHECK(!plan.shellOpen);
	NOX_CHECK_EQ(plan.commandLine, std::string("\"C:/VS/Code.exe\" -g \"C:/nope/scripts/main.lua:7\""));
	NOX_CHECK_EQ(plan.workingDir, std::string("C:/nope/scripts"));
}

NOX_TEST(ExternalEditor, AutoPlanFallsBackToTheOsHandler)
{
	const ExternalEditor::LaunchPlan plan = ExternalEditor::PlanFile(Settings{}, "", "C:/nope/scripts/main.lua", 7);
	NOX_CHECK(plan.shellOpen);
	NOX_CHECK_EQ(plan.commandLine, std::string("C:/nope/scripts/main.lua"));
}

NOX_TEST(ExternalEditor, OsModeAlwaysUsesTheOsHandler)
{
	Settings os;
	os.mode = Mode::OsDefault;
	const ExternalEditor::LaunchPlan file = ExternalEditor::PlanFile(os, kCode, "C:/nope/a.lua", 3);
	NOX_CHECK(file.shellOpen);
	NOX_CHECK_EQ(file.commandLine, std::string("C:/nope/a.lua"));
	const ExternalEditor::LaunchPlan folder = ExternalEditor::PlanFolder(os, kCode, "C:/nope");
	NOX_CHECK(folder.shellOpen);
}

NOX_TEST(ExternalEditor, CustomTemplateIsExpanded)
{
	const ExternalEditor::LaunchPlan plan = ExternalEditor::PlanFile(Custom("\"<exe>\" --goto \"{file}:{line}\""), kCode,
		"C:/nope/scripts/main.lua", 3);
	NOX_CHECK(!plan.shellOpen);
	NOX_CHECK_EQ(plan.commandLine, std::string("\"C:/VS/Code.exe\" --goto \"C:/nope/scripts/main.lua:3\""));
	// Without VS Code the placeholder names the bare command.
	const ExternalEditor::LaunchPlan noVs = ExternalEditor::PlanFile(Custom("\"<exe>\" {file}"), "", "C:/nope/a.lua", 1);
	NOX_CHECK_EQ(noVs.commandLine, std::string("\"code\" C:/nope/a.lua"));
	// An empty custom command is the automatic choice.
	const ExternalEditor::LaunchPlan empty = ExternalEditor::PlanFile(Custom(""), "", "C:/nope/a.lua", 1);
	NOX_CHECK(empty.shellOpen);
}

NOX_TEST(ExternalEditor, FolderPlanOpensAWorkspaceInVSCode)
{
	const ExternalEditor::LaunchPlan plan = ExternalEditor::PlanFolder(Settings{}, kCode, "C:/p/projects/nox-racing");
	NOX_CHECK(!plan.shellOpen);
	NOX_CHECK_EQ(plan.commandLine, std::string("\"C:/VS/Code.exe\" \"C:/p/projects/nox-racing\""));
	NOX_CHECK_EQ(plan.workingDir, std::string("C:/p/projects/nox-racing"));
	NOX_CHECK(ExternalEditor::PlanFolder(Settings{}, "", "C:/p").shellOpen);
}

NOX_TEST(ExternalEditor, SettingsRoundTripThroughJson)
{
	const Settings custom = Custom("\"x\" {file}");
	const Settings back = ExternalEditor::FromJson(ExternalEditor::ToJson(custom));
	NOX_CHECK(back.mode == Mode::Custom);
	NOX_CHECK_EQ(back.command, std::string("\"x\" {file}"));
	NOX_CHECK(ExternalEditor::FromJson(nlohmann::json::object()).mode == Mode::Auto);
	NOX_CHECK(ExternalEditor::FromJson(nlohmann::json{ { "mode", "os" } }).mode == Mode::OsDefault);
}

NOX_TEST(LuaEditorSupport, StubHasTheApiAndTheRegistryComponents)
{
	nox::reflect::RegisterBuiltinComponents();
	const std::string stub = NoxLua::LuaApiStubText();
	NOX_CHECK(stub.rfind("---@meta", 0) == 0);
	NOX_CHECK(stub.find("---@class nox.Ctx") != std::string::npos);
	NOX_CHECK(stub.find("function Ctx:component(h, name) end") != std::string::npos);
	NOX_CHECK(stub.find("@@COMPONENT_OVERLOADS@@") == std::string::npos);

	const auto& components = nox::reflect::Registry::Get().All();
	NOX_REQUIRE(!components.empty());
	const nox::reflect::ComponentDesc& first = components.front();
	NOX_CHECK(stub.find("---@class nox.comp." + first.name + "\n") != std::string::npos);
	NOX_CHECK(stub.find("---@overload fun(self: nox.Ctx, h: nox.Entity, name: \"" + first.name + "\"): nox.comp." +
		first.name + "?\n") != std::string::npos);
	// Every property of the first component is a typed field (names here are plain identifiers).
	for (const nox::reflect::PropertyDesc& prop : first.properties) {
		if (prop.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) continue;
		NOX_CHECK(stub.find("---@field " + prop.name + " ") != std::string::npos);
	}
}

NOX_TEST(LuaEditorSupport, WritesStubAndLuarcOnceAndKeepsUserKeys)
{
	nox::reflect::RegisterBuiltinComponents();
	const fs::path dir = NoxTest::ScratchDir() / "lua_support_project";
	fs::remove_all(dir);
	fs::create_directories(dir);

	std::string report;
	NOX_CHECK(NoxLua::WriteLuaEditorSupport(dir.string(), report));
	NOX_CHECK(fs::is_regular_file(dir / ".nox" / "types" / "nox.lua"));
	NOX_CHECK(fs::is_regular_file(dir / ".luarc.json"));
	NOX_CHECK(report.find("wrote") != std::string::npos);

	const nlohmann::json luarc = nlohmann::json::parse(ReadAll(dir / ".luarc.json"));
	NOX_CHECK_EQ(luarc["runtime"]["version"].get<std::string>(), std::string("Lua 5.5"));
	NOX_CHECK_EQ(luarc["workspace"]["library"][0].get<std::string>(), std::string("./.nox/types"));
	NOX_CHECK_EQ(luarc["diagnostics"]["globals"][0].get<std::string>(), std::string("nox"));

	// A second run changes nothing: the stub keeps its text and .luarc.json is not rewritten.
	report.clear();
	NOX_CHECK(NoxLua::WriteLuaEditorSupport(dir.string(), report));
	NOX_CHECK(report.empty());

	// User keys survive and the engine entries are added to the user's arrays.
	WriteAll(dir / ".luarc.json", R"({ "runtime": { "version": "LuaJIT" }, "workspace": { "library": ["./libs"] },
		"diagnostics": { "globals": ["custom"] } })");
	report.clear();
	NOX_CHECK(NoxLua::WriteLuaEditorSupport(dir.string(), report));
	const nlohmann::json merged = nlohmann::json::parse(ReadAll(dir / ".luarc.json"));
	NOX_CHECK_EQ(merged["runtime"]["version"].get<std::string>(), std::string("LuaJIT"));
	NOX_CHECK_EQ(merged["workspace"]["library"].size(), size_t(2));
	NOX_CHECK(merged["diagnostics"]["globals"].size() == 5);
	NOX_CHECK(merged["diagnostics"]["globals"][0] == "custom");
}

NOX_TEST(LuaEditorSupport, LuarcWithCommentsAndInvalidJsonIsLeftAlone)
{
	const fs::path dir = NoxTest::ScratchDir() / "lua_support_comments";
	fs::remove_all(dir);
	fs::create_directories(dir);

	const std::string commented = "{ // the user's comment\n  \"runtime\": {}\n}\n";
	WriteAll(dir / ".luarc.json", commented);
	std::string report;
	NOX_CHECK(NoxLua::WriteLuaEditorSupport(dir.string(), report));
	NOX_CHECK_EQ(ReadAll(dir / ".luarc.json"), commented);
	NOX_CHECK(report.find("has comments") != std::string::npos);

	WriteAll(dir / ".luarc.json", "[1, 2]");
	report.clear();
	NOX_CHECK(!NoxLua::WriteLuaEditorSupport(dir.string(), report));
	NOX_CHECK_EQ(ReadAll(dir / ".luarc.json"), std::string("[1, 2]"));
}
