#include "editor/ExternalEditor.h"
#include "editor/ProcessRunner.h"
#include "assets/AssetIO.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace {
	// -g is the short form of --goto: opens the file at the line in an existing VS Code window.
	const char* kVSCodeTemplate = "\"<exe>\" -g \"{file}:{line}\"";

	std::wstring Utf8ToWideText(const std::string& text) {
		if (text.empty()) return std::wstring();
		const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		if (needed <= 0) return std::wstring();
		std::wstring out(static_cast<size_t>(needed), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
		return out;
	}

	bool FileExists(const fs::path& path) {
		std::error_code ec;
		return fs::is_regular_file(path, ec);
	}

	std::string Env(const char* name) {
		char* value = nullptr;
		size_t size = 0;
		if (_dupenv_s(&value, &size, name) != 0 || !value) return std::string();
		std::string out(value);
		std::free(value);
		return out;
	}

	// Launches a command line without blocking; logs the command so the launch is visible in the console.
	bool Launch(const std::string& commandLine, const std::string& workingDir, std::string& error) {
		if (!ProcessRunner::LaunchDetached(commandLine, workingDir, true)) {
			error = "could not start: " + commandLine;
			std::cerr << "[ExternalEditor] launch failed: " << commandLine << std::endl;
			return false;
		}
		std::cout << "[ExternalEditor] launched: " << commandLine << std::endl;
		return true;
	}

	// Hands the path to the OS handler (ShellExecuteW "open" returns at once).
	bool ShellOpen(const std::string& path, std::string& error) {
		const std::wstring wide = Utf8ToWideText(path);
		const INT_PTR result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
		if (result <= 32) {   // ShellExecute reports failures as values up to 32
			error = "the OS has no handler for " + path;
			std::cerr << "[ExternalEditor] ShellExecute failed (" << result << "): " << path << std::endl;
			return false;
		}
		std::cout << "[ExternalEditor] shell open: " << path << std::endl;
		return true;
	}

	// Nearest folder at or above `start` that holds a project.json; `start` itself when there is none.
	std::string ProjectRootFor(const fs::path& start) {
		std::error_code ec;
		for (fs::path dir = start; !dir.empty(); dir = dir.parent_path()) {
			if (FileExists(dir / "project.json")) return dir.string();
			if (dir == dir.root_path() || dir.parent_path() == dir) break;
		}
		return start.string();
	}
}

namespace ExternalEditor {

	Settings& Current() {
		static Settings settings;
		return settings;
	}

	nlohmann::json ToJson(const Settings& settings) {
		const char* mode = settings.mode == Mode::OsDefault ? "os" : settings.mode == Mode::Custom ? "custom" : "auto";
		return nlohmann::json{ { "mode", mode }, { "command", settings.command } };
	}

	Settings FromJson(const nlohmann::json& json) {
		Settings settings;
		if (!json.is_object()) return settings;
		const std::string mode = json.value("mode", std::string("auto"));
		if (mode == "os") settings.mode = Mode::OsDefault;
		else if (mode == "custom") settings.mode = Mode::Custom;
		settings.command = json.value("command", std::string());
		return settings;
	}

	std::string DefaultCommand() {
		return kVSCodeTemplate;
	}

	std::string DetectVSCode() {
		// `code` on PATH is the bin\code.cmd wrapper; the executable sits one folder up.
		wchar_t found[MAX_PATH * 4] = {};
		const DWORD length = SearchPathW(nullptr, L"code.cmd", nullptr, static_cast<DWORD>(std::size(found)), found, nullptr);
		if (length > 0 && length < std::size(found)) {
			const fs::path exe = fs::path(found).parent_path().parent_path() / "Code.exe";
			if (FileExists(exe)) return exe.string();
		}
		const std::string localAppData = Env("LOCALAPPDATA");
		const std::string programFiles = Env("ProgramFiles");
		const std::string programFilesX86 = Env("ProgramFiles(x86)");
		const std::vector<fs::path> candidates = {
			fs::path(localAppData) / "Programs" / "Microsoft VS Code" / "Code.exe",
			fs::path(programFiles) / "Microsoft VS Code" / "Code.exe",
			fs::path(programFilesX86) / "Microsoft VS Code" / "Code.exe",
		};
		for (const fs::path& candidate : candidates) {
			if (!candidate.empty() && FileExists(candidate)) return candidate.string();
		}
		return std::string();
	}

	std::string ExpandTemplate(const std::string& templ, const std::string& exe, const std::string& file, int line,
		const std::string& root) {
		const std::pair<std::string, std::string> tokens[] = {
			{ "<exe>", exe },
			{ "{file}", file },
			{ "{line}", std::to_string(line > 0 ? line : 1) },
			{ "{root}", root },
		};
		std::string out = templ;
		for (const auto& [token, value] : tokens) {
			size_t at = 0;
			while ((at = out.find(token, at)) != std::string::npos) {
				out.replace(at, token.size(), value);
				at += value.size();
			}
		}
		return out;
	}

	LaunchPlan PlanFile(const Settings& settings, const std::string& vscodeExe, const std::string& absolutePath, int line) {
		LaunchPlan plan;
		const bool custom = settings.mode == Mode::Custom && !settings.command.empty();
		if (settings.mode == Mode::OsDefault || (!custom && vscodeExe.empty())) {
			plan.shellOpen = true;
			plan.commandLine = absolutePath;
			return plan;
		}
		const std::string root = ProjectRootFor(fs::path(absolutePath).parent_path());
		plan.workingDir = root;
		if (custom) {
			plan.commandLine = ExpandTemplate(settings.command, vscodeExe.empty() ? "code" : vscodeExe, absolutePath, line, root);
			return plan;
		}
		plan.commandLine = ExpandTemplate(DefaultCommand(), vscodeExe, absolutePath, line, root);
		return plan;
	}

	LaunchPlan PlanFolder(const Settings& settings, const std::string& vscodeExe, const std::string& absolutePath) {
		LaunchPlan plan;
		if (settings.mode != Mode::OsDefault && !vscodeExe.empty()) {
			plan.commandLine = "\"" + vscodeExe + "\" \"" + absolutePath + "\"";
			plan.workingDir = absolutePath;
			return plan;
		}
		plan.shellOpen = true;
		plan.commandLine = absolutePath;
		return plan;
	}

	bool Run(const LaunchPlan& plan, std::string& error) {
		if (plan.shellOpen) return ShellOpen(plan.commandLine, error);
		return Launch(plan.commandLine, plan.workingDir, error);
	}

	bool OpenFile(const std::string& absolutePath, int line, std::string& error) {
		return Run(PlanFile(Current(), DetectVSCode(), absolutePath, line), error);
	}

	bool OpenFolder(const std::string& absolutePath, std::string& error) {
		return Run(PlanFolder(Current(), DetectVSCode(), absolutePath), error);
	}

	bool OpenLogicalFile(const std::string& logicalPath, int line, std::string& error) {
		const auto physical = AssetIO::SourcePath(logicalPath);
		if (!physical) {
			error = "not in the loose source tree: " + logicalPath;
			return false;
		}
		return OpenFile(physical->string(), line, error);
	}

	bool OpenLogicalFolder(const std::string& logicalPath, std::string& error) {
		const auto physical = AssetIO::SourcePath(logicalPath);
		if (!physical) {
			error = "not in the loose source tree: " + logicalPath;
			return false;
		}
		return OpenFolder(physical->string(), error);
	}
}
