#include "PlayWorldSnapshot.h"

#include "ComponentManager.h"
#include "LightManager.h"
#include "LightNode.h"
#include "PhysicsEngine.h"
#include "RigidBody.h"
#include "SceneGraph.h"
#include "SceneLoader.h"
#include "SceneNode.h"
#include "TransformSystem.h"
#include "reflect/Reflection.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>

using nlohmann::json;

namespace {

// Serializable nodes are the ones SceneLoader writes: not generated, not imported from a model file.
bool IsSerializedNode(const SceneNode& node) {
	return !node.IsGenerated() && node.nodeIndex < 0;
}

// Records the node and, when it is serialized, its own keys and children. Descendants of a non-serialized
// node are only remembered as captured (they are rebuilt with their owner).
void CaptureTree(const std::shared_ptr<SceneNode>& node, int parentRecord, size_t index, SceneLoader& loader,
	std::vector<json>& ownJson, std::vector<std::shared_ptr<SceneNode>>& nodes, std::vector<int>& parents,
	std::vector<size_t>& indices, std::set<const SceneNode*>& captured) {
	captured.insert(node.get());
	const bool serialized = IsSerializedNode(*node);
	int record = -1;
	if (serialized) {
		record = static_cast<int>(nodes.size());
		nodes.push_back(node);
		parents.push_back(parentRecord);
		indices.push_back(index);
		ownJson.push_back(loader.SerializeNodeOwn(node));
	}
	for (size_t i = 0; i < node->children.size(); ++i) {
		const auto& child = node->children[i];
		if (!child) continue;
		if (serialized) {
			CaptureTree(child, record, i, loader, ownJson, nodes, parents, indices, captured);
		}
		else {
			std::function<void(const std::shared_ptr<SceneNode>&)> mark = [&](const std::shared_ptr<SceneNode>& n) {
				captured.insert(n.get());
				for (const auto& c : n->children) if (c) mark(c);
			};
			mark(child);
		}
	}
}

void MoveChildTo(const std::shared_ptr<SceneNode>& parent, const std::shared_ptr<SceneNode>& child, size_t index) {
	auto& siblings = parent->children;
	auto it = std::find(siblings.begin(), siblings.end(), child);
	if (it == siblings.end()) return;
	siblings.erase(it);
	siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(std::min(index, siblings.size())), child);
}

} // namespace

bool PlayWorldSnapshot::RemoveSubtree(SceneGraph& scene, const std::shared_ptr<SceneNode>& node, PhysicsEngine* physics,
	Report* report) {
	if (!node || node == scene.GetRoot()) return false;
	ComponentManager* cm = scene.GetComponentManager();
	LightManager* lm = scene.GetLightManager().get();

	std::function<void(const std::shared_ptr<SceneNode>&)> destroy = [&](const std::shared_ptr<SceneNode>& n) {
		for (const auto& c : n->children) {
			if (c) destroy(c);
		}
		if (report) ++report->removedNodes;
		if (auto ln = std::dynamic_pointer_cast<LightNode>(n)) {
			if (lm && ln->GetLight()) lm->UnregisterLight(ln->GetLight());
		}
		if (cm && n->GetEntityID() != INVALID_ENTITY) {
			const EntityID id = n->GetEntityID();
			if (const PhysicsComponent* physics_ = cm->GetPhysics(id)) {
				if (physics && physics_->bodyID != 0) {
					physics->RemoveBody(physics_->bodyID);
					if (report) ++report->removedBodies;
				}
			}
			cm->DestroyEntity(id);
			if (report) ++report->destroyedEntities;
		}
	};
	destroy(node);
	if (auto parent = node->parentNode.lock()) {
		auto& siblings = parent->children;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
	}
	return true;
}

