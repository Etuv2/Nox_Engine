#include "CharacterSystem.h"

#include "ShapeOverlap.h"
#include "../PhysicsEngine.h"
#include "../RigidBody.h"
#include "../SceneNode.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kDepenetrationPasses = 4;
constexpr float kMinPenetration = 1e-5f;
constexpr float kContactSkin = 0.01f;   // a surface this close still counts as touching (floor detection)

bool Finite(const glm::vec3& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

float LengthXZ(const glm::vec3& v) {
	return std::sqrt(v.x * v.x + v.z * v.z);
}

// Sphere centers of the capsule whose feet are at `feet`: bottom, middle and top.
void SamplePoints(const glm::vec3& feet, const CharacterSettings& s, glm::vec3 out[3]) {
	const glm::vec3 up(0.0f, 1.0f, 0.0f);
	const float r = s.radius;
	const float top = std::max(s.height, 2.0f * r) - r;
	out[0] = feet + up * r;
	out[1] = feet + up * (0.5f * (r + top));
	out[2] = feet + up * top;
}

// One capsule sweep against the physics world. Holds the probe sphere and the static planes for the move.
class Mover {
public:
	Mover(PhysicsEngine& physics, const std::shared_ptr<SceneNode>& self, const CharacterSettings& settings)
		: m_physics(physics)
		, m_self(self)
		, m_settings(settings)
		, m_probe(std::make_shared<RigidBody>()) {
		m_probe->setSphere(settings.radius);
		const float slope = std::clamp(settings.slopeLimitDegrees, 0.0f, 89.0f);
		m_floorCos = std::cos(glm::radians(slope));
		for (const auto& body : physics.GetBodies()) {
			if (body && body->getShapeType() == RigidBody::ShapeType::PLANE) m_planes.push_back(body);
		}
	}

	struct Result {
		glm::vec3 feet{ 0.0f };
		bool collided = false;   // any contact during the sweep
		bool onFloor = false;    // floor contact during the last substep
	};

	// Moves the capsule from `feet` by `displacement` in substeps no longer than half the radius.
	Result Run(const glm::vec3& feet, const glm::vec3& displacement) {
		Result result;
		glm::vec3 position = feet;
		const float maxStep = std::max(0.01f, m_settings.radius * 0.5f);
		const int steps = std::max(1, static_cast<int>(std::ceil(glm::length(displacement) / maxStep)));
		const glm::vec3 sub = displacement / static_cast<float>(steps);
		for (int i = 0; i < steps; ++i) {
			position += sub;
			bool floor = false;
			if (Depenetrate(position, floor)) result.collided = true;
			result.onFloor = floor;
		}
		result.feet = position;
		return result;
	}

private:
	// Bodies that may touch a sphere at `center`: broadphase hits (not planes) plus every plane.
	std::vector<std::shared_ptr<RigidBody>> Candidates(const glm::vec3& center) {
		const float r = m_settings.radius + 0.05f;
		std::vector<std::shared_ptr<RigidBody>> out;
		for (const auto& body : m_physics.QueryAABB(center - glm::vec3(r), center + glm::vec3(r))) {
			if (body && body->getShapeType() != RigidBody::ShapeType::PLANE) out.push_back(body);
		}
		out.insert(out.end(), m_planes.begin(), m_planes.end());
		return out;
	}

	// Pushes the feet out of every body the capsule overlaps, up to kDepenetrationPasses passes. Returns true when
	// anything was touched; floorContact is set when a touched surface is walkable.
	bool Depenetrate(glm::vec3& feet, bool& floorContact) {
		bool touched = false;
		for (int pass = 0; pass < kDepenetrationPasses; ++pass) {
			bool corrected = false;
			for (int k = 0; k < 3; ++k) {
				glm::vec3 samples[3];
				SamplePoints(feet, m_settings, samples);
				for (const auto& body : Candidates(samples[k])) {
					if (body->getAttachedNode() == m_self) continue;
					// Each test uses the sample as it is after the corrections made so far.
					SamplePoints(feet, m_settings, samples);
					m_probe->setPosition(samples[k]);
					glm::vec3 push(0.0f);
					float penetration = 0.0f;
					if (!ShapeOverlap::Test(*m_probe, *body, kContactSkin, push, penetration)) continue;
					touched = true;
					if (push.y >= m_floorCos) floorContact = true;
					if (penetration > kMinPenetration) {
						feet += push * penetration;
						corrected = true;
					}
				}
			}
			if (!corrected) break;
		}
		return touched;
	}

	PhysicsEngine& m_physics;
	const std::shared_ptr<SceneNode>& m_self;
	const CharacterSettings& m_settings;
	std::shared_ptr<RigidBody> m_probe;
	std::vector<std::shared_ptr<RigidBody>> m_planes;
	float m_floorCos = 0.0f;
};

} // namespace

