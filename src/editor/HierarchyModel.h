#pragma once
// Scene hierarchy view model for the editor (no ImGui here). Flattens the node tree into the rows the Hierarchy
// window draws, in display order, after the search filter, the sort mode and the per-scene collapse state.
// Folders are ordinary nodes (SceneNode::FOLDER): they are listed like any other node, with their children below.
//
// Search box syntax: whitespace-separated terms. "t:<type>" keeps nodes of that type tag (folder, light, mesh, audio,
// camera, gui, lpv, skeletal, node; case-insensitive). Any other term is a case-insensitive substring of the node
// name, and every such term must match. A node is shown when it matches or has a matching descendant; the ancestors
// of a match are expanded while the filter is active.
//
// Collapse state is keyed by the node's hierarchy path ("Folder/Child"), so it survives reloads of the same scene
// but not a rename of the node or one of its ancestors.

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include "json.hpp"

class SceneNode;

namespace nox::editor {

enum class HierarchySort { SceneOrder, NameFoldersFirst };

struct HierarchyFilter {
	std::vector<std::string> texts;   // lower-case name substrings, all must match
	std::string typeToken;            // lower-case type tag; empty = none

	bool Active() const { return !texts.empty() || !typeToken.empty(); }
	static HierarchyFilter Parse(const std::string& query);
};

struct HierarchyRow {
	std::shared_ptr<SceneNode> node;
	int depth = 0;                  // 0 = directly below the scene root
	bool hasChildren = false;       // rows drawn below it (before filtering, not counting generated nodes)
	bool open = false;              // children are listed below this row
	std::string path;               // persistence key: slash-separated names from the scene root
};

// Lower-case type tag of a node: folder, light, mesh, audio, camera, gui, lpv, skeletal or node.
std::string NodeTypeToken(const SceneNode& node);
// True when the node matches the filter itself (not counting its descendants).
bool HierarchyNodeMatches(SceneNode& node, const HierarchyFilter& filter);

class HierarchyView {
public:
	// Rows below root in display order (root itself is not a row). Generated nodes (rebuilt on load) are listed
	// only while a filter is active.
	std::vector<HierarchyRow> Build(const std::shared_ptr<SceneNode>& root, const HierarchyFilter& filter,
		HierarchySort sort) const;

	bool IsCollapsed(const std::string& path) const { return m_collapsed.count(path) != 0; }
	void SetCollapsed(const std::string& path, bool collapsed);
	// Collapses every node (CollapseAll) or expands every node (ExpandAll).
	void CollapseAll(const std::shared_ptr<SceneNode>& root);
	void ExpandAll();
	// Expands every ancestor of node so its row is visible. Nodes outside the hierarchy are ignored.
	void Reveal(SceneNode* node);

	// Persistence of the collapse state of one scene: {"collapsed": ["Folder", "Folder/Child", ...]}.
	void Load(const nlohmann::json& saved);
	nlohmann::json Save() const;

private:
	std::unordered_set<std::string> m_collapsed;
};

// Parses the sort mode name stored in the editor settings ("scene" or "name"). Unknown names give SceneOrder.
HierarchySort ParseHierarchySort(const std::string& name);
const char* HierarchySortName(HierarchySort sort);

} // namespace nox::editor
