#pragma once
// Kinematic character controller: a vertical capsule on a scene node that is moved by move(displacement)
// and never simulated as a body. Collision is tested against the physics bodies of the PhysicsEngine (static,
// kinematic and dynamic shapes, plus planes). The capsule is sampled with three spheres (feet, middle, head);
// a move is split into substeps, and after each substep the capsule is pushed out of whatever it touches, so
// motion slides along walls and floors (Godot move_and_slide semantics, simplified). Step-up climbs ledges no
// higher than step_offset while on the floor.
//
// Components: the "CharacterController" descriptor (src/reflect/components/PhysicsComponents.cpp) adds and
// removes entries here; the state lives in this system, keyed by node, so it is owned by the PhysicsEngine.

#include <glm/glm.hpp>

#include <map>
#include <memory>

class PhysicsEngine;
class SceneNode;

struct CharacterSettings {
	float height = 1.8f;               // capsule height, feet to head (world units)
	float radius = 0.4f;               // capsule radius
	float stepOffset = 0.3f;           // highest ledge the character climbs while walking
	float slopeLimitDegrees = 45.0f;   // steepest floor (angle from up) that counts as floor
	bool gravity = true;               // engine applies gravity to velocity.y once per frame
	glm::vec3 velocity{ 0.0f };        // runtime: vertical velocity is integrated while gravity is on
	bool onFloor = false;              // runtime: the last move ended touching a floor
};

class CharacterSystem {
public:
	// Adds a character to the node (default settings); false when node is null. Adding twice keeps the settings.
	bool Add(const std::shared_ptr<SceneNode>& node);
	void Remove(const SceneNode* node);
	bool Has(const SceneNode* node) const;
	CharacterSettings* Find(const SceneNode* node);
	const CharacterSettings* Find(const SceneNode* node) const;
	size_t Count() const { return m_entries.size(); }
	// Calls f(const SceneNode&, const CharacterSettings&) for every character whose node is still alive.
	template <typename F> void ForEach(F&& f) const {
		for (const auto& item : m_entries) {
			if (auto node = item.second.node.lock()) f(static_cast<const SceneNode&>(*node), item.second.settings);
		}
	}

	// Moves the node's character by displacement, sliding along what it touches. Returns true when the
	// capsule touched anything during the move. Updates node position and settings.onFloor.
	bool Move(PhysicsEngine& physics, SceneNode& node, const glm::vec3& displacement);

	// Once per frame: integrates gravity into velocity.y and moves vertically by it, for characters with
	// gravity on. Horizontal motion is always driven explicitly with Move().
	void Update(PhysicsEngine& physics, float dt);

	void Clear() { m_entries.clear(); }

private:
	struct Entry {
		std::weak_ptr<SceneNode> node;
		CharacterSettings settings;
	};
	std::map<const SceneNode*, Entry> m_entries;
};
