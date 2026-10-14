#pragma once
// Prefabs (docs/prefabs.md): a *.prefab.json file holds one node subtree in the scene-file node format.
//
// A scene node may instead be a reference:
//   { "prefab": "prefabs/crate.prefab.json", "name": "Crate 3", "position": [1, 0, 2],
//     "components": { "Light": { "intensity": 2 } },
//     "children": [ ...nodes added to this instance... ],
//     "overrides": { "Lid/Hinge": { "components": { "Transform": ... } }, "Old": { "remove": true } } }
// Expansion (ExpandReferences) replaces the reference by the prefab subtree with these edits applied:
//  - root scalar keys and root "components" (merged per component and property) replace the prefab root's;
//  - "children" are appended to the root's children;
//  - "overrides" address descendants by name path ("A/B"; duplicate sibling names get "#k"); a path entry
//    applies the same keys as a root reference, or removes the node when it carries "remove": true.
// The expanded root keeps "prefab" so the loaded node knows its source (SceneNode::GetPrefabPath).
// Saving writes the reference again with only the differences from the prefab (MakeReference).
//
// Nesting: a prefab may reference other prefabs; depth is limited to kMaxNestingDepth and cycles are errors.
// Pure JSON functions here do no ECS work; SavePrefab and InstantiatePrefab use the SceneLoader.
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "json.hpp"

class SceneGraph;
class SceneLoader;
class SceneNode;

namespace nox::prefab {

inline constexpr const char* kExtension = ".prefab.json";
inline constexpr int kMaxNestingDepth = 8;

// True for a logical path ending in ".prefab.json" (case-insensitive extension).
bool IsPrefabPath(std::string_view logicalPath);

// Child addressing used by override paths: the node name (or its type when unnamed), '/' replaced by '_',
// and "#k" for the k-th later sibling with the same key.
std::vector<std::string> ChildKeys(const nlohmann::json& children);

// Descendant of node addressed by "A/B" (empty path = node itself). Null when absent.
nlohmann::json* FindPath(nlohmann::json& node, const std::string& path);

// Reads the node JSON of a prefab file. Nested references are not expanded here.
std::optional<nlohmann::json> ReadPrefabJson(const std::string& logicalPath, std::string* error);

// Replaces every reference inside node (including node itself) with its expanded subtree. Call once per
// JSON tree; expanded roots keep their "prefab" key, so a second call would expand them again.
bool ExpandReferences(nlohmann::json& node, std::string* error, int depth = 0,
	const std::vector<std::string>& stack = {});

// Reference form of an instance: the keys and children that differ between instanceFlat and prefabFlat (both
// are fully flattened node trees, see SceneLoader::SerializeNodeTree). Returns { "prefab": path, ... }.
nlohmann::json MakeReference(const std::string& prefabPath, const nlohmann::json& instanceFlat,
	const nlohmann::json& prefabFlat);

// Writes JSON to the loose authoring tree at a logical path (creates folders). Loose mode only.
bool WriteJsonFile(const std::string& logicalPath, const nlohmann::json& json, std::string* error);

// Editor and Lua entry points.
// Writes node (its subtree, flattened at the root) to a *.prefab.json asset.
bool SavePrefab(SceneLoader& loader, const std::shared_ptr<SceneNode>& node, const std::string& logicalPath,
	std::string* error);
// Instantiates a prefab under parent (null = scene root). placement holds optional root keys such as
// "name", "position", "rotation" (degrees), "scale" and "components".
std::shared_ptr<SceneNode> InstantiatePrefab(SceneLoader& loader, SceneGraph& graph, const std::string& logicalPath,
	const std::shared_ptr<SceneNode>& parent, const nlohmann::json& placement, std::string* error);

} // namespace nox::prefab
