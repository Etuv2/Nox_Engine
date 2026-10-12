#include "app/ProjectBootstrap.h"
#include "assets/AssetIO.h"
#include "json.hpp"

#include <fstream>
#include <iostream>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

namespace fs = std::filesystem;

namespace {
	BootstrapInfo g_info;

	fs::path LocalAppData()
	{
		PWSTR raw = nullptr;
		fs::path result;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) result = raw;
		CoTaskMemFree(raw);
		return result;
	}

	// Pack names and user_dir are relative, non-escaping paths ("game.pak", "NOX/NOX_Racing").
	bool SafeRelative(const std::string& value)
	{
		if (value.empty()) return false;
		const fs::path p(value);
		if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
		for (const fs::path& part : p) {
			if (part == ".." || part == ".") return false;
		}
		return true;
	}

	// Development: the checkout holding CMakeLists.txt, shaders/ and config/ above the executable.
	fs::path FindSourceRoot(const fs::path& exeDir)
	{
		std::error_code ec;
		fs::path dir = exeDir;
		for (int i = 0; i < 6 && !dir.empty(); ++i) {
			if (fs::exists(dir / "shaders", ec) && fs::exists(dir / "config", ec) && fs::exists(dir / "CMakeLists.txt", ec)) return dir;
			if (dir == dir.parent_path()) break;
			dir = dir.parent_path();
		}
		// A loose tree beside the executable (packages made by the older loose packager).
		if (fs::exists(exeDir / "shaders", ec) && fs::exists(exeDir / "config", ec)) return exeDir;
		return {};
	}
}

namespace ProjectBootstrap {
	fs::path ExecutablePath()
	{
		std::wstring buffer(MAX_PATH, L'\0');
		for (;;) {
			const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (n == 0) return {};
			if (n < buffer.size()) { buffer.resize(n); break; }
			buffer.resize(buffer.size() * 2);
		}
		return fs::path(buffer);
	}

	const BootstrapInfo& Info() { return g_info; }

	bool Initialize(const fs::path& explicitFile, std::string& error)
	{
		std::error_code ec;
		g_info = BootstrapInfo{};
		g_info.exeDir = ExecutablePath().parent_path();
		const fs::path candidate = explicitFile.empty() ? g_info.exeDir / "nox_bootstrap.json" : explicitFile;

		AssetIO::Config config;
		if (fs::is_regular_file(candidate, ec)) {
			std::ifstream in(candidate, std::ios::binary);
			const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
			if (!doc.is_object() || doc.value("format", 0) != 1) {
				error = "Invalid bootstrap file: " + candidate.string();
				return false;
			}
			g_info.bootstrapFile = candidate;
			g_info.project = doc.value("project", std::string());
			g_info.descriptor = doc.value("descriptor", std::string());
			g_info.contentVersion = doc.value("content_version", std::string());
			g_info.reloadEnabled = doc.value("reload", false);
			const std::string mode = doc.value("mode", std::string("packed"));
			const fs::path baseDir = candidate.parent_path();
			if (mode == "packed") {
				g_info.packed = true;
				config.mode = AssetIO::Mode::Packed;
				for (const auto& pack : doc.value("packs", nlohmann::json::array())) {
					if (!pack.is_string() || !SafeRelative(pack.get<std::string>())) {
						error = "Bootstrap pack entries must be relative file names: " + candidate.string();
						return false;
					}
					config.packs.push_back(baseDir / fs::path(pack.get<std::string>()));
				}
				if (config.packs.empty()) {
					error = "Bootstrap names no packs: " + candidate.string();
					return false;
				}
			}
			else if (mode == "loose") {
				config.mode = AssetIO::Mode::Loose;
				config.looseRoot = fs::weakly_canonical(baseDir / fs::path(doc.value("root", std::string("."))), ec);
				g_info.sourceRoot = config.looseRoot;
			}
			else {
				error = "Unknown bootstrap mode '" + mode + "'";
				return false;
			}
			const std::string userDir = doc.value("user_dir", "NOX/" + (g_info.project.empty() ? std::string("Engine") : g_info.project));
			if (!SafeRelative(userDir)) {
				error = "Bootstrap user_dir must be a relative folder: " + userDir;
				return false;
			}
			const fs::path appData = LocalAppData();
			if (appData.empty()) {
				error = "Could not resolve %LOCALAPPDATA% for the user directory";
				return false;
			}
			config.userDir = appData / fs::path(userDir);
			config.cacheDir = config.userDir / "cache" / "generated";
		}
		else if (!explicitFile.empty()) {
			error = "Bootstrap file not found: " + explicitFile.string();
			return false;
		}
		else {
			g_info.sourceRoot = FindSourceRoot(g_info.exeDir);
			if (g_info.sourceRoot.empty()) {
				error = "No nox_bootstrap.json beside the executable and no source checkout above it (" + g_info.exeDir.string() + ")";
				return false;
			}
			config.mode = AssetIO::Mode::Loose;
			config.looseRoot = g_info.sourceRoot;
			config.cacheDir = g_info.sourceRoot / "generated";
			// Development state stays in the ignored build tree, never in authored content.
			config.userDir = g_info.sourceRoot / "build" / "user";
		}
		config.projectName = g_info.project;
		fs::create_directories(config.userDir, ec);
		// Shipped content changed (new packs): generator output cached for the previous content is
		// discarded so it can never override the new packs' generated/ results.
		if (g_info.packed && !g_info.contentVersion.empty()) {
			const fs::path stamp = config.userDir / "cache" / "content_version";
			std::string previous;
			{
				std::ifstream in(stamp, std::ios::binary);
				std::getline(in, previous);
			}
			if (previous != g_info.contentVersion) {
				fs::remove_all(config.cacheDir, ec);
				fs::create_directories(stamp.parent_path(), ec);
				std::ofstream(stamp, std::ios::binary | std::ios::trunc) << g_info.contentVersion << std::endl;
				if (!previous.empty()) std::cout << "[Bootstrap] content changed; cleared generated cache" << std::endl;
			}
		}
		fs::create_directories(config.cacheDir, ec);

		std::string ioError;
		if (!AssetIO::Initialize(config, &ioError)) {
			error = "Asset I/O initialization failed: " + ioError;
			return false;
		}
		// Legacy relative paths (tool launches, diagnostics) keep resolving against the checkout in
		// development. Shipping never depends on the working directory.
		if (!g_info.sourceRoot.empty()) fs::current_path(g_info.sourceRoot, ec);
		std::cout << "[Bootstrap] mode=" << (g_info.packed ? "packed" : "loose")
			<< " project=" << (g_info.project.empty() ? "(none)" : g_info.project)
			<< " root=" << (g_info.packed ? std::string("(packs)") : g_info.sourceRoot.string())
			<< " user=" << config.userDir.string() << std::endl;
		for (const fs::path& pack : config.packs) std::cout << "[Bootstrap] mounted " << pack.string() << std::endl;
		return true;
	}
}
