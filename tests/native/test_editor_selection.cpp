// Editor selection set, range select, clipboard documents and paste naming, headless: plain SceneNodes and
// JSON, no ImGui, no Core.
#include "reflect/Reflection.h"   // first project include: reflection JSON must not pick the other json.hpp

#include "TestFramework.h"

#include "SceneNode.h"
#include "editor/Clipboard.h"
#include "editor/Selection.h"

#include <memory>
#include <string>
#include <vector>

using nox::editor::Selection;

namespace {

std::shared_ptr<SceneNode> MakeNode(const std::string& name) {
	auto node = std::make_shared<SceneNode>();
	node->SetName(name);
	return node;
}

void Attach(const std::shared_ptr<SceneNode>& parent, const std::shared_ptr<SceneNode>& child) {
	child->parentNode = parent;
	parent->children.push_back(child);
}

} // namespace

NOX_TEST(EditorSelection, ToggleAddsRemovesAndMovesPrimary)
{
	auto a = MakeNode("a");
	auto b = MakeNode("b");
	Selection sel;
	sel.Set(a);
	NOX_CHECK_EQ(sel.Size(), size_t(1));
	NOX_CHECK(sel.Primary() == a);

	sel.Toggle(b);                       // Ctrl+click adds and becomes primary
	NOX_CHECK_EQ(sel.Size(), size_t(2));
	NOX_CHECK(sel.Primary() == b);

	sel.Toggle(a);                       // not primary: moves to primary, set unchanged in size
	NOX_CHECK_EQ(sel.Size(), size_t(2));
	NOX_CHECK(sel.Primary() == a);

	sel.Toggle(a);                       // primary again: removed
	NOX_CHECK_EQ(sel.Size(), size_t(1));
	NOX_CHECK(sel.Primary() == b);
	NOX_CHECK(!sel.Contains(a));
}

NOX_TEST(EditorSelection, SetAllKeepsOrderAndDropsDuplicatesAndNulls)
{
	auto a = MakeNode("a");
	auto b = MakeNode("b");
	Selection sel;
	sel.SetAll({ a, nullptr, b, a });
	NOX_REQUIRE(sel.Size() == 2);
	NOX_CHECK(sel.Nodes()[0] == b);
	NOX_CHECK(sel.Nodes()[1] == a);
	NOX_CHECK(sel.Primary() == a);
	sel.Clear();
	NOX_CHECK(sel.Empty());
	NOX_CHECK(sel.Primary() == nullptr);
}

NOX_TEST(EditorSelection, TopLevelSkipsNodesWhoseAncestorIsSelected)
{
	auto root = MakeNode("root");
	auto folder = MakeNode("folder");
	auto child = MakeNode("child");
	auto other = MakeNode("other");
	Attach(root, folder);
	Attach(folder, child);
	Attach(root, other);

	Selection sel;
	sel.SetAll({ child, folder, other });
	const auto top = sel.TopLevel();
	NOX_REQUIRE(top.size() == 2);
	NOX_CHECK(top[0] == folder);
	NOX_CHECK(top[1] == other);
}

NOX_TEST(EditorSelection, PruneDropsDetachedNodes)
{
	auto root = MakeNode("root");
	auto a = MakeNode("a");
	auto b = MakeNode("b");
	Attach(root, a);
	Attach(root, b);
	Selection sel;
	sel.SetAll({ a, b });
	// Detach b from the tree (as a delete does).
	root->children.erase(root->children.begin() + 1);
	b->parentNode.reset();
	NOX_CHECK(sel.Prune(root));
	NOX_REQUIRE(sel.Size() == 1);
	NOX_CHECK(sel.Primary() == a);
	NOX_CHECK(!sel.Prune(root));
}

NOX_TEST(EditorSelection, RangeBetweenFollowsVisibleOrderInBothDirections)
{
	auto a = MakeNode("a");
	auto b = MakeNode("b");
	auto c = MakeNode("c");
	auto d = MakeNode("d");
	const std::vector<std::shared_ptr<SceneNode>> rows{ a, b, c, d };

	const auto forward = nox::editor::RangeBetween(rows, b, d);
	NOX_REQUIRE(forward.size() == 3);
	NOX_CHECK(forward[0] == b && forward[2] == d);

	const auto backward = nox::editor::RangeBetween(rows, d, b);
	NOX_CHECK_EQ(backward.size(), size_t(3));

	const auto unknown = nox::editor::RangeBetween(rows, nullptr, c);
	NOX_REQUIRE(unknown.size() == 1);
	NOX_CHECK(unknown[0] == c);
}

NOX_TEST(EditorClipboard, DocumentRoundTripsAndRejectsForeignText)
{
	const nlohmann::json n1 = { { "name", "Crate" }, { "type", "node" } };
	const nlohmann::json n2 = { { "name", "Wheel" } };
	const std::string text = nox::editor::MakeClipboardDocument({ n1, n2 }).dump();

	std::vector<nlohmann::json> nodes;
	NOX_REQUIRE(nox::editor::ParseClipboardDocument(text, nodes));
	NOX_REQUIRE(nodes.size() == 2);
	NOX_CHECK_EQ(nodes[0].value("name", std::string()), std::string("Crate"));
	NOX_CHECK_EQ(nodes[1].value("name", std::string()), std::string("Wheel"));

	NOX_CHECK(!nox::editor::ParseClipboardDocument("hello world", nodes));
	NOX_CHECK(!nox::editor::ParseClipboardDocument("{\"nodes\": []}", nodes));
	NOX_CHECK(!nox::editor::ParseClipboardDocument("{ broken", nodes));
	NOX_CHECK(!nox::editor::ParseClipboardDocument(
		"{\"format\":\"nox.scene_nodes\",\"version\":1,\"nodes\":[]}", nodes));
	NOX_CHECK(!nox::editor::ParseClipboardDocument(
		"{\"format\":\"nox.scene_nodes\",\"version\":99,\"nodes\":[{\"name\":\"x\"}]}", nodes));
}

NOX_TEST(EditorClipboard, UniqueSiblingNameCountsUpFromTheStem)
{
	using nox::editor::UniqueSiblingName;
	NOX_CHECK_EQ(UniqueSiblingName("Crate", {}), std::string("Crate"));
	NOX_CHECK_EQ(UniqueSiblingName("Crate", { "Wheel" }), std::string("Crate"));
	NOX_CHECK_EQ(UniqueSiblingName("Crate", { "Crate" }), std::string("Crate (1)"));
	NOX_CHECK_EQ(UniqueSiblingName("Crate", { "Crate", "Crate (1)" }), std::string("Crate (2)"));
	NOX_CHECK_EQ(UniqueSiblingName("Crate (1)", { "Crate (1)" }), std::string("Crate (2)"));
	NOX_CHECK_EQ(UniqueSiblingName("Crate (1)", { "Crate (1)", "Crate (2)" }), std::string("Crate (3)"));
	NOX_CHECK_EQ(UniqueSiblingName("Box (old)", { "Box (old)" }), std::string("Box (old) (1)"));
}
