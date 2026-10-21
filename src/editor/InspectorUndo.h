#pragma once
// Undoable editing operations for the editor inspector and hierarchy. No ImGui here: the editor
// workspace supplies the undo stack (UndoPush) and the node operations (NodeOps), and tests drive the
// same code with plain SceneNodes.
//
// Node identity: undoing a delete or redoing a create re-instantiates the node, which gives a new
// SceneNode pointer. Undo closures therefore never capture a raw node; they hold a NodeHandle that the
// NodeTable re-points at the re-created node (Transfer also moves handles of the children).

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "reflect/Reflection.h"

class SceneNode;

namespace nox::editor {

// Pushes one undo step. Null means "no undo" (play mode, where edits are not authored).
using UndoPush = std::function<void(const std::string& label, std::function<void()> undo, std::function<void()> redo)>;

struct NodeSlot {
	std::weak_ptr<SceneNode> node;
};
using NodeHandle = std::shared_ptr<NodeSlot>;

class NodeTable {
public:
	// Handle for a live node (created on first use).
	NodeHandle HandleOf(const std::shared_ptr<SceneNode>& node);
	// Current node for a handle; null when it has been deleted and not re-created.
	std::shared_ptr<SceneNode> Resolve(const NodeHandle& handle) const;
	// Points a handle at another node.
	void Rebind(const NodeHandle& handle, const std::shared_ptr<SceneNode>& node);
	// Moves the handles of from and its children (matched by index and name) onto to and its children.
	void Transfer(const std::shared_ptr<SceneNode>& from, const std::shared_ptr<SceneNode>& to);
	void Clear();

private:
	std::unordered_map<const SceneNode*, NodeHandle> m_byNode;
};

// Core operations the node undo steps need. The editor fills these from Core; tests use fakes.
struct NodeOps {
	std::function<nlohmann::json(const std::shared_ptr<SceneNode>&)> serialize;
	std::function<std::shared_ptr<SceneNode>(const nlohmann::json&, const std::shared_ptr<SceneNode>& parent)> instantiate;
	std::function<bool(const std::shared_ptr<SceneNode>&)> destroy;
	// (node, newParent, index, keepWorld)
	std::function<bool(const std::shared_ptr<SceneNode>&, const std::shared_ptr<SceneNode>&, int, bool)> reparent;
};

// ---- Properties ------------------------------------------------------------------------------------

// A continuous edit (drag, slider, colour picker, text) that becomes one undo step when it ends.
struct PropertyEdit {
	bool active = false;
	NodeHandle node;
	std::string component;
	std::string property;
	nox::reflect::PropValue before;
};

// Discrete edit: reads the old value, writes the new one and pushes one step when it changed.
bool SetPropertyUndoable(NodeTable& table, const UndoPush& push, const std::shared_ptr<SceneNode>& node,
	const std::string& component, const std::string& property, const nox::reflect::PropValue& value,
	std::string* error = nullptr);

// Continuous edit step: the first call latches the value before the edit; every call writes the value
// live (no undo entry yet).
bool SetPropertyContinuous(PropertyEdit& edit, NodeTable& table, const std::shared_ptr<SceneNode>& node,
	const std::string& component, const std::string& property, const nox::reflect::PropValue& value,
	std::string* error = nullptr);

// Ends a continuous edit and pushes one step when the value differs from the latched one.
// Returns true when a step was pushed.
bool EndPropertyEdit(PropertyEdit& edit, NodeTable& table, const UndoPush& push);

// Multi-edit: the value a target node gets from an edit the primary node made (before -> after). Vector components
// the edit changed take the new component value and the others keep the target's own; a scalar that changed takes
// the new value. A target of another type gets the new value.
nox::reflect::PropValue MergeEditedValue(const nox::reflect::PropValue& target, const nox::reflect::PropValue& before,
	const nox::reflect::PropValue& after);

// ---- Components ------------------------------------------------------------------------------------

// Adds a component (desc.add) and pushes a step whose undo removes it again.
bool AddComponentUndoable(NodeTable& table, const UndoPush& push, const std::shared_ptr<SceneNode>& node,
	const nox::reflect::ComponentDesc& desc);
// Removes a component (desc.remove). Undo restores it from a JSON snapshot taken before the removal.
bool RemoveComponentUndoable(NodeTable& table, const UndoPush& push, const std::shared_ptr<SceneNode>& node,
	const nox::reflect::ComponentDesc& desc);

// ---- Nodes -----------------------------------------------------------------------------------------

// Deletes a node (with its subtree). Undo re-creates it from JSON under its old parent and index.
bool DeleteNodeUndoable(NodeTable& table, const NodeOps& ops, const UndoPush& push, const std::shared_ptr<SceneNode>& node);

// Registers a node that was just created (or duplicated) by the editor. Undo deletes it; redo re-creates
// it from the JSON captured now under the same parent.
void RecordCreatedNode(NodeTable& table, const NodeOps& ops, const UndoPush& push, const std::string& label,
	const std::shared_ptr<SceneNode>& created);

// Instantiates a node from JSON under parent (null = root) and records it as created.
std::shared_ptr<SceneNode> CreateNodeUndoable(NodeTable& table, const NodeOps& ops, const UndoPush& push,
	const std::string& label, const nlohmann::json& nodeJson, const std::shared_ptr<SceneNode>& parent);

// Moves a node under newParent at index (negative = last), keeping its world transform.
bool ReparentNodeUndoable(NodeTable& table, const NodeOps& ops, const UndoPush& push, const std::shared_ptr<SceneNode>& node,
	const std::shared_ptr<SceneNode>& newParent, int index);

// Groups a node into a new folder: the folder takes the node's slot under the same parent and the node moves
// inside it (world transforms are kept; folders have identity transforms). One undo step. Null on failure.
std::shared_ptr<SceneNode> GroupIntoFolderUndoable(NodeTable& table, const NodeOps& ops, const UndoPush& push,
	const std::shared_ptr<SceneNode>& node, const nlohmann::json& folderJson);

// Unpacks a prefab instance: the node and its subtree stay as they are, but the node stops being an instance
// (its prefab link is cleared). Undo links it again. False when the node is not an instance.
bool UnpackPrefabUndoable(NodeTable& table, const UndoPush& push, const std::shared_ptr<SceneNode>& node);

// ---- Batches ---------------------------------------------------------------------------------------

// Collects the steps several operations push (Sink) and commits them as one undo entry: undo runs the
// collected undos last-to-first, redo runs the redos first-to-last. Nothing is pushed when no step was collected.
class UndoBatch {
public:
	UndoPush Sink();
	bool Empty() const { return m_steps->empty(); }
	size_t Size() const { return m_steps->size(); }
	void Commit(const std::string& label, const UndoPush& push) const;

private:
	struct Step { std::string label; std::function<void()> undo, redo; };
	std::shared_ptr<std::vector<Step>> m_steps = std::make_shared<std::vector<Step>>();
};

} // namespace nox::editor
