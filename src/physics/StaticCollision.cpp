#include "StaticCollision.h"

#include "MeshCollider.h"
#include "../PhysicsEngine.h"
#include "../RigidBody.h"
#include "../Scene.h"
#include "../SceneNode.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

// Rotation of a world matrix with its scale removed.
glm::quat RotationOf(const glm::mat4& world) {
	const glm::mat3 axes(glm::normalize(glm::vec3(world[0])), glm::normalize(glm::vec3(world[1])),
		glm::normalize(glm::vec3(world[2])));
	return glm::quat_cast(axes);
}

// Nodes under root (root included when includeChildren is false: only root itself).
void CollectSubtree(const std::shared_ptr<SceneNode>& node, bool includeChildren, std::vector<std::shared_ptr<SceneNode>>& out) {
	out.push_back(node);
	if (!includeChildren) return;
	for (const auto& child : node->children) {
		if (child) CollectSubtree(child, true, out);
	}
}

} // namespace

bool StaticCollisionSystem::Add(const std::shared_ptr<SceneNode>& node) {
	if (!node) return false;
	auto it = m_entries.find(node.get());
	if (it != m_entries.end()) {
		if (it->second.node.lock() == node) return true;
		m_entries.erase(it);   // stale entry at a reused address
	}
	Entry entry;
	entry.node = node;
	m_entries.emplace(node.get(), std::move(entry));
	m_dirty = true;
	return true;
}

void StaticCollisionSystem::Remove(PhysicsEngine& physics, const SceneNode* node) {
	auto it = m_entries.find(node);
	if (it == m_entries.end()) return;
	for (auto& [owner, generated] : it->second.bodies) {
		(void)owner;
		Release(physics, generated);
	}
	m_entries.erase(it);
}

bool StaticCollisionSystem::Has(const SceneNode* node) const {
	return m_entries.count(node) > 0;
}

StaticCollisionSettings* StaticCollisionSystem::Find(const SceneNode* node) {
	auto it = m_entries.find(node);
	return it == m_entries.end() ? nullptr : &it->second.settings;
}

const StaticCollisionSettings* StaticCollisionSystem::Find(const SceneNode* node) const {
	auto it = m_entries.find(node);
	return it == m_entries.end() ? nullptr : &it->second.settings;
}

size_t StaticCollisionSystem::GeneratedBodyCount() const {
	size_t count = 0;
	for (const auto& [node, entry] : m_entries) {
		(void)node;
		count += entry.bodies.size();
	}
	return count;
}

void StaticCollisionSystem::Update(PhysicsEngine& physics, const ModelPathFn& modelPath, float dt) {
	m_sinceSync += dt;
	if (!m_dirty && m_sinceSync < kResyncSeconds) return;
	m_sinceSync = 0.0f;
	m_dirty = false;
	for (auto it = m_entries.begin(); it != m_entries.end();) {
		if (it->second.node.lock()) {
			Sync(physics, it->second, modelPath);
			++it;
			continue;
		}
		for (auto& [owner, generated] : it->second.bodies) {
			(void)owner;
			Release(physics, generated);
		}
		it = m_entries.erase(it);
	}
}

void StaticCollisionSystem::Clear(PhysicsEngine& physics) {
	for (auto& [node, entry] : m_entries) {
		(void)node;
		for (auto& [owner, generated] : entry.bodies) {
			(void)owner;
			Release(physics, generated);
		}
	}
	m_entries.clear();
	m_dirty = true;
}

void StaticCollisionSystem::Sync(PhysicsEngine& physics, Entry& entry, const ModelPathFn& modelPath) {
	const std::shared_ptr<SceneNode> root = entry.node.lock();
	if (!root || !modelPath) return;
	const StaticCollisionSettings& settings = entry.settings;

	// Which nodes should have a body now: nodes with a model file and no collider of their own.
	std::vector<std::shared_ptr<SceneNode>> subtree;
	CollectSubtree(root, settings.includeChildren, subtree);
	struct Wanted {
		std::shared_ptr<SceneNode> node;
		std::string path;
		glm::vec3 scale{ 1.0f };   // world scale of the node: baked into the geometry
		std::string key;           // path and scale: a change rebuilds the body
	};
	std::map<const SceneNode*, Wanted> wanted;
	for (const auto& node : subtree) {
		const std::shared_ptr<Scene> model = node->GetModel();
		if (!model || node->GetRigidBody()) continue;
		std::string path = modelPath(*model);
		if (path.empty()) continue;
		const glm::mat4 world = node->GetWorldPosition4x4();
		Wanted w;
		w.node = node;
		w.scale = glm::vec3(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
		w.key = path + "|" + std::to_string(w.scale.x) + "," + std::to_string(w.scale.y) + "," + std::to_string(w.scale.z);
		w.path = std::move(path);
		wanted.emplace(node.get(), std::move(w));
	}

	// Drop bodies whose node is no longer wanted or whose file changed.
	for (auto it = entry.bodies.begin(); it != entry.bodies.end();) {
		auto w = wanted.find(it->first);
		if (w == wanted.end() || w->second.key != it->second.key) {
			Release(physics, it->second);
			it = entry.bodies.erase(it);
		}
		else {
			++it;
		}
	}

	// Create the missing ones, and keep the existing ones' collision settings current.
	for (const auto& [owner, want] : wanted) {
		auto existing = entry.bodies.find(owner);
		if (existing != entry.bodies.end()) {
			RigidBody& rb = *existing->second.body;
			if (rb.getCollisionLayer() != settings.layer) rb.setCollisionLayer(settings.layer);
			if (rb.getCollisionMask() != settings.mask) rb.setCollisionMask(settings.mask);
			continue;
		}
		const std::shared_ptr<SceneNode>& node = want.node;
		auto mesh = MeshCollider::LoadShared(want.path, want.scale);
		if (!mesh) continue;   // unreadable file: no geometry, retried when the component is next synced
		glm::vec3 mn(0.0f), mx(0.0f);
		MeshCollider::LocalBounds(*mesh, mn, mx);

		const glm::mat4 world = node->GetWorldPosition4x4();
		auto rb = std::make_shared<RigidBody>();
		rb->AttachNode(node);
		rb->setPosition(glm::vec3(world[3]));
		rb->setOrientation(RotationOf(world));
		rb->setFriction(physics.GetConfig().defaultFriction);
		rb->setRestitution(physics.GetConfig().defaultRestitution);
		rb->setCollisionLayer(settings.layer);
		rb->setCollisionMask(settings.mask);
		rb->setMesh(mesh, mn, mx, want.path);
		rb->computeInertiaTensor();
		rb->computeAABB();
		// The body is not the node's RigidBody on purpose: the node keeps no collider entry, so nothing is saved.
		physics.AddBody(rb);
		entry.bodies.emplace(owner, GeneratedBody{ rb, want.key });
	}
}

void StaticCollisionSystem::Release(PhysicsEngine& physics, GeneratedBody& generated) {
	if (!generated.body) return;
	physics.RemoveBody(generated.body);
	generated.body->AttachNode(nullptr);
	generated.body.reset();
}
