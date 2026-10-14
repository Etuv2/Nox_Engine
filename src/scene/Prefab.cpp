#include "Prefab.h"

#include "../SceneGraph.h"
#include "../SceneLoader.h"
#include "../SceneNode.h"
#include "../assets/AssetIO.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

using nlohmann::json;

namespace nox::prefab {

namespace {

const char* const kRefKey = "prefab";
const char* const kOverridesKey = "overrides";
const char* const kChildrenKey = "children";
const char* const kComponentsKey = "components";

bool IsRefKeyAbsent(const std::string& key) {
	return key == kRefKey || key == kOverridesKey || key == kChildrenKey || key == kComponentsKey;
}

// Merges a "components" object into dst: per component, per property; a missing component is copied whole.
void MergeComponents(json& dst, const json& src) {
	if (!src.is_object()) return;
	if (!dst.contains(kComponentsKey) || !dst[kComponentsKey].is_object()) dst[kComponentsKey] = json::object();
	json& target = dst[kComponentsKey];
	for (auto it = src.begin(); it != src.end(); ++it) {
		if (!it.value().is_object()) continue;
		if (!target.contains(it.key()) || !target[it.key()].is_object()) {
			target[it.key()] = it.value();
			continue;
		}
		for (auto prop = it.value().begin(); prop != it.value().end(); ++prop) {
			target[it.key()][prop.key()] = prop.value();
		}
	}
}

// Applies the keys of a reference (or override entry) to a node of the expanded tree.
bool ApplyEdits(json& target, const json& edits, std::string* error, int depth, const std::vector<std::string>& stack) {
	for (auto it = edits.begin(); it != edits.end(); ++it) {
		if (IsRefKeyAbsent(it.key()) || it.key() == "remove") continue;
		target[it.key()] = it.value();
	}
	if (edits.contains(kComponentsKey)) MergeComponents(target, edits[kComponentsKey]);
	if (edits.contains(kChildrenKey) && edits[kChildrenKey].is_array()) {
		if (!target.contains(kChildrenKey) || !target[kChildrenKey].is_array()) target[kChildrenKey] = json::array();
		for (json child : edits[kChildrenKey]) {
			if (!ExpandReferences(child, error, depth + 1, stack)) return false;
			target[kChildrenKey].push_back(std::move(child));
		}
	}
	return true;
}

// Removes the node addressed by path (its parent's child entry).
bool RemovePath(json& root, const std::string& path) {
	const size_t slash = path.rfind('/');
	const std::string parentPath = slash == std::string::npos ? std::string() : path.substr(0, slash);
	const std::string key = slash == std::string::npos ? path : path.substr(slash + 1);
	json* parent = FindPath(root, parentPath);
	if (!parent || !parent->contains(kChildrenKey) || !(*parent)[kChildrenKey].is_array()) return false;
	json& children = (*parent)[kChildrenKey];
	const std::vector<std::string> keys = ChildKeys(children);
	for (size_t i = 0; i < keys.size(); ++i) {
		if (keys[i] == key) {
			children.erase(children.begin() + static_cast<std::ptrdiff_t>(i));
			return true;
		}
	}
	return false;
}

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

void DiffNode(const json& inst, const json& base, const std::string& path, json& local, json& overrides) {
	for (auto it = inst.begin(); it != inst.end(); ++it) {
		if (it.key() == kChildrenKey || it.key() == kComponentsKey) continue;
		if (!base.contains(it.key()) || base[it.key()] != it.value()) local[it.key()] = it.value();
	}

	if (inst.contains(kComponentsKey) && inst[kComponentsKey].is_object()) {
		const json baseComps = base.contains(kComponentsKey) && base[kComponentsKey].is_object()
			? base[kComponentsKey] : json::object();
		json comps = json::object();
		for (auto it = inst[kComponentsKey].begin(); it != inst[kComponentsKey].end(); ++it) {
			if (!it.value().is_object()) continue;
			if (!baseComps.contains(it.key()) || !baseComps[it.key()].is_object()) {
				comps[it.key()] = it.value();
				continue;
			}
			json changed = json::object();
			for (auto prop = it.value().begin(); prop != it.value().end(); ++prop) {
				if (!baseComps[it.key()].contains(prop.key()) || baseComps[it.key()][prop.key()] != prop.value()) {
					changed[prop.key()] = prop.value();
				}
			}
			if (!changed.empty()) comps[it.key()] = changed;
		}
		if (!comps.empty()) local[kComponentsKey] = comps;
	}

	const json emptyArray = json::array();
	const json& ic = inst.contains(kChildrenKey) && inst[kChildrenKey].is_array() ? inst[kChildrenKey] : emptyArray;
	const json& bc = base.contains(kChildrenKey) && base[kChildrenKey].is_array() ? base[kChildrenKey] : emptyArray;
	const std::vector<std::string> ik = ChildKeys(ic);
	const std::vector<std::string> bk = ChildKeys(bc);
	std::map<std::string, size_t> baseIndex;
	for (size_t i = 0; i < bk.size(); ++i) baseIndex[bk[i]] = i;
	const std::set<std::string> instKeys(ik.begin(), ik.end());

	auto childPath = [&path](const std::string& key) { return path.empty() ? key : path + "/" + key; };

	json added = json::array();
	for (size_t i = 0; i < ic.size(); ++i) {
		const auto found = baseIndex.find(ik[i]);
		if (found == baseIndex.end()) {
			added.push_back(ic[i]);
			continue;
		}
		json childLocal = json::object();
		const std::string cp = childPath(ik[i]);
		DiffNode(ic[i], bc[found->second], cp, childLocal, overrides);
		if (!childLocal.empty()) overrides[cp] = childLocal;
	}
	for (const std::string& key : bk) {
		if (instKeys.count(key) == 0) overrides[childPath(key)] = json{ { "remove", true } };
	}
	if (!added.empty()) local[kChildrenKey] = added;
}

} // namespace

bool IsPrefabPath(std::string_view logicalPath) {
	const std::string lower = Lower(std::string(logicalPath));
	const std::string ext = kExtension;
	return lower.size() > ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0;
}

std::vector<std::string> ChildKeys(const json& children) {
	std::vector<std::string> keys;
	if (!children.is_array()) return keys;
	std::map<std::string, int> seen;
	for (const json& child : children) {
		std::string base;
		if (child.is_object()) {
			if (child.contains("name") && child["name"].is_string()) base = child["name"].get<std::string>();
			if (base.empty() && child.contains("type") && child["type"].is_string()) base = child["type"].get<std::string>();
		}
		if (base.empty()) base = "node";
		std::replace(base.begin(), base.end(), '/', '_');
		const int n = seen[base]++;
		keys.push_back(n == 0 ? base : base + "#" + std::to_string(n));
	}
	return keys;
}

json* FindPath(json& node, const std::string& path) {
	json* cur = &node;
	if (path.empty()) return cur;
	size_t start = 0;
	while (start <= path.size()) {
		const size_t slash = path.find('/', start);
		const std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		if (!cur->is_object() || !cur->contains(kChildrenKey) || !(*cur)[kChildrenKey].is_array()) return nullptr;
		json& children = (*cur)[kChildrenKey];
		const std::vector<std::string> keys = ChildKeys(children);
		size_t idx = keys.size();
		for (size_t i = 0; i < keys.size(); ++i) {
			if (keys[i] == segment) { idx = i; break; }
		}
		if (idx == keys.size()) return nullptr;
		cur = &children[idx];
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
	return cur;
}

std::optional<json> ReadPrefabJson(const std::string& logicalPath, std::string* error) {
	if (!IsPrefabPath(logicalPath)) {
		if (error) *error = "not a prefab path (expected *.prefab.json): " + logicalPath;
		return std::nullopt;
	}
	std::string readError;
	const auto text = AssetIO::ReadText(logicalPath, &readError);
	if (!text) {
		if (error) *error = "cannot read prefab '" + logicalPath + "': " + readError;
		return std::nullopt;
	}
	json parsed = json::parse(*text, nullptr, false);
	if (!parsed.is_object()) {
		if (error) *error = "prefab '" + logicalPath + "' is not a JSON node object";
		return std::nullopt;
	}
	if (parsed.contains(kRefKey)) {
		if (error) *error = "prefab '" + logicalPath + "' has a prefab reference on its root; nest references in children";
		return std::nullopt;
	}
	return parsed;
}

bool ExpandReferences(json& node, std::string* error, int depth, const std::vector<std::string>& stack) {
	if (!node.is_object()) return true;
	if (!node.contains(kRefKey)) {
		if (node.contains(kChildrenKey) && node[kChildrenKey].is_array()) {
			for (json& child : node[kChildrenKey]) {
				if (!ExpandReferences(child, error, depth, stack)) return false;
			}
		}
		return true;
	}

	if (!node[kRefKey].is_string()) {
		if (error) *error = "prefab reference must be a string path";
		return false;
	}
	const std::string path = node[kRefKey].get<std::string>();
	if (depth >= kMaxNestingDepth) {
		if (error) *error = "prefab nesting deeper than " + std::to_string(kMaxNestingDepth) + " at '" + path + "'";
		return false;
	}
	if (std::find(stack.begin(), stack.end(), path) != stack.end()) {
		if (error) *error = "prefab '" + path + "' references itself";
		return false;
	}

	std::optional<json> prefab = ReadPrefabJson(path, error);
	if (!prefab) return false;

	std::vector<std::string> nextStack = stack;
	nextStack.push_back(path);
	json expanded = std::move(*prefab);
	if (!ExpandReferences(expanded, error, depth + 1, nextStack)) return false;
	expanded[kRefKey] = path;

	// Root edits first; overrides and removals afterwards so paths refer to the expanded tree.
	if (!ApplyEdits(expanded, node, error, depth, nextStack)) return false;
	if (node.contains(kOverridesKey) && node[kOverridesKey].is_object()) {
		std::vector<std::string> removals;
		for (auto it = node[kOverridesKey].begin(); it != node[kOverridesKey].end(); ++it) {
			if (it.value().is_object() && it.value().value("remove", false)) {
				removals.push_back(it.key());
				continue;
			}
			json* target = FindPath(expanded, it.key());
			if (!target) {
				std::cerr << "[Prefab] Warning: override path '" << it.key() << "' not found in prefab '" << path
					<< "'; skipped" << std::endl;
				continue;
			}
			if (!ApplyEdits(*target, it.value(), error, depth, nextStack)) return false;
		}
		for (const std::string& removePath : removals) {
			if (!RemovePath(expanded, removePath)) {
				std::cerr << "[Prefab] Warning: removed path '" << removePath << "' not found in prefab '" << path
					<< "'" << std::endl;
			}
		}
	}
	node = std::move(expanded);
	return true;
}

json MakeReference(const std::string& prefabPath, const json& instanceFlat, const json& prefabFlat) {
	json local = json::object();
	json overrides = json::object();
	DiffNode(instanceFlat, prefabFlat, "", local, overrides);
	json ref = json::object();
	ref[kRefKey] = prefabPath;
	for (auto it = local.begin(); it != local.end(); ++it) ref[it.key()] = it.value();
	if (!overrides.empty()) ref[kOverridesKey] = overrides;
	return ref;
}

bool WriteJsonFile(const std::string& logicalPath, const json& value, std::string* error) {
	if (AssetIO::GetMode() != AssetIO::Mode::Loose) {
		if (error) *error = "prefabs can only be written in loose (source) mode";
		return false;
	}
	const auto physical = AssetIO::SourcePath(logicalPath);
	if (!physical) {
		if (error) *error = "invalid prefab path: " + logicalPath;
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(physical->parent_path(), ec);
	std::ofstream out(*physical, std::ios::binary);
	if (!out) {
		if (error) *error = "cannot open for writing: " + physical->string();
		return false;
	}
	out << value.dump(2);
	return static_cast<bool>(out);
}

bool SavePrefab(SceneLoader& loader, const std::shared_ptr<SceneNode>& node, const std::string& logicalPath,
	std::string* error) {
	if (!node) {
		if (error) *error = "no node to save";
		return false;
	}
	if (!IsPrefabPath(logicalPath)) {
		if (error) *error = "prefab path must end with .prefab.json: " + logicalPath;
		return false;
	}
	// Root flattened (a prefab is its own content); nested instances stay references.
	const json subtree = loader.SerializeNodeTree(node, true, true);
	return WriteJsonFile(logicalPath, subtree, error);
}

std::shared_ptr<SceneNode> InstantiatePrefab(SceneLoader& loader, SceneGraph& graph, const std::string& logicalPath,
	const std::shared_ptr<SceneNode>& parent, const json& placement, std::string* error) {
	if (!IsPrefabPath(logicalPath)) {
		if (error) *error = "not a prefab path: " + logicalPath;
		return nullptr;
	}
	// Reading first reports a missing or invalid prefab with its reason; InstantiateNode expands the reference
	// itself (expanding twice would apply the edits twice).
	if (!ReadPrefabJson(logicalPath, error)) return nullptr;
	json reference = placement.is_object() ? placement : json::object();
	reference[kRefKey] = logicalPath;
	auto parentNode = parent ? parent : graph.GetRoot();
	auto node = loader.InstantiateNode(graph, reference, parentNode, logicalPath);
	if (!node && error && error->empty()) *error = "prefab instantiation failed: " + logicalPath;
	return node;
}

} // namespace nox::prefab
