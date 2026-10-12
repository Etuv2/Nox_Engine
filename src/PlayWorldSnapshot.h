#pragma once
// Play-world restoration (Stop). Captured at Play: every live entity's local transform and, for entities with a
// rigid body, the body's pose and velocities. Stop removes entities created during play (their bodies first),
// restores the captured transforms and body states, and reports what it did.
//
// Scene-owned entities (referenced by a SceneNode) are never destroyed here, even when they were created during
// play: the scene graph still refers to them. Script-spawned entities are not scene-owned and are removed.
#include "ComponentTypes.h"

#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <set>
#include <vector>

class SceneGraph;
class PhysicsEngine;

class PlayWorldSnapshot {
public:
	struct BodyState {
		glm::vec3 position{ 0.0f };
		glm::quat orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 linearVelocity{ 0.0f };
		glm::vec3 angularVelocity{ 0.0f };
	};

	struct EntityState {
		EntityHandle handle;
		glm::mat4 localTransform{ 1.0f };
		bool hasBody = false;
		BodyState body;
	};

	struct Report {
		size_t destroyedEntities = 0;
		size_t removedBodies = 0;
		size_t restoredTransforms = 0;
		size_t restoredBodies = 0;
		size_t keptSceneEntities = 0;   // created during play but owned by the scene graph
	};

	// Records the authored state. Call before the game module begins play.
	void Capture(SceneGraph& scene);
	// Undoes play: call after the game module ended play and before the scene is reconfigured.
	Report Restore(SceneGraph& scene, PhysicsEngine* physics);

	bool Captured() const { return m_captured; }
	size_t EntityCount() const { return m_entities.size(); }

private:
	bool m_captured = false;
	std::vector<EntityState> m_entities;
	std::set<EntityHandle> m_known;
};
