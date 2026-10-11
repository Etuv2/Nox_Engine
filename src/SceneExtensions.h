#pragma once
// Extension points through which an application plugs content producers into the generic scene
// loader, without the loader knowing their names, file layouts or pack formats.
//
//   * Procedural generators: a model node with "procedural": { "generator": "<name>", "params": {} }
//     is produced by the generator registered under <name>.
//   * Scene keys: a handler registered for a top-level scene key runs after the scene's nodes are
//     loaded and may fill the scene's render extras (decals, flares, interior atlas, ...).
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "SceneRenderExtras.h"
#include "json.hpp"

struct ProceduralGeneratorResult {
	bool ok = false;
	std::string gltfPath;                                         // generated model, loaded as the node's model
	nlohmann::json childNodes = nlohmann::json::array();         // scene-schema nodes added under the node
	nlohmann::json reflectionProbes = nlohmann::json::array();   // scene "reflection_probes" entries
	bool hasRenderExtras = false;
	SceneRenderExtras renderExtras;                              // replaces the scene's extras when set
};

struct ProceduralGeneratorInput {
	const nlohmann::json& params;      // node "procedural.params"
	const nlohmann::json& sceneGame;   // the scene's "game" object (opaque to the engine)
};
using ProceduralGenerator = std::function<ProceduralGeneratorResult(const ProceduralGeneratorInput&)>;

struct SceneKeyInput {
	const nlohmann::json& value;
	// Resolves an asset path the way the loader does (scene directory, asset root); empty if missing.
	std::function<std::string(const std::string&)> resolvePath;
	SceneRenderExtras& renderExtras;
};
// Returns false when the key's content is required but failed to load (the scene load is aborted).
using SceneKeyHandler = std::function<bool(SceneKeyInput&)>;

namespace SceneExtensions {
	// The default generator is used when a node's "procedural" block omits "generator".
	void RegisterGenerator(const std::string& name, ProceduralGenerator generator, bool isDefault = false);
	const ProceduralGenerator* FindGenerator(const std::string& name);
	const std::string& DefaultGenerator();

	void RegisterSceneKey(const std::string& key, SceneKeyHandler handler);
	const std::vector<std::pair<std::string, SceneKeyHandler>>& SceneKeys();
}
