/*
* Engine application entry shared by the generic executable (src/main.cpp) and the legacy racing
* executable (src/legacy/main_racing_legacy.cpp). Both read the same command line (docs/building.md).
*/
#include "app/EngineMain.h"
#include "MainWindow.h"
#include "app/ProjectBootstrap.h"
#include "assets/AssetIO.h"
#include "GameModule.h"
#include "scripting/LuaGameModule.h"
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


namespace fs = std::filesystem;

namespace {
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
			else if (a == "--project") cli.project = next();
			else if (a == "--bootstrap") ++i;   // consumed by main() before presets are read
		}
		const auto presetText = AssetIO::ReadText("config/launch_presets.json");
		const nlohmann::json presets = presetText ? nlohmann::json::parse(*presetText, nullptr, false) : nlohmann::json();
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
				cfg.project = p.value("project", cfg.project);
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
		if (!cli.project.empty()) cfg.project = cli.project;
		std::cout << "[Launch] preset=" << (preset.empty() ? "(none)" : preset) << " mode=" << cfg.mode
			<< " scene=" << (cfg.scene.empty() ? "(config)" : cfg.scene) << " quality=" << cfg.quality
			<< " window=" << cfg.window << " project=" << (cfg.project.empty() ? "(scene)" : cfg.project) << std::endl;
		return cfg;
	}
}

int RunNoxEngine(int argc, char* argv[])
{
	CrashHandler::Install();
	// Asset mounts come first: launch presets, configuration and scenes are all read through AssetIO.
	fs::path bootstrapFile;
	for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == "--bootstrap") bootstrapFile = fs::u8path(argv[i + 1]);
	std::string bootstrapError;
	if (!ProjectBootstrap::Initialize(bootstrapFile, bootstrapError)) {
		std::cerr << "[Bootstrap] " << bootstrapError << std::endl;
		MessageBoxA(nullptr, bootstrapError.c_str(), "NOX", MB_OK | MB_ICONERROR);
		return EXIT_FAILURE;
	}
	ConsoleLog::Get().Install();
	const LaunchConfig launch = ParseLaunch(argc, argv);
	// Project runtimes: scenes naming a Lua project descriptor run it through the generic adapter.
	GameModules::RegisterProjectRuntime("lua", [](const std::string& path, const nlohmann::json& descriptor, std::string& error) {
		return NoxLua::CreateLuaProjectModule(path, descriptor, error);
	});

	// Create and initialize the main application window
	MainWindow mainWindow;
	mainWindow.SetLaunchConfig(launch);
	if (!mainWindow.Initialize())
		return EXIT_FAILURE;
	mainWindow.Run();
	mainWindow.Cleanup();
	ConsoleLog::Get().Uninstall();
	AssetIO::Shutdown();
	return EXIT_SUCCESS;
}