bool CharacterSystem::Add(const std::shared_ptr<SceneNode>& node) {
	if (!node) return false;
	auto it = m_entries.find(node.get());
	if (it != m_entries.end()) {
		if (it->second.node.lock() == node) return true;
		m_entries.erase(it);   // stale entry at a reused address
	}
	Entry entry;
	entry.node = node;
	m_entries.emplace(node.get(), std::move(entry));
	return true;
}

void CharacterSystem::Remove(const SceneNode* node) {
	m_entries.erase(node);
}

bool CharacterSystem::Has(const SceneNode* node) const {
	return m_entries.count(node) > 0;
}

CharacterSettings* CharacterSystem::Find(const SceneNode* node) {
	auto it = m_entries.find(node);
	return it == m_entries.end() ? nullptr : &it->second.settings;
}

const CharacterSettings* CharacterSystem::Find(const SceneNode* node) const {
	auto it = m_entries.find(node);
	return it == m_entries.end() ? nullptr : &it->second.settings;
}

bool CharacterSystem::Move(PhysicsEngine& physics, SceneNode& node, const glm::vec3& displacement) {
	auto it = m_entries.find(&node);
	if (it == m_entries.end() || !Finite(displacement)) return false;
	const std::shared_ptr<SceneNode> self = it->second.node.lock();
	if (!self) return false;
	CharacterSettings& s = it->second.settings;

	glm::mat4 parentWorld(1.0f);
	if (const std::shared_ptr<SceneNode> parent = self->parentNode.lock()) {
		parentWorld = parent->GetWorldPosition4x4();
	}
	const glm::vec3 start(self->GetWorldPosition4x4()[3]);

	Mover mover(physics, self, s);
	Mover::Result sweep = mover.Run(start, displacement);
	glm::vec3 finalFeet = sweep.feet;
	bool collided = sweep.collided;
	bool onFloor = sweep.onFloor;

	// Step-up: a blocked walk from the floor retries one step higher and drops back down onto a ledge.
	const glm::vec3 horizontal(displacement.x, 0.0f, displacement.z);
	if (sweep.collided && s.onFloor && s.stepOffset > 0.0f && LengthXZ(horizontal) > 1e-5f) {
		const glm::vec3 up(0.0f, s.stepOffset, 0.0f);
		const Mover::Result raised = mover.Run(start, up);
		const Mover::Result forward = mover.Run(raised.feet, horizontal);
		const Mover::Result landed = mover.Run(forward.feet, -up);
		const float directGain = LengthXZ(finalFeet - start);
		if (landed.onFloor && LengthXZ(landed.feet - start) > directGain + 1e-4f) {
			finalFeet = landed.feet;
			onFloor = true;
		}
	}

	const glm::vec3 local = glm::vec3(glm::inverse(parentWorld) * glm::vec4(finalFeet, 1.0f));
	self->SetPosition(local);
	s.onFloor = onFloor;
	return collided;
}

void CharacterSystem::Update(PhysicsEngine& physics, float dt) {
	const float gravity = physics.GetGravity().y;
	for (auto it = m_entries.begin(); it != m_entries.end();) {
		const std::shared_ptr<SceneNode> node = it->second.node.lock();
		if (!node) {
			it = m_entries.erase(it);
			continue;
		}
		CharacterSettings& s = it->second.settings;
		if (s.gravity) {
			if (s.onFloor && s.velocity.y <= 0.0f) {
				s.velocity.y = 0.0f;
			}
			else {
				s.velocity.y += gravity * dt;
			}
			Move(physics, *node, glm::vec3(0.0f, s.velocity.y * dt, 0.0f));
			if (s.onFloor && s.velocity.y < 0.0f) s.velocity.y = 0.0f;
		}
		++it;
	}
}
