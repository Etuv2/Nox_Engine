/*
* Main entry point for the Nox Engine application.
*
* Command line (docs/nfs2015/game.md, "Launching"):
*   Nox_Engine.exe [--preset <name>] [--game | --editor] [--scene <path>] [--quality high|ultra]
*                  [--window windowed|borderless|fullscreen] [--width N --height N]
* Presets come from config/launch_presets.json ("default" is used without arguments). Assets are
* resolved from the executable's location, not the working directory.
*/
#include "MainWindow.h"
#include "CrashHandler.h"
#include "ImGui/ConsoleLog.h"
#include "json.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Trying to force PC to use dedicated GPU instead of integrated graphics(weird bug)
extern "C" {
	__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace fs = std::filesystem;

std::string NoxExecutablePath()
{
	wchar_t buffer[MAX_PATH] = {};
	const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
	return n ? fs::path(buffer).string() : std::string();
}

namespace {
	// Packaged builds keep their assets next to the exe; build-tree binaries (a CMakeCache.txt one
	// level up) use the source tree so editor saves land in the repository.
	void SetAssetRoot()
	{
		std::error_code ec;
		const fs::path exeDir = fs::path(NoxExecutablePath()).parent_path();
		if (exeDir.empty()) return;
		const bool inBuildTree = fs::exists(exeDir.parent_path() / "CMakeCache.txt", ec);
		if (!inBuildTree && fs::exists(exeDir / "shaders", ec) && fs::exists(exeDir / "config", ec)) {
			fs::current_path(exeDir, ec);
			return;
		}
		fs::path dir = exeDir;
		for (int i = 0; i < 6 && !dir.empty(); ++i) {
			if (fs::exists(dir / "shaders", ec) && fs::exists(dir / "config", ec) && fs::exists(dir / "CMakeLists.txt", ec)) {
				fs::current_path(dir, ec);
				return;
			}
			if (dir == dir.parent_path()) break;
			dir = dir.parent_path();
		}
		// Otherwise keep the working directory (legacy behaviour).
	}

	LaunchConfig ParseLaunch(int argc, char* argv[])
	{
		LaunchConfig cfg;
		LaunchConfig cli;
		std::string preset;
		bool cliMode = false;
		for (int i = 1; i < argc; ++i) {
			const std::string a = argv[i];
			auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
			if (a == "--preset") preset = next();
			else if (a == "--game") { cli.mode = "game"; cliMode = true; }
			else if (a == "--editor") { cli.mode = "editor"; cliMode = true; }
			else if (a == "--scene") cli.scene = next();
			else if (a == "--quality") cli.quality = next();
			else if (a == "--window") cli.window = next();
			else if (a == "--width") cli.width = std::atoi(next().c_str());
			else if (a == "--height") cli.height = std::atoi(next().c_str());
		}
		std::ifstream in("config/launch_presets.json");
		const nlohmann::json presets = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
		if (presets.is_object()) {
			if (preset.empty() && !cliMode) preset = presets.value("default", std::string());
			const nlohmann::json all = presets.value("presets", nlohmann::json::object());
			if (!preset.empty() && all.contains(preset) && all[preset].is_object()) {
				const nlohmann::json& p = all[preset];
				cfg.mode = p.value("mode", cfg.mode);
				cfg.scene = p.value("scene", cfg.scene);
				cfg.quality = p.value("quality", cfg.quality);
				cfg.window = p.value("window", cfg.window);
				cfg.width = p.value("width", cfg.width);
				cfg.height = p.value("height", cfg.height);
			}
			else if (!preset.empty()) {
				std::cerr << "[Launch] Unknown preset '" << preset << "'" << std::endl;
			}
		}
		if (cliMode) cfg.mode = cli.mode;
		if (!cli.scene.empty()) cfg.scene = cli.scene;
		if (!cli.quality.empty()) cfg.quality = cli.quality;
		if (!cli.window.empty()) cfg.window = cli.window;
		if (cli.width > 0) cfg.width = cli.width;
		if (cli.height > 0) cfg.height = cli.height;
		std::cout << "[Launch] preset=" << (preset.empty() ? "(none)" : preset) << " mode=" << cfg.mode
			<< " scene=" << (cfg.scene.empty() ? "(config)" : cfg.scene) << " quality=" << cfg.quality
			<< " window=" << cfg.window << " cwd=" << fs::current_path().string() << std::endl;
		return cfg;
	}
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	SetAssetRoot();
	ConsoleLog::Get().Install();
	const LaunchConfig launch = ParseLaunch(argc, argv);

	// Create and initialize the main application window
	MainWindow mainWindow;
	mainWindow.SetLaunchConfig(launch);
	if (!mainWindow.Initialize())
		return EXIT_FAILURE;
	mainWindow.Run();
	mainWindow.Cleanup();
	ConsoleLog::Get().Uninstall();
	return EXIT_SUCCESS;
}