void PlayWorldSnapshot::Capture(SceneGraph& scene, SceneLoader& loader) {
	m_entities.clear();
	m_known.clear();
	m_nodes.clear();
	m_capturedNodes.clear();

	// Scene nodes: records in pre-order, so a parent always precedes its children.
	std::vector<json> ownJson;
	std::vector<std::shared_ptr<SceneNode>> nodes;
	std::vector<int> parents;
	std::vector<size_t> indices;
	if (auto root = scene.GetRoot()) {
		m_capturedNodes.insert(root.get());
		for (size_t i = 0; i < root->children.size(); ++i) {
			if (root->children[i]) {
				CaptureTree(root->children[i], -1, i, loader, ownJson, nodes, parents, indices, m_capturedNodes);
			}
		}
	}
	m_nodes.resize(nodes.size());
	for (size_t r = 0; r < nodes.size(); ++r) {
		NodeRecord& rec = m_nodes[r];
		rec.node = nodes[r];
		rec.parentRecord = parents[r];
		rec.index = indices[r];
		rec.own = std::move(ownJson[r]);
		rec.prefabPath = nodes[r]->GetPrefabPath();
		if (rec.parentRecord >= 0) m_nodes[static_cast<size_t>(rec.parentRecord)].children.push_back(r);
	}

	ComponentManager* cm = scene.GetComponentManager();
	if (!cm) {
		m_captured = true;
		return;
	}
	std::vector<EntityID> ids;
	for (const auto& entry : cm->GetTransformPool()) ids.push_back(entry.entity);
	std::sort(ids.begin(), ids.end());
	for (EntityID id : ids) {
		const EntityHandle handle = cm->MakeHandle(id);
		if (!cm->IsHandleValid(handle)) continue;
		m_known.insert(handle);
		EntityState state;
		state.handle = handle;
		if (const TransformComponent* transform = cm->GetTransform(id)) state.localTransform = transform->localTransform;
		if (const PhysicsComponent* physics = cm->GetPhysics(id)) {
			if (physics->rigidBody) {
				state.hasBody = true;
				state.body.position = physics->rigidBody->getPosition();
				state.body.orientation = physics->rigidBody->getOrientation();
				state.body.linearVelocity = physics->rigidBody->getVelocity();
				state.body.angularVelocity = physics->rigidBody->getAngularVelocity();
			}
		}
		m_entities.push_back(state);
	}
	m_captured = true;
}

