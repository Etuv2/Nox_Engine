#include "HierarchyModel.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include "../SceneNode.h"

namespace nox::editor {

namespace {

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// Children in display order. Generated nodes are left out unless a filter is active (as before the folder rework).
std::vector<std::shared_ptr<SceneNode>> ListedChildren(SceneNode& parent, const HierarchyFilter& filter, HierarchySort sort) {
	std::vector<std::shared_ptr<SceneNode>> out;
	for (const auto& c : parent.children) {
		if (!c) continue;
		if (c->IsGenerated() && !filter.Active()) continue;
		out.push_back(c);
	}
	if (sort == HierarchySort::NameFoldersFirst) {
		std::stable_sort(out.begin(), out.end(), [](const std::shared_ptr<SceneNode>& a, const std::shared_ptr<SceneNode>& b) {
			if (a->IsFolder() != b->IsFolder()) return a->IsFolder();
			return Lower(a->GetName()) < Lower(b->GetName());
		});
	}
	return out;
}

// True when node or a listed descendant matches the filter.
bool SubtreeMatches(SceneNode& node, const HierarchyFilter& filter) {
	if (HierarchyNodeMatches(node, filter)) return true;
	for (const auto& c : node.children) {
		if (c && SubtreeMatches(*c, filter)) return true;
	}
	return false;
}

void Emit(SceneNode& parent, const std::string& parentPath, int depth, const HierarchyView& view,
	const HierarchyFilter& filter, HierarchySort sort, std::vector<HierarchyRow>& out) {
	for (const auto& child : ListedChildren(parent, filter, sort)) {
		if (filter.Active() && !SubtreeMatches(*child, filter)) continue;
		const std::string path = parentPath.empty() ? child->GetName() : parentPath + "/" + child->GetName();
		const std::vector<std::shared_ptr<SceneNode>> children = ListedChildren(*child, filter, sort);
		const bool hasChildren = !children.empty();
		bool open = false;
		if (hasChildren) {
			if (filter.Active()) {
				open = std::any_of(children.begin(), children.end(),
					[&](const std::shared_ptr<SceneNode>& c) { return SubtreeMatches(*c, filter); });
			}
			else {
				open = !view.IsCollapsed(path);
			}
		}
		HierarchyRow row;
		row.node = child;
		row.depth = depth;
		row.hasChildren = hasChildren;
		row.open = open;
		row.path = path;
		out.push_back(row);
		if (open) Emit(*child, path, depth + 1, view, filter, sort, out);
	}
}

void CollectPaths(SceneNode& node, const std::string& parentPath, std::vector<std::string>& paths) {
	for (const auto& c : node.children) {
		if (!c) continue;
		const std::string path = parentPath.empty() ? c->GetName() : parentPath + "/" + c->GetName();
		if (!c->children.empty()) paths.push_back(path);
		CollectPaths(*c, path, paths);
	}
}

} // namespace

HierarchyFilter HierarchyFilter::Parse(const std::string& query) {
	HierarchyFilter filter;
	std::istringstream in(Lower(query));
	std::string token;
	while (in >> token) {
		if (token.rfind("t:", 0) == 0) {
			if (token.size() > 2) filter.typeToken = token.substr(2);
		}
		else {
			filter.texts.push_back(token);
		}
	}
	return filter;
}

std::string NodeTypeToken(const SceneNode& node) {
	switch (node.GetNodeType()) {
	case SceneNode::FOLDER: return "folder";
	case SceneNode::LIGHT: return "light";
	case SceneNode::MODEL: return "mesh";
	case SceneNode::AUDIO: return "audio";
	case SceneNode::CAMERA: return "camera";
	case SceneNode::GUI: return "gui";
	case SceneNode::LPV_VOLUME: return "lpv";
	case SceneNode::SKELETAL: return "skeletal";
	default: return "node";
	}
}

bool HierarchyNodeMatches(SceneNode& node, const HierarchyFilter& filter) {
	if (!filter.typeToken.empty() && NodeTypeToken(node) != filter.typeToken) return false;
	if (!filter.texts.empty()) {
		const std::string name = Lower(node.GetName());
		for (const std::string& text : filter.texts) {
			if (name.find(text) == std::string::npos) return false;
		}
	}
	return true;
}

std::vector<HierarchyRow> HierarchyView::Build(const std::shared_ptr<SceneNode>& root, const HierarchyFilter& filter,
	HierarchySort sort) const {
	std::vector<HierarchyRow> rows;
	if (root) Emit(*root, std::string(), 0, *this, filter, sort, rows);
	return rows;
}

void HierarchyView::SetCollapsed(const std::string& path, bool collapsed) {
	if (collapsed) m_collapsed.insert(path);
	else m_collapsed.erase(path);
}

void HierarchyView::CollapseAll(const std::shared_ptr<SceneNode>& root) {
	if (!root) return;
	std::vector<std::string> paths;
	CollectPaths(*root, std::string(), paths);
	m_collapsed.insert(paths.begin(), paths.end());
}

void HierarchyView::ExpandAll() {
	m_collapsed.clear();
}

void HierarchyView::Reveal(SceneNode* node) {
	if (!node) return;
	// Every proper prefix of the node's path is an ancestor; the node's own row stays as it is.
	const std::string path = node->GetHierarchyPath();
	size_t end = path.find('/');
	while (end != std::string::npos) {
		m_collapsed.erase(path.substr(0, end));
		end = path.find('/', end + 1);
	}
}

void HierarchyView::Load(const nlohmann::json& saved) {
	m_collapsed.clear();
	if (!saved.is_object() || !saved.contains("collapsed") || !saved["collapsed"].is_array()) return;
	for (const auto& p : saved["collapsed"]) {
		if (p.is_string()) m_collapsed.insert(p.get<std::string>());
	}
}

nlohmann::json HierarchyView::Save() const {
	std::vector<std::string> sorted(m_collapsed.begin(), m_collapsed.end());
	std::sort(sorted.begin(), sorted.end());
	nlohmann::json collapsed = nlohmann::json::array();
	for (const auto& p : sorted) collapsed.push_back(p);
	return { {"collapsed", collapsed} };
}

HierarchySort ParseHierarchySort(const std::string& name) {
	return name == "name" ? HierarchySort::NameFoldersFirst : HierarchySort::SceneOrder;
}

const char* HierarchySortName(HierarchySort sort) {
	return sort == HierarchySort::NameFoldersFirst ? "name" : "scene";
}

} // namespace nox::editor
