// Editor inspector undo: property edits, coalesced drags, component add/remove and node lifetime steps,
// exercised headless with plain SceneNodes (no ImGui, no Core).
#include "reflect/Reflection.h"   // first project include: reflection JSON must not pick the other json.hpp

#include "TestFramework.h"

#include "LightNode.h"
#include "PointLight.h"
#include "SceneNode.h"
#include "editor/InspectorUndo.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace nox::reflect;
using nox::editor::NodeOps;
using nox::editor::NodeTable;
using nox::editor::PropertyEdit;
using nox::editor::UndoPush;

namespace {

struct Step {
	std::string label;
	std::function<void()> undo, redo;
};

// Collects pushed steps so the test can run undo/redo itself.
struct Recorder {
	std::vector<Step> steps;
	UndoPush push() {
		return [this](const std::string& label, std::function<void()> undo, std::function<void()> redo) {
			steps.push_back({ label, std::move(undo), std::move(redo) });
		};
	}
};

std::shared_ptr<SceneNode> MakeNode(const std::string& name) {
	auto node = std::make_shared<SceneNode>();
	node->SetName(name);
	return node;
}

// Subtree JSON in the scene format: {"name", "children"}.
nlohmann::json SubtreeJson(const std::shared_ptr<SceneNode>& n) {
	nlohmann::json children = nlohmann::json::array();
	for (const auto& c : n->children) children.push_back(SubtreeJson(c));
	return { {"name", n->GetName()}, {"children", children} };
}

std::shared_ptr<SceneNode> BuildSubtree(const nlohmann::json& j, const std::shared_ptr<SceneNode>& parent) {
	auto node = MakeNode(j.value("name", std::string("node")));
	if (parent) parent->AddChild(node);
	if (j.contains("children")) {
		for (const auto& c : j["children"]) BuildSubtree(c, node);
	}
	return node;
}

// Fake node operations: the same contract the editor fills from Core, on plain SceneNodes.
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

// Test-only component on a LightNode: "present" when the Light facet is, attach/detach a PointLight.
// The engine's Light descriptor has no add/remove, so the round trip uses this stand-in.
void RegisterTestLightSlot() {
	ComponentDesc c;
	c.name = "EditorTestLightSlot";
	c.category = "Test";
	c.has = [](const SceneNode& n) { return Registry::Get().Find("Light")->has(n); };
	c.add = [](SceneNode& n) {
		auto ln = dynamic_cast<LightNode*>(&n);
		if (!ln) return false;
		ln->SetLight(std::make_shared<PointLight>());
		return true;
	};
	c.remove = [](SceneNode& n) {
		auto ln = dynamic_cast<LightNode*>(&n);
		if (!ln) return false;
		ln->SetLight(nullptr);
		return true;
	};
	PropertyDesc intensity;
	intensity.name = "intensity";
	intensity.type = PropType::Float;
	intensity.get = [](const SceneNode& n) {
		auto ln = dynamic_cast<const LightNode*>(&n);
		return PropValue(ln && ln->GetLight() ? static_cast<double>(ln->GetLight()->GetIntensity()) : 0.0);
	};
	intensity.set = [](SceneNode& n, const PropValue& v) {
		auto ln = dynamic_cast<LightNode*>(&n);
		if (!ln || !ln->GetLight()) return false;
		ln->GetLight()->SetIntensity(static_cast<float>(std::get<double>(v)));
		return true;
	};
	c.properties = { intensity };
	Registry::Get().Register(std::move(c));
}

} // namespace

