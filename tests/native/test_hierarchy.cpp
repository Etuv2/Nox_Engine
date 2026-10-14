// Scene hierarchy folders, headless: the Hierarchy view model (rows, search, sort, collapse state) and the grouping
// undo step. No ImGui, no Core: plain SceneNodes with the node operations the editor uses.
#include "reflect/Reflection.h"   // first project include: reflection JSON must not pick the other json.hpp

#include "TestFramework.h"

#include "SceneNode.h"
#include "editor/HierarchyModel.h"
#include "editor/InspectorUndo.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using nox::editor::HierarchyFilter;
using nox::editor::HierarchyRow;
using nox::editor::HierarchySort;
using nox::editor::HierarchyView;
using nox::editor::NodeOps;
using nox::editor::NodeTable;
using nox::editor::UndoPush;

namespace {

std::shared_ptr<SceneNode> MakeNode(const std::string& name, SceneNode::NODE_TYPE type = SceneNode::NODE) {
	auto node = std::make_shared<SceneNode>();
	node->SetName(name);
	node->SetNodeType(type);
	return node;
}

std::shared_ptr<SceneNode> Child(const std::shared_ptr<SceneNode>& parent, const std::string& name) {
	for (const auto& c : parent->children) {
		if (c && c->GetName() == name) return c;
	}
	return nullptr;
}

// Row labels as "depth:name", in display order, for compact comparisons.
std::vector<std::string> Labels(const std::vector<HierarchyRow>& rows) {
	std::vector<std::string> out;
	for (const auto& r : rows) out.push_back(std::to_string(r.depth) + ":" + r.node->GetName());
	return out;
}

// Scene: Props (folder) { Crate, Deep (folder) { Lamp (light) } }, Loose, Zeta, Alpha (folder).
std::shared_ptr<SceneNode> MakeScene() {
	auto root = MakeNode("Root");
	auto props = MakeNode("Props", SceneNode::FOLDER);
	auto deep = MakeNode("Deep", SceneNode::FOLDER);
	root->AddChild(props);
	props->AddChild(MakeNode("Crate"));
	props->AddChild(deep);
	deep->AddChild(MakeNode("Lamp", SceneNode::LIGHT));
	root->AddChild(MakeNode("Loose"));
	root->AddChild(MakeNode("Zeta"));
	root->AddChild(MakeNode("Alpha", SceneNode::FOLDER));
	return root;
}

// Subtree JSON in the scene format: {"name", "children"} plus the folder type.
nlohmann::json SubtreeJson(const std::shared_ptr<SceneNode>& n) {
	nlohmann::json children = nlohmann::json::array();
	for (const auto& c : n->children) children.push_back(SubtreeJson(c));
	nlohmann::json j = { {"name", n->GetName()}, {"children", children} };
	if (n->IsFolder()) j["type"] = "folder";
	return j;
}

std::shared_ptr<SceneNode> BuildSubtree(const nlohmann::json& j, const std::shared_ptr<SceneNode>& parent) {
	const bool folder = j.value("type", std::string()) == "folder";
	auto node = MakeNode(j.value("name", std::string("node")), folder ? SceneNode::FOLDER : SceneNode::NODE);
	if (parent) parent->AddChild(node);
	if (j.contains("children")) {
		for (const auto& c : j["children"]) BuildSubtree(c, node);
	}
	return node;
}

NodeOps FakeOps() {
	NodeOps ops;
	ops.serialize = [](const std::shared_ptr<SceneNode>& n) { return SubtreeJson(n); };
	ops.instantiate = [](const nlohmann::json& j, const std::shared_ptr<SceneNode>& parent) -> std::shared_ptr<SceneNode> {
		if (!parent) return nullptr;
		return BuildSubtree(j, parent);
	};
	ops.destroy = [](const std::shared_ptr<SceneNode>& n) {
		auto parent = n->parentNode.lock();
		if (!parent) return false;
		auto& siblings = parent->children;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), n), siblings.end());
		return true;
	};
	ops.reparent = [](const std::shared_ptr<SceneNode>& n, const std::shared_ptr<SceneNode>& p, int index, bool) {
		if (!p) return false;
		p->AddChild(n);
		if (index >= 0) {
			auto& siblings = p->children;
			siblings.erase(std::remove(siblings.begin(), siblings.end(), n), siblings.end());
			siblings.insert(siblings.begin() + std::min(static_cast<size_t>(index), siblings.size()), n);
		}
		return true;
	};
	return ops;
}

struct Step {
	std::string label;
	std::function<void()> undo, redo;
};

} // namespace

NOX_TEST(Hierarchy, FilterParsesTypeTokensAndNameTerms)
{
	const HierarchyFilter f = HierarchyFilter::Parse("  T:Folder   Crate  wheel ");
	NOX_CHECK_EQ(f.typeToken, std::string("folder"));
	NOX_REQUIRE(f.texts.size() == 2);
	NOX_CHECK_EQ(f.texts[0], std::string("crate"));
	NOX_CHECK_EQ(f.texts[1], std::string("wheel"));
	NOX_CHECK(!HierarchyFilter::Parse("").Active());
	NOX_CHECK(HierarchyFilter::Parse("t:light").Active());
}

