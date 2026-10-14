// nox_cook: headless procedural cook (contract C10). No window, no GL context.
//
// For every procedural node of the given scenes it computes the node's cooked key (CookedResults::Identify),
// keeps an existing valid result (generated/cooked/<generator>-<key>/result.json) and otherwise runs the
// registered generator with its output redirected into that directory, then writes result.json.
//
//   nox_cook [--repo <dir>] [--scene scenes/x.json]... [--manifest <file.json>] [--force]
//
// --manifest lists the result directories of the selected scenes (tools/AssetPackaging.ps1 packs them).
// Output: <repo>/generated/cooked/... (the AssetIO cache mounted at "generated/"). Exit 0 on success.
// Linked with NoxProcedural (the generator registry is compiled there); it never links the legacy racing game.
#include "procedural/ProceduralStreetGenerators.h"
#include "SceneExtensions.h"
#include "assets/AssetIO.h"
#include "cook/CookedResults.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// Required by the engine's editor code (EditorWorkspace); nox_cook never starts the editor.
std::string NoxExecutablePath()
{
	return {};
}

namespace {

	struct Options {
		std::string repo;
		std::vector<std::string> scenes;
		std::string manifest;
		bool force = false;
	};

	struct Entry {
		std::string scene;
		std::string generator;
		std::string key;
		std::string dir;
		bool cached = false;
	};

	void PrintUsage()
	{
		std::cerr << "usage: nox_cook [--repo <dir>] [--scene scenes/x.json]... [--manifest <file.json>] [--force]" << std::endl;
	}

	bool ParseArgs(int argc, char** argv, Options& out)
	{
		for (int i = 1; i < argc; ++i) {
			const std::string arg = argv[i];
			const bool hasValue = i + 1 < argc;
			if (arg == "--repo" && hasValue) out.repo = argv[++i];
			else if (arg == "--scene" && hasValue) out.scenes.push_back(argv[++i]);
			else if (arg == "--manifest" && hasValue) out.manifest = argv[++i];
			else if (arg == "--force") out.force = true;
			else { std::cerr << "nox_cook: unknown or incomplete argument: " << arg << std::endl; return false; }
		}
		return !out.scenes.empty();
	}

	// The repository is the nearest ancestor of the executable (or working directory) holding the engine sources.
	fs::path FindRepo(const fs::path& start)
	{
		std::error_code ec;
		fs::path dir = fs::weakly_canonical(start, ec);
		while (!dir.empty()) {
			if (fs::exists(dir / "CMakeLists.txt", ec) && fs::exists(dir / "src" / "cook" / "CookedResults.h", ec)) return dir;
			if (!dir.has_parent_path() || dir.parent_path() == dir) break;
			dir = dir.parent_path();
		}
		return {};
	}

	void CollectProcedural(const json& nodes, std::vector<json>& out)
	{
		if (!nodes.is_array()) return;
		for (const json& node : nodes) {
			if (!node.is_object()) continue;
			if (node.contains("procedural") && node["procedural"].is_object()) out.push_back(node["procedural"]);
			if (node.contains("children")) CollectProcedural(node["children"], out);
		}
	}

	bool CookGenerator(const std::string& generator, const json& params, const json& sceneGame, bool force,
		Entry& entry, std::string& error)
	{
		const ProceduralGenerator* generate = SceneExtensions::FindGenerator(generator);
		const ProceduralGeneratorInfo* info = SceneExtensions::FindGeneratorInfo(generator);
		if (!generate || !info) {
			error = "generator '" + generator + "' is not registered with nox_cook";
			return false;
		}
		const std::optional<CookedResults::NodeIdentity> identity = CookedResults::Identify(generator, *info, params, sceneGame, &error);
		if (!identity) return false;
		entry.generator = generator;
		entry.key = identity->key;
		entry.dir = identity->dir;

		CookedResults::Result existing;
		std::string loadError;
		if (!force && CookedResults::LoadDir(identity->dir, existing, &loadError) == CookedResults::Lookup::Found &&
			existing.key == identity->key) {
			entry.cached = true;
			return true;
		}

		// The generator writes its files into the cooked directory: the output location is not part of the key.
		json cookParams = params.is_object() ? params : json::object();
		cookParams["output"] = identity->dir + "/" + CookedResults::kModelStem;
		const ProceduralGeneratorResult generated = (*generate)(ProceduralGeneratorInput{ cookParams, sceneGame });

		CookedResults::Result result;
		if (!CookedResults::FromGenerated(*identity, generated, result, &error)) {
			error = "generator '" + generator + "': " + error;
			return false;
		}
		result.defaultOutput = info->defaultOutput;
		if (!CookedResults::Write(result, &error)) return false;
		return true;
	}