NOX_TEST(EditorInspector, DiscreteEditPushesOneStepThatUndoesAndRedoes)
{
	RegisterBuiltinComponents();
	auto node = MakeNode("mover");
	NodeTable table;
	Recorder rec;
	std::string error;
	NOX_CHECK(nox::editor::SetPropertyUndoable(table, rec.push(), node, "Transform", "position",
		PropValue(glm::vec3(1.0f, 2.0f, 3.0f)), &error));
	NOX_REQUIRE(rec.steps.size() == 1);
	NOX_CHECK(rec.steps[0].label == "Transform.position");

	// Same value again: nothing to undo.
	NOX_CHECK(nox::editor::SetPropertyUndoable(table, rec.push(), node, "Transform", "position",
		PropValue(glm::vec3(1.0f, 2.0f, 3.0f)), &error));
	NOX_CHECK(rec.steps.size() == 1);

	rec.steps[0].undo();
	PropValue value;
	NOX_REQUIRE(GetProperty(*node, "Transform", "position", value));
	NOX_CHECK(std::get<glm::vec3>(value) == glm::vec3(0.0f));

	rec.steps[0].redo();
	NOX_REQUIRE(GetProperty(*node, "Transform", "position", value));
	NOX_CHECK(std::get<glm::vec3>(value) == glm::vec3(1.0f, 2.0f, 3.0f));
}

NOX_TEST(EditorInspector, RejectedValueLeavesNodeAndUndoUntouched)
{
	RegisterBuiltinComponents();
	auto node = MakeNode("rejected");
	NodeTable table;
	Recorder rec;
	std::string error;
	// Node.name rejects empty names.
	NOX_CHECK(!nox::editor::SetPropertyUndoable(table, rec.push(), node, "Node", "name", PropValue(std::string()), &error));
	NOX_CHECK(!error.empty());
	NOX_CHECK(rec.steps.empty());
	NOX_CHECK(node->GetName() == "rejected");
}

NOX_TEST(EditorInspector, ContinuousDragCoalescesIntoOneStep)
{
	RegisterBuiltinComponents();
	auto node = MakeNode("dragged");
	NodeTable table;
	Recorder rec;
	PropertyEdit edit;
	for (int i = 1; i <= 5; ++i) {
		NOX_CHECK(nox::editor::SetPropertyContinuous(edit, table, node, "Transform", "scale",
			PropValue(glm::vec3(static_cast<float>(i))), nullptr));
	}
	NOX_CHECK(rec.steps.empty());   // live writes push nothing
	NOX_CHECK(nox::editor::EndPropertyEdit(edit, table, rec.push()));
	NOX_REQUIRE(rec.steps.size() == 1);
	NOX_CHECK(!edit.active);

	PropValue value;
	NOX_REQUIRE(GetProperty(*node, "Transform", "scale", value));
	NOX_CHECK(std::get<glm::vec3>(value) == glm::vec3(5.0f));
	rec.steps[0].undo();
	NOX_REQUIRE(GetProperty(*node, "Transform", "scale", value));
	NOX_CHECK(std::get<glm::vec3>(value) == glm::vec3(1.0f));   // the value latched before the first write
}

NOX_TEST(EditorInspector, PlayModeWritesWithoutUndo)
{
	RegisterBuiltinComponents();
	auto node = MakeNode("live");
	NodeTable table;
	std::string error;
	// A null push is play mode: the value is written, no entry is recorded.
	NOX_CHECK(nox::editor::SetPropertyUndoable(table, nullptr, node, "Transform", "position",
		PropValue(glm::vec3(4.0f, 0.0f, 0.0f)), &error));
	PropValue value;
	NOX_REQUIRE(GetProperty(*node, "Transform", "position", value));
	NOX_CHECK(std::get<glm::vec3>(value) == glm::vec3(4.0f, 0.0f, 0.0f));
}

