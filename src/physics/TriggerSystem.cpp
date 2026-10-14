#include "TriggerSystem.h"

#include "../PhysicsEngine.h"
#include "../RigidBody.h"
#include "../SceneNode.h"

namespace {

constexpr size_t kMaxTriggerEvents = 8192;

// Rotation of a world matrix with its scale removed.
glm::quat RotationOf(const glm::mat4& world) {
	const glm::mat3 axes(glm::normalize(glm::vec3(world[0])), glm::normalize(glm::vec3(world[1])),
		glm::normalize(glm::vec3(world[2])));
	return glm::quat_cast(axes);
}

} // namespace

bool TriggerSystem::Add(const std::shared_ptr<SceneNode>& node) {
	if (!node) return false;
	auto it = m_entries.find(node.get());
	if (it != m_entries.end()) {
		if (it->second.node.lock() == node) return true;
		// Stale entry at a reused address: the old node is gone.
		m_entries.erase(it);
	}
	Entry entry;
	entry.node = node;
	m_entries.emplace(node.get(), std::move(entry));
	return true;
}

void TriggerSystem::Remove(const SceneNode* node) {
	auto it = m_entries.find(node);
	if (it == m_entries.end()) return;
	const std::shared_ptr<SceneNode> alive = it->second.node.lock();
	ExitAll(it->second, alive ? alive->GetEntityID() : INVALID_ENTITY);
	m_entries.erase(it);
}

bool TriggerSystem::Has(const SceneNode* node) const {
	return m_entries.count(node) > 0;
}

TriggerSettings* TriggerSystem::Find(const SceneNode* node) {
	auto it = m_entries.find(node);
	return it == m_entries.end() ? nullptr : &it->second.settings;
}

const TriggerSettings* TriggerSystem::Find(const SceneNode* node) const {
	auto it = m_entries.find(node);
	return it == m_entries.end() ? nullptr : &it->second.settings;
}

void TriggerSystem::Update(const PhysicsEngine& physics) {
	for (auto it = m_entries.begin(); it != m_entries.end();) {
		Entry& entry = it->second;
		const std::shared_ptr<SceneNode> node = entry.node.lock();
		if (!node) {
			ExitAll(entry, INVALID_ENTITY);
			it = m_entries.erase(it);
			continue;
		}
		const EntityID triggerEntity = node->GetEntityID();

		std::map<uint32_t, EntityID> current;
		if (entry.settings.enabled) {
			const glm::mat4 world = node->GetWorldPosition4x4();
			const glm::vec3 center(world[3]);
			const std::vector<PhysicsEngine::BodyID> ids =
				entry.settings.shape == TriggerSettings::Shape::SPHERE
					? physics.OverlapSphere(center, entry.settings.radius, entry.settings.mask)
					: physics.OverlapBox(center, entry.settings.halfExtents, RotationOf(world), entry.settings.mask);
			for (PhysicsEngine::BodyID id : ids) {
				const std::shared_ptr<RigidBody> body = physics.GetBody(id);
				if (!body) continue;
				// Filtering is symmetric with collision: the body's mask must include the trigger's layer.
				if (!(body->getCollisionMask() & entry.settings.layer)) continue;
				const std::shared_ptr<SceneNode> bodyNode = body->getAttachedNode();
				if (bodyNode == node) continue;   // a trigger never reports its own body
				current[id] = bodyNode ? bodyNode->GetEntityID() : INVALID_ENTITY;
			}
		}

		for (const auto& [id, other] : current) {
			if (entry.overlaps.count(id)) continue;
			if (m_events.size() < kMaxTriggerEvents) m_events.push_back(TriggerEvent{ true, triggerEntity, other });
		}
		for (const auto& [id, other] : entry.overlaps) {
			if (current.count(id)) continue;
			if (m_events.size() < kMaxTriggerEvents) m_events.push_back(TriggerEvent{ false, triggerEntity, other });
		}
		entry.overlaps = std::move(current);
		++it;
	}
}

std::vector<TriggerEvent> TriggerSystem::TakeEvents() {
	std::vector<TriggerEvent> events;
	events.swap(m_events);
	return events;
}

void TriggerSystem::Clear() {
	m_entries.clear();
	m_events.clear();
}

void TriggerSystem::ExitAll(Entry& entry, EntityID triggerEntity) {
	for (const auto& [id, other] : entry.overlaps) {
		(void)id;
		if (m_events.size() < kMaxTriggerEvents) m_events.push_back(TriggerEvent{ false, triggerEntity, other });
	}
	entry.overlaps.clear();
}
