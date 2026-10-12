#pragma once
// Cooked procedural results (contract C10): complete, versioned outputs of procedural generators,
// stored under generated/cooked/<generator>-<key>/ and loaded by SceneLoader when the generator's
// code is not compiled into the running executable (or when a valid cooked result exists).
//
// Directory layout (logical paths, readable through AssetIO):
//   generated/cooked/<generator>-<key>/result.json   format, identity, children, probes, render extras
//   generated/cooked/<generator>-<key>/model.gltf    generated model (+ .bin and generated textures)
//   generated/cooked/<generator>-<key>/model<suffix> side files (e.g. model_layout.json)
//
// Key = SHA-256 over canonical JSON of { format, generator, generator_version, params (without
// "output", a location), scene_game (the scene "game" keys the generator declares), inputs (SHA-256 of
// every file under the generator's declared input directories, by logical path) }.
#include "SceneExtensions.h"
#include "SceneRenderExtras.h"
#include "json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CookedResults {

	constexpr int kFormatVersion = 1;
	constexpr const char* kRootDir = "generated/cooked";
	constexpr const char* kResultFile = "result.json";
	constexpr const char* kModelStem = "model";

	struct InputHash {
		std::string path;     // logical path
		std::string sha256;   // lower-case hex
	};

	struct SideFile {
		std::string suffix;   // appended to the live output stem (e.g. "_layout.json")
		std::string file;     // file name inside the result directory (e.g. "model_layout.json")
	};

	// Everything a cooked result stores. Round-trips exactly through ToJson/FromJson.
	struct Result {
		int format = kFormatVersion;
		std::string generator;
		int generatorVersion = 0;
		std::string key;                          // SHA-256 hex
		std::string defaultOutput;                // live output stem used when params omit "output"
		nlohmann::json params = nlohmann::json::object();   // canonical params without "output"
		std::vector<std::string> sceneGameKeys;   // declared scene "game" keys
		nlohmann::json sceneGame = nlohmann::json::object(); // subset of the scene "game" object
		std::vector<InputHash> inputs;
		std::string model;                        // file name inside the result directory
		std::vector<SideFile> sideFiles;
		nlohmann::json childNodes = nlohmann::json::array();
		nlohmann::json reflectionProbes = nlohmann::json::array();
		bool hasRenderExtras = false;
		SceneRenderExtras renderExtras;
		std::string dir;                          // logical directory; set by LoadDir/Identify, not stored
	};

	// Identity of a procedural node as far as its key is concerned.
	struct NodeIdentity {
		std::string generator;
		int generatorVersion = 0;
		std::string defaultOutput;
		std::vector<std::string> sceneGameKeys;
		nlohmann::json params = nlohmann::json::object();
		nlohmann::json sceneGame = nlohmann::json::object();
		std::vector<InputHash> inputs;
		std::string key;
		std::string dir;                          // "generated/cooked/<generator>-<key>"
	};

	// Canonical JSON text (object keys sorted, compact). Equal values give equal text.
	std::string CanonicalJson(const nlohmann::json& value);
	// Params without the "output" location key.
	nlohmann::json WithoutOutput(const nlohmann::json& params);
	std::string ParamsHash(const nlohmann::json& params);
	// The declared keys of a scene "game" object that are present (absent keys are omitted).
	nlohmann::json SceneGameSubset(const nlohmann::json& sceneGame, const std::vector<std::string>& keys);
	// Hashes every file under the generator's declared input directories (sorted by path). Returns false
	// with an error when a declared directory cannot be listed or a file cannot be read.
	bool HashInputs(const ProceduralGeneratorInfo& info, const nlohmann::json& params,
		std::vector<InputHash>& out, std::string* error);
	std::string ComputeKey(const std::string& generator, int generatorVersion, const nlohmann::json& params,
		const nlohmann::json& sceneGameSubset, const std::vector<InputHash>& inputs);
	// Computes the identity (key and directory) of a procedural node with the given raw params.
	std::optional<NodeIdentity> Identify(const std::string& generator, const ProceduralGeneratorInfo& info,
		const nlohmann::json& rawParams, const nlohmann::json& sceneGame, std::string* error);

	// Render extras (decal sets, lens flares, interior atlas). Textures are stored as logical paths.
	nlohmann::json RenderExtrasToJson(const SceneRenderExtras& extras);
	bool RenderExtrasFromJson(const nlohmann::json& json, SceneRenderExtras& out, std::string* error);

	nlohmann::json ToJson(const Result& result);
	// Structural validation plus key recomputation from the stored identity. Does not re-hash inputs.
	bool FromJson(const nlohmann::json& json, Result& out, std::string* error);

	// Builds a result from a generator run whose output was written under identity.dir + "/model".
	bool FromGenerated(const NodeIdentity& identity, const ProceduralGeneratorResult& generated, Result& out,
		std::string* error);
	// The generator result a loaded cooked result stands for (logical paths).
	ProceduralGeneratorResult ToGenerated(const Result& result);

	enum class Lookup { Missing, Invalid, Found };
	// Reads <logicalDir>/result.json and checks that its files exist. Invalid means present but unusable.
	Lookup LoadDir(const std::string& logicalDir, Result& out, std::string* error);
	// Writes result.json into result.dir (atomic replace through a temporary file).
	bool Write(const Result& result, std::string* error);
	// Copies the side files to their live locations (authoredOutput + suffix) inside the cache, when they
	// differ. authoredOutput must be a logical path under generated/.
	bool MaterializeSideFiles(const Result& result, const std::string& authoredOutput, std::string* error);
	// For generators whose code is not registered: finds the cooked result of this generator, params and
	// scene game subset. The key cannot be recomputed without the generator, so the match uses the stored
	// params hash and scene subset hash (input files are not re-hashed).
	bool FindUnregistered(const std::string& generator, const nlohmann::json& rawParams,
		const nlohmann::json& sceneGame, Result& out, std::string* error);
}
