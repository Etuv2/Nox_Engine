#include "CharacterSystem.h"

#include "ShapeOverlap.h"
#include "../PhysicsEngine.h"
#include "../RigidBody.h"
#include "../CollisionWorld.h"
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
	Mover(PhysicsEngine& physics, const std::shared_ptr<SceneNode>& self, const CharacterSettings& settings,
		std::vector<ControllerHit>& hits)
		: m_physics(physics)
		, m_self(self)
		, m_settings(settings)
		, m_hits(hits)
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
		std::shared_ptr<RigidBody> floorBody;   // body that gave that floor contact (null for level geometry)
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
			std::shared_ptr<RigidBody> floorBody;
			if (Depenetrate(position, floor, floorBody)) result.collided = true;
			result.onFloor = floor;
			result.floorBody = floor ? floorBody : nullptr;
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

	// Pushes the feet out of every body and world triangle the capsule overlaps, up to kDepenetrationPasses passes.
	// Returns true when anything was touched; floorContact is set when a touched surface is walkable.
	// A surface steeper than the slope limit is a wall: it pushes horizontally only, so the capsule cannot climb it.
	bool Depenetrate(glm::vec3& feet, bool& floorContact, std::shared_ptr<RigidBody>& floorBody) {
		bool touched = false;
		// Records the contact for the controller's hit list once per body (or level geometry) per move.
		auto report = [&](const std::shared_ptr<RigidBody>& body, const glm::vec3& normal, const glm::vec3& point) {
			for (const ControllerHit& h : m_hits) {
				if (h.body == body && (body || glm::distance(h.point, point) < 1e-3f)) return;
			}
			ControllerHit hit;
			hit.controller = m_self->GetEntityID();
			hit.body = body;
			hit.normal = normal;
			hit.point = point;
			m_hits.push_back(hit);
		};
		auto apply = [&](const glm::vec3& push, float penetration, const std::shared_ptr<RigidBody>& who) -> bool {
			if (push.y >= m_floorCos) {
				floorContact = true;
				floorBody = who;   // the last floor contact decides which body the character stands on
			}
			if (penetration <= kMinPenetration) return false;
			glm::vec3 move = push * penetration;
			if (push.y > -m_floorCos && push.y < m_floorCos) {
				const glm::vec3 horizontal(push.x, 0.0f, push.z);
				const float length = glm::length(horizontal);
				if (length < 1e-5f) return false;
				move = horizontal / length * (penetration / length);
			}
			feet += move;
			return true;
		};
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
					report(body, push, samples[k] - push * m_settings.radius);
					if (apply(push, penetration, body)) corrected = true;
				}
				// Static world geometry (CollisionWorld): the deepest triangle contact of this sample.
				if (const CollisionWorld* world = m_physics.WorldCollision()) {
					SamplePoints(feet, m_settings, samples);
					std::vector<CollisionWorld::SphereContact> contacts;
					world->SphereContacts(samples[k], m_settings.radius, kContactSkin, CollisionWorld::Any, contacts);
					if (!contacts.empty()) {
						const CollisionWorld::SphereContact* deepest = &contacts[0];
						for (const auto& c : contacts) {
							if (c.depth > deepest->depth) deepest = &c;
						}
						touched = true;
						report(nullptr, deepest->normal, deepest->point);
						if (apply(deepest->normal, deepest->depth, nullptr)) corrected = true;
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
	std::vector<ControllerHit>& m_hits;
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

	Mover mover(physics, self, s, m_hits);
	Mover::Result sweep = mover.Run(start, displacement);
	glm::vec3 finalFeet = sweep.feet;
	bool collided = sweep.collided;
	bool onFloor = sweep.onFloor;
	std::shared_ptr<RigidBody> floorBody = sweep.floorBody;

	// Step-up: a walk from the floor that is blocked (it gains less than half of what was asked) retries one step
	// higher, moves forward at least one radius (the rounded foot must clear the edge before it can drop), and drops
	// back down onto a ledge. A walk up a slope is not blocked, so it never takes this path.
	const glm::vec3 horizontal(displacement.x, 0.0f, displacement.z);
	const float horizontalLength = LengthXZ(horizontal);
	const float directGain = LengthXZ(finalFeet - start);
	if (sweep.collided && s.onFloor && s.stepOffset > 0.0f && horizontalLength > 1e-5f &&
		directGain < 0.5f * horizontalLength) {
		const glm::vec3 up(0.0f, s.stepOffset, 0.0f);
		const glm::vec3 reach = horizontal / horizontalLength * std::max(horizontalLength, s.radius);
		const Mover::Result raised = mover.Run(start, up);
		const Mover::Result forward = mover.Run(raised.feet, reach);
		const Mover::Result landed = mover.Run(forward.feet, -up);
		if (landed.onFloor && LengthXZ(landed.feet - start) > directGain + 1e-4f) {
			finalFeet = landed.feet;
			onFloor = true;
			floorBody = landed.floorBody;
		}
	}

	// Snap to ground: a move that left the floor (a step down, or a slope falling away) drops back onto a floor that
	// lies within step_offset below. A character that is jumping (velocity up) is not snapped.
	if (!onFloor && s.onFloor && s.velocity.y <= 0.0f && s.stepOffset > 0.0f) {
		const Mover::Result down = mover.Run(finalFeet, glm::vec3(0.0f, -s.stepOffset, 0.0f));
		if (down.onFloor) {
			finalFeet = down.feet;
			onFloor = true;
			floorBody = down.floorBody;
		}
	}

	const glm::vec3 local = glm::vec3(glm::inverse(parentWorld) * glm::vec4(finalFeet, 1.0f));
	self->SetPosition(local);
	s.onFloor = onFloor;
	s.floorBody = onFloor ? floorBody : std::weak_ptr<RigidBody>();
	return collided;
}

std::vector<ControllerHit> CharacterSystem::TakeHits() {
	std::vector<ControllerHit> hits;
	hits.swap(m_hits);
	return hits;
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
		// Carried by the platform under the feet: its motion over the last frame, applied before gravity.
		if (s.onFloor) {
			if (const std::shared_ptr<RigidBody> platform = s.floorBody.lock()) {
				const glm::vec3 carry = platform->getPlatformVelocity() * dt;
				if (glm::dot(carry, carry) > 0.0f) Move(physics, *node, carry);
			}
		}
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
