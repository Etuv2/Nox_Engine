#pragma once

// External script editor (docs/editor.md, "External script editor"): opens project files (Lua scripts) and project
// folders in an IDE outside the engine. The choice is a user setting ("external_editor" in the editor settings file):
//   auto    VS Code when it is installed (code on PATH, %LOCALAPPDATA%, Program Files), else the OS default handler.
//   os      the OS default handler for the file (ShellExecuteW "open").
//   custom  a command template, for example  "<exe>" -g "{file}:{line}"
//           {file} is the absolute file path, {line} the 1-based line (1 when unknown), {root} the project folder,
//           <exe> the detected VS Code executable (or "code" when none was detected).
// Folders always open in VS Code when it is detected (as a workspace), else through the OS handler.
//
// Launches never block the editor: templates run through ProcessRunner::LaunchDetached (CreateProcessW, no console
// window), the OS handler through ShellExecuteW. Every launch writes one "[ExternalEditor]" line to the log.
// PlanFile and PlanFolder decide what a launch does without starting anything; the tests check those plans.

#include <string>
#include "json.hpp"

namespace ExternalEditor {

	enum class Mode { Auto, OsDefault, Custom };

	struct Settings {
		Mode mode = Mode::Auto;
		std::string command;          // template for Mode::Custom
	};

	// What a launch will do: a command line started in workingDir, or (shellOpen) commandLine is the path for the OS handler.
	struct LaunchPlan {
		bool shellOpen = false;
		std::string commandLine;
		std::string workingDir;
	};

	// The active settings (loaded by the editor from its settings file, saved with it).
	Settings& Current();
	nlohmann::json ToJson(const Settings& settings);
	Settings FromJson(const nlohmann::json& json);

	// The template used when the command is empty (VS Code syntax).
	std::string DefaultCommand();

	// Path of Code.exe, "" when VS Code is not installed in a known place.
	std::string DetectVSCode();

	// The launch for an absolute file path at a 1-based line (0 = no line). vscodeExe is DetectVSCode()'s result.
	LaunchPlan PlanFile(const Settings& settings, const std::string& vscodeExe, const std::string& absolutePath, int line);
	// The launch for an absolute folder (opened as a VS Code workspace when VS Code is known).
	LaunchPlan PlanFolder(const Settings& settings, const std::string& vscodeExe, const std::string& absolutePath);

	// Runs a plan. False with a reason when nothing could be started.
	bool Run(const LaunchPlan& plan, std::string& error);

	// Opens an absolute file path at a 1-based line (0 = no line). False with a reason when nothing could be started.
	bool OpenFile(const std::string& absolutePath, int line, std::string& error);
	// Opens an absolute folder as a workspace (VS Code) or in the OS handler.
	bool OpenFolder(const std::string& absolutePath, std::string& error);

	// Same for logical asset paths ("projects/x/scripts/main.lua"), resolved through AssetIO (loose source tree only).
	bool OpenLogicalFile(const std::string& logicalPath, int line, std::string& error);
	bool OpenLogicalFolder(const std::string& logicalPath, std::string& error);

	// Expands the template placeholders (exposed for the settings preview and tests).
	std::string ExpandTemplate(const std::string& templ, const std::string& exe, const std::string& file, int line,
		const std::string& root);
}
