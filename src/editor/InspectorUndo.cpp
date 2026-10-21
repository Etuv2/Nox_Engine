#include "InspectorUndo.h"

#include <algorithm>
#include "../SceneNode.h"
#include "reflect/ReflectServices.h"

namespace nox::editor {

using nox::reflect::ComponentDesc;
using nox::reflect::PropValue;
using nox::reflect::Registry;

// ---- NodeTable -------------------------------------------------------------------------------------

NodeHandle NodeTable::HandleOf(const std::shared_ptr<SceneNode>& node)
{
	if (!node) return nullptr;
	auto it = m_byNode.find(node.get());
	if (it != m_byNode.end() && it->second->node.lock() == node) return it->second;
	auto handle = std::make_shared<NodeSlot>();
	handle->node = node;
	m_byNode[node.get()] = handle;
	return handle;
}

std::shared_ptr<SceneNode> NodeTable::Resolve(const NodeHandle& handle) const
{
	return handle ? handle->node.lock() : nullptr;
}

void NodeTable::Rebind(const NodeHandle& handle, const std::shared_ptr<SceneNode>& node)
{
	if (!handle) return;
	if (auto old = handle->node.lock()) {
		auto it = m_byNode.find(old.get());
		if (it != m_byNode.end() && it->second == handle) m_byNode.erase(it);
	}
	handle->node = node;
	if (node) m_byNode[node.get()] = handle;
}

void NodeTable::Transfer(const std::shared_ptr<SceneNode>& from, const std::shared_ptr<SceneNode>& to)
{
	if (!from || !to) return;
	auto it = m_byNode.find(from.get());
	if (it != m_byNode.end() && it->second->node.lock() == from) {
		const NodeHandle handle = it->second;
		Rebind(handle, to);
	}
	const size_t count = std::min(from->children.size(), to->children.size());
	for (size_t i = 0; i < count; ++i) {
		if (from->children[i]->GetName() == to->children[i]->GetName()) Transfer(from->children[i], to->children[i]);
	}
}

void NodeTable::Clear()
{
	m_byNode.clear();
}

// ---- Properties ------------------------------------------------------------------------------------

namespace {

void NotifyUnlessPlaying()
{
	const auto& services = nox::reflect::Services();
	if (!services.isPlaying || !services.isPlaying()) nox::reflect::NotifySceneChanged();
}

std::string PropertyLabel(const std::string& component, const std::string& property)
{
	return component + "." + property;
}

// Writes a property through the registry and tells the scene what changed.
bool WriteProperty(const std::shared_ptr<SceneNode>& node, const std::string& component, const std::string& property,
	const PropValue& value, std::string* error)
{
	if (!nox::reflect::SetProperty(*node, component, property, value, error)) return false;
	// While playing, the edit only touches the running world: the authored scene stays clean.
	const auto& services = nox::reflect::Services();
	if (!services.isPlaying || !services.isPlaying()) nox::reflect::NotifySceneChanged();
	return true;
}

void PushPropertyStep(NodeTable& table, const UndoPush& push, const NodeHandle& handle, const std::string& component,
	const std::string& property, const PropValue& before, const PropValue& after)
{
	if (!push) return;
	NodeTable* t = &table;
	push(PropertyLabel(component, property),
		[t, handle, component, property, before] {
			if (auto n = t->Resolve(handle)) WriteProperty(n, component, property, before, nullptr);
		},
		[t, handle, component, property, after] {
			if (auto n = t->Resolve(handle)) WriteProperty(n, component, property, after, nullptr);
		});
}

} // namespace

bool SetPropertyUndoable(NodeTable& table, const UndoPush& push, const std::shared_ptr<SceneNode>& node,
	const std::string& component, const std::string& property, const PropValue& value, std::string* error)
{
	if (!node) return false;
	PropValue before;
	if (!nox::reflect::GetProperty(*node, component, property, before)) {
		if (error) *error = component + "." + property + ": not readable";
		return false;
	}
	if (!WriteProperty(node, component, property, value, error)) return false;
	PropValue after;
	nox::reflect::GetProperty(*node, component, property, after);
	if (!nox::reflect::ValuesEqual(before, after)) {
		PushPropertyStep(table, push, table.HandleOf(node), component, property, before, after);
	}
	return true;
}

bool SetPropertyContinuous(PropertyEdit& edit, NodeTable& table, const std::shared_ptr<SceneNode>& node,
	const std::string& component, const std::string& property, const PropValue& value, std::string* error)
{
	if (!node) return false;
	if (!edit.active) {
		if (!nox::reflect::GetProperty(*node, component, property, edit.before)) {
			if (error) *error = component + "." + property + ": not readable";
			return false;
		}
		edit.active = true;
		edit.node = table.HandleOf(node);
		edit.component = component;
		edit.property = property;
	}
	return WriteProperty(node, component, property, value, error);
}

bool EndPropertyEdit(PropertyEdit& edit, NodeTable& table, const UndoPush& push)
{
	if (!edit.active) return false;
	edit.active = false;
	auto node = table.Resolve(edit.node);
	if (!node) return false;
	PropValue after;
	if (!nox::reflect::GetProperty(*node, edit.component, edit.property, after)) return false;
	if (nox::reflect::ValuesEqual(edit.before, after)) return false;
	PushPropertyStep(table, push, edit.node, edit.component, edit.property, edit.before, after);
	return push != nullptr;
}

// ---- Components ------------------------------------------------------------------------------------

bool AddComponentUndoable(NodeTable& table, const UndoPush& push, const std::shared_ptr<SceneNode>& node,
	const ComponentDesc& desc)
{
	if (!node || !desc.add || (desc.has && desc.has(*node))) return false;
	if (!desc.add(*node)) return false;
	NotifyUnlessPlaying();
	if (push) {
		NodeTable* t = &table;
		NodeHandle handle = table.HandleOf(node);
		const std::string name = desc.name;
		auto addOn = [t, handle, name](bool add) {
			auto n = t->Resolve(handle);
			const ComponentDesc* d = Registry::Get().Find(name);
			if (!n || !d) return;
			if (add) {
				if (d->add && !(d->has && d->has(*n))) d->add(*n);
			}
			else if (d->remove && d->has && d->has(*n)) {
				d->remove(*n);
			}
			NotifyUnlessPlaying();
		};
		push("Add " + name, [addOn] { addOn(false); }, [addOn] { addOn(true); });
	}
	return true;
}

bool RemoveComponentUndoable(NodeTable& table, const UndoPush& push, const std::shared_ptr<SceneNode>& node,
	const ComponentDesc& desc)
{
	if (!node || !desc.remove || !desc.has || !desc.has(*node)) return false;
	// Snapshot the component's savable properties; undo restores them through DeserializeComponents.
	nlohmann::json snapshot = nox::reflect::SerializeComponents(*node, false);
	nlohmann::json component = snapshot.contains(desc.name) ? snapshot[desc.name] : nlohmann::json::object();
	if (!desc.remove(*node)) return false;
	NotifyUnlessPlaying();
	if (push) {
		NodeTable* t = &table;
		NodeHandle handle = table.HandleOf(node);
		const std::string name = desc.name;
		auto removeOn = [t, handle, name, component](bool remove) {
			auto n = t->Resolve(handle);
			const ComponentDesc* d = Registry::Get().Find(name);
			if (!n || !d) return;
			if (remove) {
				if (d->remove && d->has && d->has(*n)) d->remove(*n);
			}
			else {
				nlohmann::json components = nlohmann::json::object();
				components[name] = component;
				nox::reflect::DeserializeComponents(*n, components);
			}
			NotifyUnlessPlaying();
		};
		push("Remove " + name, [removeOn] { removeOn(false); }, [removeOn] { removeOn(true); });
	}
	return true;
}

// ---- Nodes -----------------------------------------------------------------------------------------

namespace {

int IndexIn(const std::shared_ptr<SceneNode>& parent, const std::shared_ptr<SceneNode>& child)
{
	if (!parent) return -1;
	for (size_t i = 0; i < parent->children.size(); ++i) {
		if (parent->children[i] == child) return static_cast<int>(i);
	}
	return -1;
}

} // namespace

bool DeleteNodeUndoable(NodeTable& table, const NodeOps& ops, const UndoPush& push, const std::shared_ptr<SceneNode>& node)
{
	if (!node || !ops.destroy || !ops.instantiate || !ops.serialize) return false;
	const std::shared_ptr<SceneNode> parent = node->parentNode.lock();
	if (!parent) return false;
	const int index = IndexIn(parent, node);
	const nlohmann::json snapshot = ops.serialize(node);
	const std::string label = "Delete " + node->GetName();
	const NodeHandle parentHandle = table.HandleOf(parent);
	// The holder keeps the last live node (and its subtree) alive so handles can be moved onto a re-creation.
	auto holder = std::make_shared<std::shared_ptr<SceneNode>>(node);
	if (!ops.destroy(node)) return false;
	NodeTable* t = &table;
	if (push) {
		push(label,
			[t, ops, holder, snapshot, parentHandle, index] {
				auto p = t->Resolve(parentHandle);
				auto restored = ops.instantiate(snapshot, p);
				if (!restored) return;
				t->Transfer(*holder, restored);
				*holder = restored;
				if (p && index >= 0) ops.reparent(restored, p, index, false);
			},
			[ops, holder] {
				if (*holder) ops.destroy(*holder);
			});
	}
	return true;
}

void RecordCreatedNode(NodeTable& table, const NodeOps& ops, const UndoPush& push, const std::string& label,
	const std::shared_ptr<SceneNode>& created)
{
	if (!created || !push || !ops.destroy || !ops.instantiate || !ops.serialize) return;
	const std::shared_ptr<SceneNode> parent = created->parentNode.lock();
	const NodeHandle parentHandle = table.HandleOf(parent);
	const nlohmann::json snapshot = ops.serialize(created);
	auto holder = std::make_shared<std::shared_ptr<SceneNode>>(created);
	NodeTable* t = &table;
	push(label,
		[ops, holder] {
			if (*holder) ops.destroy(*holder);
		},
		[t, ops, holder, snapshot, parentHandle] {
			auto restored = ops.instantiate(snapshot, t->Resolve(parentHandle));
			if (!restored) return;
			t->Transfer(*holder, restored);
			*holder = restored;
		});
}

std::shared_ptr<SceneNode> CreateNodeUndoable(NodeTable& table, const NodeOps& ops, const UndoPush& push,
	const std::string& label, const nlohmann::json& nodeJson, const std::shared_ptr<SceneNode>& parent)
{
	if (!ops.instantiate) return nullptr;
	auto node = ops.instantiate(nodeJson, parent);
	if (node) RecordCreatedNode(table, ops, push, label, node);
	return node;
}

bool ReparentNodeUndoable(NodeTable& table, const NodeOps& ops, const UndoPush& push, const std::shared_ptr<SceneNode>& node,
	const std::shared_ptr<SceneNode>& newParent, int index)
{
	if (!node || !newParent || !ops.reparent) return false;
	const std::shared_ptr<SceneNode> oldParent = node->parentNode.lock();
	if (!oldParent) return false;
	const int oldIndex = IndexIn(oldParent, node);
	if (!ops.reparent(node, newParent, index, true)) return false;
	if (push) {
		NodeTable* t = &table;
		const NodeHandle nodeHandle = table.HandleOf(node);
		const NodeHandle oldHandle = table.HandleOf(oldParent);
		const NodeHandle newHandle = table.HandleOf(newParent);
		push("Move " + node->GetName(),
			[t, ops, nodeHandle, oldHandle, oldIndex] {
				auto n = t->Resolve(nodeHandle);
				auto p = t->Resolve(oldHandle);
				if (n && p) ops.reparent(n, p, oldIndex, true);
			},
			[t, ops, nodeHandle, newHandle, index] {
				auto n = t->Resolve(nodeHandle);
				auto p = t->Resolve(newHandle);
				if (n && p) ops.reparent(n, p, index, true);
			});
	}
	return true;
}

std::shared_ptr<SceneNode> GroupIntoFolderUndoable(NodeTable& table, const NodeOps& ops, const UndoPush& push,
	const std::shared_ptr<SceneNode>& node, const nlohmann::json& folderJson)
{
	if (!node || !ops.instantiate || !ops.reparent || !ops.serialize || !ops.destroy) return nullptr;
	const std::shared_ptr<SceneNode> parent = node->parentNode.lock();
	if (!parent) return nullptr;
	const int index = IndexIn(parent, node);
	auto folder = ops.instantiate(folderJson, parent);   // appended after the siblings
	if (!folder) return nullptr;
	// Redo re-creates the folder from this empty snapshot and moves the node back in.
	const nlohmann::json snapshot = ops.serialize(folder);
	ops.reparent(folder, parent, index, true);           // take the node's slot
	ops.reparent(node, folder, -1, true);
	if (push) {
		NodeTable* t = &table;
		const NodeHandle nodeHandle = table.HandleOf(node);
		const NodeHandle parentHandle = table.HandleOf(parent);
		auto holder = std::make_shared<std::shared_ptr<SceneNode>>(folder);
		push("Group into Folder",
			[t, ops, holder, nodeHandle, parentHandle, index] {
				auto n = t->Resolve(nodeHandle);
				auto p = t->Resolve(parentHandle);
				if (n && p) ops.reparent(n, p, index, true);
				if (*holder) ops.destroy(*holder);
			},
			[t, ops, holder, nodeHandle, parentHandle, index, snapshot] {
				auto p = t->Resolve(parentHandle);
				auto n = t->Resolve(nodeHandle);
				auto restored = p ? ops.instantiate(snapshot, p) : nullptr;
				if (!restored) return;
				ops.reparent(restored, p, index, true);
				if (n) ops.reparent(n, restored, -1, true);
				*holder = restored;
			});
	}
	return folder;
}

bool UnpackPrefabUndoable(NodeTable& table, const UndoPush& push, const std::shared_ptr<SceneNode>& node)
{
	if (!node || node->GetPrefabPath().empty()) return false;
	const std::string before = node->GetPrefabPath();
	node->SetPrefabPath(std::string());
	NodeTable* t = &table;
	NodeHandle handle = table.HandleOf(node);
	if (push) {
		push("Unpack Prefab",
			[t, handle, before] { if (auto n = t->Resolve(handle)) n->SetPrefabPath(before); },
			[t, handle] { if (auto n = t->Resolve(handle)) n->SetPrefabPath(std::string()); });
	}
	return true;
}

// ---- Batches ---------------------------------------------------------------------------------------

UndoPush UndoBatch::Sink()
{
	auto steps = m_steps;
	return [steps](const std::string& label, std::function<void()> undo, std::function<void()> redo) {
		steps->push_back({ label, std::move(undo), std::move(redo) });
	};
}

void UndoBatch::Commit(const std::string& label, const UndoPush& push) const
{
	if (!push || m_steps->empty()) return;
	auto steps = m_steps;
	push(label,
		[steps] { for (auto it = steps->rbegin(); it != steps->rend(); ++it) if (it->undo) it->undo(); },
		[steps] { for (const Step& s : *steps) if (s.redo) s.redo(); });
}

} // namespace nox::editor
