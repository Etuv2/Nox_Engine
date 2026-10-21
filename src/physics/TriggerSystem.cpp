#include "TriggerSystem.h"

#include "../PhysicsEngine.h"
#include "../RigidBody.h"
#include "../SceneNode.h"
#include "CharacterSystem.h"

#include <algorithm>

namespace {

constexpr size_t kMaxTriggerEvents = 8192;

// Rotation of a world matrix with its scale removed.
glm::quat RotationOf(const glm::mat4& world) {
	const glm::mat3 axes(glm::normalize(glm::vec3(world[0])), glm::normalize(glm::vec3(world[1])),
		glm::normalize(glm::vec3(world[2])));
	return glm::quat_cast(axes);
}

// True when a character (feet at the node's position, capsule standing on +Y) overlaps the trigger volume: any of its
// three sample spheres (feet, middle, head, as CharacterSystem samples it) is within the volume grown by the radius.
bool CharacterTouches(const TriggerSettings& trigger, const glm::mat4& triggerWorld, const SceneNode& character,
	const CharacterSettings& settings) {
	const glm::mat4 world = character.GetWorldPosition4x4();
	const glm::vec3 feet(world[3]);
	const float r = settings.radius;
	const float top = std::max(settings.height, 2.0f * r) - r;
	const glm::vec3 up(0.0f, 1.0f, 0.0f);
	const glm::vec3 samples[3] = { feet + up * r, feet + up * (0.5f * (r + top)), feet + up * top };
	const glm::vec3 center(triggerWorld[3]);
	const glm::mat3 axes(glm::normalize(glm::vec3(triggerWorld[0])), glm::normalize(glm::vec3(triggerWorld[1])),
		glm::normalize(glm::vec3(triggerWorld[2])));
	for (const glm::vec3& p : samples) {
		if (trigger.shape == TriggerSettings::Shape::SPHERE) {
			const float reach = trigger.radius + r;
			if (glm::dot(p - center, p - center) <= reach * reach) return true;
			continue;
		}
		const glm::vec3 local = glm::transpose(axes) * (p - center);
		const glm::vec3 diff = local - glm::clamp(local, -trigger.halfExtents, trigger.halfExtents);
		if (glm::dot(diff, diff) <= r * r) return true;
	}
	return false;
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

		std::map<uint64_t, EntityID> current;
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

			// Character controllers: their capsule, filtered the same way (the character's mask takes the trigger's layer).
			physics.Characters().ForEach([&](const SceneNode& character, const CharacterSettings& settings) {
				if (&character == node.get()) return;
				if (!(settings.mask & entry.settings.layer) || !(settings.layer & entry.settings.mask)) return;
				if (!CharacterTouches(entry.settings, world, character, settings)) return;
				const uint64_t key = kCharacterKeyBit | static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&character));
				current[key] = character.GetEntityID();
			});
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
