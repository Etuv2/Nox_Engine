#pragma once
// Project descriptor (contract C4): projects/<name>/project.json, read through AssetIO.
#include "LuaRuntime.h"
#include "LuaWorld.h"

#include "json.hpp"

#include <string>
#include <vector>

namespace NoxLua {

	struct ProjectDescriptor {
		std::string descriptorPath;          // logical path, e.g. "projects/sample-arena/project.json"
		ProjectContent content;              // name, entry and script roots resolved to logical paths
		std::vector<std::string> dataRoots;  // resolved; reserved for data-driven content
		std::vector<UiFontDecl> fonts;       // "fonts": [{"id", "path", "size"}], paths resolved
		std::string iconManifest;            // "icons": {"manifest", "size"}; logical path, "" = none
		int iconSize = 128;
		std::string inputPath;               // "input" (default "input.json" beside the descriptor), logical
		std::vector<std::string> audioBuses; // "audio": { "buses": [...] }: extra mixer buses (AudioMixer)
	};

	// Validates format 1, runtime "lua" and api_version 1; resolves entry/script_roots/data_roots relative to
	// the descriptor's directory with AssetIO::ResolveRelative.
	bool LoadProjectDescriptor(const std::string& descriptorPath, const nlohmann::json& descriptor,
		ProjectDescriptor& out, std::string& error);

	// Reads a descriptor from AssetIO and parses it.
	bool ReadProjectDescriptor(const std::string& descriptorPath, ProjectDescriptor& out, std::string& error);

	// ScriptSource over AssetIO (the only file reader the VM uses).
	class AssetScriptSource : public ScriptSource {
	public:
		bool ReadText(const std::string& logicalPath, std::string& text, std::string& error) override;
	};
}