	bool CookScene(const std::string& scene, bool force, std::vector<Entry>& entries, std::string& error)
	{
		const std::optional<std::string> text = AssetIO::ReadText(scene, &error);
		if (!text) return false;
		const json document = json::parse(*text, nullptr, false);
		if (!document.is_object()) {
			error = "scene is not a JSON object: " + scene;
			return false;
		}
		const json sceneGame = document.value("game", json::object());
		std::vector<json> procedurals;
		if (document.contains("nodes")) CollectProcedural(document["nodes"], procedurals);
		for (const json& procedural : procedurals) {
			const std::string generator = procedural.value("generator", SceneExtensions::DefaultGenerator());
			const json params = procedural.value("params", json::object());
			Entry entry;
			entry.scene = scene;
			std::string nodeError;
			if (!CookGenerator(generator, params, sceneGame, force, entry, nodeError)) {
				error = scene + ": " + nodeError;
				return false;
			}
			std::cout << "nox_cook: " << scene << " " << entry.generator << "-" << entry.key << " "
				<< (entry.cached ? "cached" : "cooked") << " " << entry.dir << std::endl;
			entries.push_back(entry);
		}
		return true;
	}

	bool WriteManifest(const std::string& path, const std::vector<Entry>& entries, std::string& error)
	{
		json results = json::array();
		std::set<std::string> seen;
		for (const Entry& entry : entries) {
			if (!seen.insert(entry.dir).second) continue;
			results.push_back({ { "scene", entry.scene }, { "generator", entry.generator }, { "key", entry.key }, { "dir", entry.dir } });
		}
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) {
			error = "cannot write manifest " + path;
			return false;
		}
		out << json{ { "format", 1 }, { "results", results } }.dump(1) << "\n";
		return static_cast<bool>(out);
	}

} // namespace

int main(int argc, char** argv)
{
	Options options;
	if (!ParseArgs(argc, argv, options)) {
		PrintUsage();
		return 2;
	}
	fs::path repo = options.repo.empty() ? FindRepo(fs::path(argv[0]).parent_path()) : fs::path(options.repo);
	if (repo.empty()) repo = FindRepo(fs::current_path());
	if (repo.empty()) {
		std::cerr << "nox_cook: cannot find the repository root (use --repo)" << std::endl;
		return 2;
	}
	repo = fs::weakly_canonical(repo);

	AssetIO::Config config;
	config.mode = AssetIO::Mode::Loose;
	config.looseRoot = repo;
	config.cacheDir = repo / "generated";
	config.userDir = repo / "build" / "user" / "cook";
	config.projectName = "cook";
	std::string error;
	if (!AssetIO::Initialize(config, &error)) {
		std::cerr << "nox_cook: cannot initialize asset I/O: " << error << std::endl;
		return 2;
	}

	RegisterProceduralStreetGenerators();

	std::vector<Entry> entries;
	bool ok = true;
	for (const std::string& scene : options.scenes) {
		std::string sceneError;
		if (!CookScene(scene, options.force, entries, sceneError)) {
			std::cerr << "nox_cook: " << sceneError << std::endl;
			ok = false;
		}
	}
	if (ok && !options.manifest.empty()) {
		std::string manifestError;
		if (!WriteManifest(options.manifest, entries, manifestError)) {
			std::cerr << "nox_cook: " << manifestError << std::endl;
			ok = false;
		}
	}
	AssetIO::Shutdown();
	return ok ? 0 : 1;
}