PlayWorldSnapshot::Report PlayWorldSnapshot::Restore(SceneGraph& scene, PhysicsEngine* physics, SceneLoader& loader,
	const std::string& sceneFilePath) {
	Report report;
	if (!m_captured) return report;
	ComponentManager* cm = scene.GetComponentManager();
	TransformSystem* ts = scene.GetTransformSystem();
	auto root = scene.GetRoot();
	if (!cm || !ts || !root) return report;

	// 1. Nodes created during play (not in the capture) leave the graph with their subtree.
	std::set<const SceneNode*> alive;
	std::set<EntityID> ownedEntities;
	std::vector<std::pair<std::shared_ptr<SceneNode>, std::shared_ptr<SceneNode>>> created;   // (parent, child)
	std::function<void(const std::shared_ptr<SceneNode>&)> walk = [&](const std::shared_ptr<SceneNode>& n) {
		alive.insert(n.get());
		if (n->GetEntityID() != INVALID_ENTITY) ownedEntities.insert(n->GetEntityID());
		for (const auto& c : n->children) {
			if (!c) continue;
			if (m_capturedNodes.count(c.get()) == 0) created.emplace_back(n, c);
			else walk(c);
		}
	};
	walk(root);
	for (const auto& [parent, top] : created) {
		RemoveSubtree(scene, top, physics, &report);
		// RemoveSubtree detaches through parentNode; children added without a parent link are detached here.
		auto& siblings = parent->children;
		siblings.erase(std::remove(siblings.begin(), siblings.end(), top), siblings.end());
	}

	// 2. Script-spawned entities (ECS only, no node) created during play.
	std::vector<EntityID> ids;
	for (const auto& entry : cm->GetTransformPool()) ids.push_back(entry.entity);
	std::sort(ids.begin(), ids.end());
	for (EntityID id : ids) {
		const EntityHandle handle = cm->MakeHandle(id);
		if (!cm->IsHandleValid(handle) || m_known.count(handle) || ownedEntities.count(id)) continue;
		if (const PhysicsComponent* physics_ = cm->GetPhysics(id)) {
			if (physics && physics_->bodyID != 0) {
				physics->RemoveBody(physics_->bodyID);
				++report.removedBodies;
			}
		}
		cm->DestroyEntity(id);
		++report.destroyedEntities;
	}

	// 3. Authored entities: transforms and body states back to the captured values.
	for (const EntityState& state : m_entities) {
		if (!cm->IsHandleValid(state.handle)) continue;   // destroyed during play; re-created below
		ts->SetLocalTransform(state.handle.id, state.localTransform);
		++report.restoredTransforms;
		if (!state.hasBody) continue;
		PhysicsComponent* physics_ = cm->GetPhysics(state.handle.id);
		if (!physics_ || !physics_->rigidBody) continue;
		RigidBody& body = *physics_->rigidBody;
		body.setPosition(state.body.position);
		body.setOrientation(state.body.orientation);
		body.setVelocity(state.body.linearVelocity);
		body.setAngularVelocity(state.body.angularVelocity);
		body.storePreviousState();   // no interpolation from the play-time pose
		body.computeAABB();
		++report.restoredBodies;
	}

	// 4. Authored nodes destroyed during play: re-created from the captured JSON. A destroyed record whose parent
	//    is also destroyed is covered by the parent's JSON and skipped.
	std::function<json(size_t)> buildJson = [&](size_t r) {
		json j = m_nodes[r].own;
		if (!m_nodes[r].children.empty()) {
			json children = json::array();
			for (size_t c : m_nodes[r].children) children.push_back(buildJson(c));
			j["children"] = children;
		}
		return j;
	};
	for (size_t r = 0; r < m_nodes.size(); ++r) {
		const NodeRecord& rec = m_nodes[r];
		const auto live = rec.node.lock();
		if (live && alive.count(live.get())) continue;
		std::shared_ptr<SceneNode> parent = root;
		if (rec.parentRecord >= 0) {
			parent = m_nodes[static_cast<size_t>(rec.parentRecord)].node.lock();
			if (!parent || alive.count(parent.get()) == 0) continue;
		}
		auto node = loader.InstantiateNode(scene, buildJson(r), parent, sceneFilePath);
		if (!node) {
			std::cerr << "[PlayWorld] Warning: could not re-create node '" << rec.own.value("name", std::string("node"))
				<< "' destroyed during play" << std::endl;
			continue;
		}
		if (!rec.prefabPath.empty()) node->SetPrefabPath(rec.prefabPath);
		MoveChildTo(parent, node, rec.index);
		++report.recreatedNodes;
	}

	// 5. Registry values of authored nodes that are still alive: write back only the properties that differ.
	for (const NodeRecord& rec : m_nodes) {
		const auto live = rec.node.lock();
		if (!live || alive.count(live.get()) == 0) continue;
		const json recorded = rec.own.contains("components") && rec.own["components"].is_object()
			? rec.own["components"] : json::object();
		if (recorded.empty()) continue;
		const json now = nox::reflect::SerializeComponents(*live);
		json patch = json::object();
		size_t values = 0;
		for (auto comp = recorded.begin(); comp != recorded.end(); ++comp) {
			if (!comp.value().is_object()) continue;
			if (!now.contains(comp.key()) || !now[comp.key()].is_object()) {
				patch[comp.key()] = comp.value();
				values += comp.value().size();
				continue;
			}
			json changed = json::object();
			for (auto prop = comp.value().begin(); prop != comp.value().end(); ++prop) {
				if (!now[comp.key()].contains(prop.key()) || now[comp.key()][prop.key()] != prop.value()) {
					changed[prop.key()] = prop.value();
				}
			}
			if (!changed.empty()) {
				values += changed.size();
				patch[comp.key()] = changed;
			}
		}
		if (patch.empty()) continue;
		std::vector<std::string> warnings;
		nox::reflect::DeserializeComponents(*live, patch, &warnings);
		for (const std::string& warning : warnings) {
			std::cerr << "[PlayWorld] Warning: restoring '" << live->GetName() << "': " << warning << std::endl;
		}
		report.restoredComponents += values;
	}
	return report;
}
