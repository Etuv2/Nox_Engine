#pragma once
// Startup selection of the asset mounts (docs/asset_io.md, "Bootstrap").
//
// A shipped distribution has a physical nox_bootstrap.json beside the executable that names its
// project, read-only packs and writable user directory. Without one the executable is a development
// build: it finds the source checkout above itself and mounts it loose. Bootstrap runs before any
// configuration, launch preset or scene is read, so every later read goes through AssetIO.
#include <filesystem>
#include <string>

struct BootstrapInfo {
	bool packed = false;
	std::string project;            // distribution/project name ("" = engine only)
	std::string descriptor;         // logical project descriptor path, may be empty
	std::string contentVersion;
	bool reloadEnabled = true;      // script reload/watchers; off by default in shipping
	std::filesystem::path exeDir;
	std::filesystem::path bootstrapFile;   // empty in development
	std::filesystem::path sourceRoot;      // loose root; empty when packed
};

namespace ProjectBootstrap {
	// Reads the bootstrap (explicitFile overrides the exe-relative lookup), initializes AssetIO and
	// creates the writable directories. False on a fatal configuration error (message in error).
	bool Initialize(const std::filesystem::path& explicitFile, std::string& error);
	const BootstrapInfo& Info();
	std::filesystem::path ExecutablePath();
}
