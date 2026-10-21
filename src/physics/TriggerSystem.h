#pragma once
// Trigger volumes: a sphere or box attached to a scene node that never collides. Once per physics frame
// PhysicsEngine::Update asks the system which bodies and character controllers overlap each enabled trigger and
// reports enter and exit events. A character is tested as its capsule (three sample spheres). Lua receives them as
// trigger_enter / trigger_exit (src/scripting/LuaPhysics.cpp).
//
// Components: the "Trigger" descriptor (src/reflect/components/PhysicsComponents.cpp) adds and removes entries
// here; the state lives in this system, keyed by node, so it is owned by the PhysicsEngine.

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "../ComponentTypes.h"

class PhysicsEngine;
class SceneNode;

struct TriggerSettings {
	enum class Shape { SPHERE = 0, BOX = 1 };
	Shape shape = Shape::SPHERE;
	float radius = 1.0f;                           // sphere radius (world units)
	glm::vec3 halfExtents{ 0.5f };                 // box half extents (world units, before node rotation)
	uint32_t layer = 1;                            // this trigger's layer: bodies whose mask has it are reported
	uint32_t mask = 0xFFFFFFFFu;                   // bodies whose layer intersects this mask are reported
	bool enabled = true;
};

struct TriggerEvent {
	bool enter = true;               // true: entered, false: left
	EntityID trigger = INVALID_ENTITY;   // entity of the trigger node
	EntityID other = INVALID_ENTITY;     // entity of the body's node (INVALID_ENTITY when it has none)
};

class TriggerSystem {
public:
	static constexpr uint64_t kCharacterKeyBit = 1ull << 63;

	// Adds a trigger to the node with default settings; false when node is null. Adding twice keeps the
	// existing settings.
	bool Add(const std::shared_ptr<SceneNode>& node);
	// Removes the trigger; every body still inside it gets an exit event.
	void Remove(const SceneNode* node);
	bool Has(const SceneNode* node) const;
	TriggerSettings* Find(const SceneNode* node);
	const TriggerSettings* Find(const SceneNode* node) const;
	size_t Count() const { return m_entries.size(); }
	// Calls f(const SceneNode&, const TriggerSettings&) for every trigger whose node is still alive.
	template <typename F> void ForEach(F&& f) const {
		for (const auto& item : m_entries) {
			if (auto node = item.second.node.lock()) f(static_cast<const SceneNode&>(*node), item.second.settings);
		}
	}

	// Recomputes the overlaps of every trigger against the bodies of `physics`. Entries whose node is gone
	// are dropped (with exit events for their overlaps).
	void Update(const PhysicsEngine& physics);

	// Events since the last call, in trigger order then body order.
	std::vector<TriggerEvent> TakeEvents();

	// Drops every trigger without events (scene unload, shutdown).
	void Clear();

private:
	struct Entry {
		std::weak_ptr<SceneNode> node;
		TriggerSettings settings;
		// Overlapping bodies (key: body id) and characters (key: kCharacterKeyBit | node address), each with the entity
		// of its node while inside.
		std::map<uint64_t, EntityID> overlaps;
	};

	void ExitAll(Entry& entry, EntityID triggerEntity);

	std::map<const SceneNode*, Entry> m_entries;
	std::vector<TriggerEvent> m_events;
};