NOX_TEST(Hierarchy, BuildListsFoldersAndChildrenInSceneOrder)
{
	auto root = MakeScene();
	HierarchyView view;
	const auto rows = view.Build(root, HierarchyFilter(), HierarchySort::SceneOrder);
	const std::vector<std::string> expected = {
		"0:Props", "1:Crate", "1:Deep", "2:Lamp", "0:Loose", "0:Zeta", "0:Alpha" };
	NOX_CHECK(Labels(rows) == expected);
	NOX_CHECK(rows[0].hasChildren && rows[0].open);
	NOX_CHECK_EQ(rows[0].path, std::string("Props"));
	NOX_CHECK_EQ(rows[3].path, std::string("Props/Deep/Lamp"));
	NOX_CHECK(!rows[6].hasChildren);
}

NOX_TEST(Hierarchy, CollapsedFolderKeepsItsRowAndHidesChildren)
{
	auto root = MakeScene();
	HierarchyView view;
	view.SetCollapsed("Props", true);
	const auto rows = view.Build(root, HierarchyFilter(), HierarchySort::SceneOrder);
	NOX_CHECK(Labels(rows) == std::vector<std::string>({ "0:Props", "0:Loose", "0:Zeta", "0:Alpha" }));
	NOX_CHECK(rows[0].hasChildren && !rows[0].open);

	// Collapse state survives a save/load of the scene's view state.
	HierarchyView reloaded;
	reloaded.Load(view.Save());
	NOX_CHECK(reloaded.IsCollapsed("Props"));
	NOX_CHECK(!reloaded.IsCollapsed("Props/Deep"));
}

NOX_TEST(Hierarchy, SearchKeepsMatchesAndTheirAncestors)
{
	auto root = MakeScene();
	HierarchyView view;
	view.SetCollapsed("Props", true);   // the filter expands the path to a match regardless
	const auto rows = view.Build(root, HierarchyFilter::Parse("lamp"), HierarchySort::SceneOrder);
	NOX_CHECK(Labels(rows) == std::vector<std::string>({ "0:Props", "1:Deep", "2:Lamp" }));

	const auto folders = view.Build(root, HierarchyFilter::Parse("t:folder"), HierarchySort::SceneOrder);
	NOX_CHECK(Labels(folders) == std::vector<std::string>({ "0:Props", "1:Deep", "0:Alpha" }));

	const auto none = view.Build(root, HierarchyFilter::Parse("nothing-matches"), HierarchySort::SceneOrder);
	NOX_CHECK(none.empty());
}

NOX_TEST(Hierarchy, NameSortPutsFoldersFirstThenAlphabetical)
{
	auto root = MakeScene();
	HierarchyView view;
	const auto rows = view.Build(root, HierarchyFilter(), HierarchySort::NameFoldersFirst);
	NOX_CHECK(Labels(rows) == std::vector<std::string>({
		"0:Alpha", "0:Props", "1:Deep", "2:Lamp", "1:Crate", "0:Loose", "0:Zeta" }));
}

NOX_TEST(Hierarchy, RevealExpandsEveryAncestorOfASelection)
{
	auto root = MakeScene();
	HierarchyView view;
	view.CollapseAll(root);
	NOX_CHECK(view.IsCollapsed("Props"));
	NOX_CHECK(view.IsCollapsed("Props/Deep"));
	view.Reveal(Child(Child(root, "Props"), "Deep")->children[0].get());
	NOX_CHECK(!view.IsCollapsed("Props"));
	NOX_CHECK(!view.IsCollapsed("Props/Deep"));
	view.ExpandAll();
	NOX_CHECK(!view.IsCollapsed("Props"));
}

NOX_TEST(Hierarchy, GroupIntoFolderUndoRestoresTheSlotAndRedoRebuildsIt)
{
	auto root = MakeNode("Root");
	auto a = MakeNode("A");
	auto b = MakeNode("B");
	auto c = MakeNode("C");
	root->AddChild(a);
	root->AddChild(b);
	root->AddChild(c);

	NodeTable table;
	NodeOps ops = FakeOps();
	std::vector<Step> steps;
	UndoPush push = [&steps](const std::string& label, std::function<void()> undo, std::function<void()> redo) {
		steps.push_back({ label, std::move(undo), std::move(redo) });
	};
	const nlohmann::json folderJson = { {"type", "folder"}, {"name", "Group"} };
	auto folder = nox::editor::GroupIntoFolderUndoable(table, ops, push, b, folderJson);
	NOX_REQUIRE(folder != nullptr);
	NOX_REQUIRE(steps.size() == 1);
	NOX_CHECK(folder->IsFolder());
	NOX_CHECK(root->children.size() == 3);
	NOX_CHECK(root->children[1] == folder);
	NOX_CHECK(b->parentNode.lock() == folder);
	NOX_CHECK(c->parentNode.lock() == root);

	steps[0].undo();
	NOX_REQUIRE(root->children.size() == 3);
	NOX_CHECK(root->children[1] == b);
	NOX_CHECK(b->children.empty());
	NOX_CHECK(b->parentNode.lock() == root);

	steps[0].redo();
	NOX_REQUIRE(root->children.size() == 3);
	NOX_CHECK(root->children[1]->IsFolder());
	NOX_CHECK(root->children[1]->GetName() == "Group");
	NOX_REQUIRE(root->children[1]->children.size() == 1);
	NOX_CHECK(root->children[1]->children[0]->GetName() == "B");
}