NOX_TEST(EditorInspector, AddAndRemoveComponentRoundTripOnLightNode)
{
	RegisterBuiltinComponents();
	RegisterTestLightSlot();
	const ComponentDesc* slot = Registry::Get().Find("EditorTestLightSlot");
	NOX_REQUIRE(slot != nullptr);

	auto light = std::make_shared<LightNode>(nullptr);
	NodeTable table;
	Recorder rec;
	NOX_REQUIRE(!slot->has(*light));
	NOX_CHECK(nox::editor::AddComponentUndoable(table, rec.push(), light, *slot));
	NOX_CHECK(slot->has(*light));
	NOX_REQUIRE(rec.steps.size() == 1);
	NOX_CHECK(rec.steps[0].label == "Add EditorTestLightSlot");

	rec.steps[0].undo();
	NOX_CHECK(!slot->has(*light));
	rec.steps[0].redo();
	NOX_CHECK(slot->has(*light));

	// Give the light a value, then remove it: undo must restore the component with that value.
	std::string error;
	NOX_REQUIRE(SetProperty(*light, "EditorTestLightSlot", "intensity", PropValue(7.0), &error));
	NOX_CHECK(nox::editor::RemoveComponentUndoable(table, rec.push(), light, *slot));
	NOX_CHECK(!slot->has(*light));
	NOX_REQUIRE(rec.steps.size() == 2);
	NOX_CHECK(rec.steps[1].label == "Remove EditorTestLightSlot");

	rec.steps[1].undo();
	NOX_REQUIRE(slot->has(*light));
	PropValue intensity;
	NOX_REQUIRE(GetProperty(*light, "EditorTestLightSlot", "intensity", intensity));
	NOX_CHECK(std::get<double>(intensity) == 7.0);

	rec.steps[1].redo();
	NOX_CHECK(!slot->has(*light));
}

NOX_TEST(EditorInspector, DeleteAndUndoKeepsHandlesOnRecreatedNodes)
{
	RegisterBuiltinComponents();
	auto root = MakeNode("root");
	auto parent = MakeNode("parent");
	auto child = MakeNode("child");
	root->AddChild(parent);
	parent->AddChild(child);

	NodeTable table;
	NodeOps ops = FakeOps();
	Recorder rec;
	const auto handle = table.HandleOf(parent);
	NOX_CHECK(nox::editor::DeleteNodeUndoable(table, ops, rec.push(), parent));
	NOX_CHECK(root->children.empty());
	NOX_REQUIRE(rec.steps.size() == 1);

	rec.steps[0].undo();
	NOX_REQUIRE(root->children.size() == 1);
	auto restored = root->children[0];
	NOX_CHECK(restored != parent);
	// The handle taken before the delete now resolves to the re-created node.
	NOX_CHECK(table.Resolve(handle) == restored);
	NOX_CHECK(restored->GetName() == "parent");

	rec.steps[0].redo();
	NOX_CHECK(root->children.empty());
	rec.steps[0].undo();
	NOX_CHECK(table.Resolve(handle) == root->children[0]);
}

NOX_TEST(EditorInspector, CreateNodeUndoRedoAndReparent)
{
	RegisterBuiltinComponents();
	auto root = MakeNode("root");
	auto a = MakeNode("a");
	auto b = MakeNode("b");
	root->AddChild(a);
	root->AddChild(b);

	NodeTable table;
	NodeOps ops = FakeOps();
	Recorder rec;
	nlohmann::json j = { {"name", "made"} };
	auto made = nox::editor::CreateNodeUndoable(table, ops, rec.push(), "Add made", j, root);
	NOX_REQUIRE(made != nullptr);
	NOX_CHECK(root->children.size() == 3);
	rec.steps[0].undo();
	NOX_CHECK(root->children.size() == 2);
	rec.steps[0].redo();
	NOX_REQUIRE(root->children.size() == 3);
	NOX_CHECK(root->children.back()->GetName() == "made");

	// Move b under a, then undo the move.
	NOX_CHECK(nox::editor::ReparentNodeUndoable(table, ops, rec.push(), b, a, -1));
	NOX_CHECK(b->parentNode.lock() == a);
	NOX_CHECK(a->children.size() == 1);
	rec.steps.back().undo();
	NOX_CHECK(b->parentNode.lock() == root);
	NOX_CHECK(a->children.empty());
}
