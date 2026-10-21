#pragma once
// Editor selection set: several nodes, one primary (the last one selected, which drives the Inspector).
// Headless: no ImGui, no Core. The editor workspace owns one and keeps the primary in sync with the
// window manager's single selected node.

#include <memory>
#include <vector>

class SceneNode;

namespace nox::editor {

class Selection {
public:
	using NodePtr = std::shared_ptr<SceneNode>;

	const std::vector<NodePtr>& Nodes() const { return m_nodes; }
	size_t Size() const { return m_nodes.size(); }
	bool Empty() const { return m_nodes.empty(); }
	bool Contains(const NodePtr& node) const;
	// Last node selected (drives the Inspector); null when empty.
	NodePtr Primary() const { return m_nodes.empty() ? nullptr : m_nodes.back(); }

	// Replaces the set with one node (null clears it).
	void Set(const NodePtr& node);
	// Replaces the set; primary is the last entry of nodes (null entries are dropped, duplicates kept once).
	void SetAll(const std::vector<NodePtr>& nodes);
	// Adds a node and makes it primary; a node already in the set is only made primary.
	void Add(const NodePtr& node);
	// Ctrl+click: adds the node when absent (it becomes primary), removes it when present.
	void Toggle(const NodePtr& node);
	void Remove(const NodePtr& node);
	void Clear() { m_nodes.clear(); }
	// Drops nodes that are no longer part of the scene (no live parent chain). Returns true when the set changed.
	bool Prune(const NodePtr& root);

	// The nodes of the set that have no ancestor in the set: group transforms and copy/delete act on these.
	std::vector<NodePtr> TopLevel() const;

private:
	std::vector<NodePtr> m_nodes;
};

// Shift+click: the nodes between anchor and target (inclusive) in the visible row order. When either end is
// not in the list, only target is returned.
std::vector<std::shared_ptr<SceneNode>> RangeBetween(const std::vector<std::shared_ptr<SceneNode>>& ordered,
	const std::shared_ptr<SceneNode>& anchor, const std::shared_ptr<SceneNode>& target);

// True when node lies below ancestor in the scene tree (ancestor itself excluded).
bool IsDescendantOf(const SceneNode* node, const SceneNode* ancestor);

} // namespace nox::editor
