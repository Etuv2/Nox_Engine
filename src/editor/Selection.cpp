#include "Selection.h"

#include <algorithm>
#include "../SceneNode.h"

namespace nox::editor {

bool Selection::Contains(const NodePtr& node) const
{
	return node && std::find(m_nodes.begin(), m_nodes.end(), node) != m_nodes.end();
}

void Selection::Set(const NodePtr& node)
{
	m_nodes.clear();
	if (node) m_nodes.push_back(node);
}

void Selection::SetAll(const std::vector<NodePtr>& nodes)
{
	m_nodes.clear();
	for (const NodePtr& n : nodes) {
		if (!n) continue;
		m_nodes.erase(std::remove(m_nodes.begin(), m_nodes.end(), n), m_nodes.end());
		m_nodes.push_back(n);
	}
}

void Selection::Add(const NodePtr& node)
{
	if (!node) return;
	m_nodes.erase(std::remove(m_nodes.begin(), m_nodes.end(), node), m_nodes.end());
	m_nodes.push_back(node);
}

void Selection::Toggle(const NodePtr& node)
{
	if (!node) return;
	// In the set and already primary: remove. Otherwise add (or move it to the primary slot).
	if (Primary() == node) Remove(node);
	else Add(node);
}

void Selection::Remove(const NodePtr& node)
{
	m_nodes.erase(std::remove(m_nodes.begin(), m_nodes.end(), node), m_nodes.end());
}

bool Selection::Prune(const NodePtr& root)
{
	const size_t before = m_nodes.size();
	m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [&](const NodePtr& n) {
		if (!n) return true;
		if (n == root) return false;
		// Walk up: the node is live when its chain reaches the root.
		const SceneNode* cur = n.get();
		while (cur && cur != root.get()) {
			auto parent = cur->parentNode.lock();
			if (!parent) return true;
			cur = parent.get();
		}
		return false;
	}), m_nodes.end());
	return m_nodes.size() != before;
}

std::vector<Selection::NodePtr> Selection::TopLevel() const
{
	std::vector<NodePtr> out;
	for (const NodePtr& n : m_nodes) {
		bool covered = false;
		for (const NodePtr& other : m_nodes) {
			if (other != n && IsDescendantOf(n.get(), other.get())) { covered = true; break; }
		}
		if (!covered) out.push_back(n);
	}
	return out;
}

bool IsDescendantOf(const SceneNode* node, const SceneNode* ancestor)
{
	if (!node || !ancestor) return false;
	for (auto parent = node->parentNode.lock(); parent; parent = parent->parentNode.lock()) {
		if (parent.get() == ancestor) return true;
	}
	return false;
}

std::vector<std::shared_ptr<SceneNode>> RangeBetween(const std::vector<std::shared_ptr<SceneNode>>& ordered,
	const std::shared_ptr<SceneNode>& anchor, const std::shared_ptr<SceneNode>& target)
{
	std::vector<std::shared_ptr<SceneNode>> out;
	if (!target) return out;
	const auto a = std::find(ordered.begin(), ordered.end(), anchor);
	const auto t = std::find(ordered.begin(), ordered.end(), target);
	if (a == ordered.end() || t == ordered.end()) {
		out.push_back(target);
		return out;
	}
	const auto lo = std::min(a, t);
	const auto hi = std::max(a, t);
	out.assign(lo, hi + 1);
	return out;
}

} // namespace nox::editor
