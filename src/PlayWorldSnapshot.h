#pragma once
// Play-world restoration (Stop). Captured at Play, for the whole authored scene:
//  - every scene node: its own serialized keys (SceneLoader::SerializeNodeOwn, including the registry
//    "components" block), its position among its siblings and its prefab link;
//  - every live entity: local transform and, for entities with a rigid body, the body's pose and velocities.
// Stop restores it in this order:
//  1. nodes created during play are removed (subtree, lights, bodies, ECS entities);
//  2. script-spawned ECS entities without a node are removed;
//  3. authored transforms and body states go back to the captured values;
//  4. authored nodes destroyed during play are re-created from their captured JSON under their captured parent
//     and index (the prefab link is kept on the re-created root);
//  5. for every authored node still alive, the registry property values that differ from the capture are
//     written back (DeserializeComponents with the diff only).
// Limits: a component added during play is not removed at Stop; a component removed during play is restored
// with its captured properties (its own record); nodes inside a re-created prefab instance keep their content
// but not their own prefab links (the save still matches the prefab).
#include "ComponentTypes.h"

#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "json.hpp"

class SceneGraph;
class SceneLoader;
class SceneNode;
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
		size_t destroyedEntities = 0;   // ECS entities removed (script-spawned and nodes created during play)
		size_t removedBodies = 0;
		size_t removedNodes = 0;        // scene nodes created during play, removed at Stop
		size_t recreatedNodes = 0;      // authored nodes destroyed during play, re-created at Stop
		size_t restoredTransforms = 0;
		size_t restoredBodies = 0;
		size_t restoredComponents = 0;  // registry property values written back at Stop
	};

	// Records the authored state. Call before the game module begins play.
	void Capture(SceneGraph& scene, SceneLoader& loader);
	// Undoes play: call after the game module ended play and before the scene is reconfigured.
	// sceneFilePath is the scene the graph was loaded from (relative asset resolution for re-created nodes).
	Report Restore(SceneGraph& scene, PhysicsEngine* physics, SceneLoader& loader, const std::string& sceneFilePath);

	// Removes a node and its subtree from the live graph: detaches it, unregisters lights, removes rigid bodies and
	// destroys the ECS entities. Used for nodes created during play and for ctx:destroy of authored nodes.
	static bool RemoveSubtree(SceneGraph& scene, const std::shared_ptr<SceneNode>& node, PhysicsEngine* physics,
		Report* report = nullptr);

	bool Captured() const { return m_captured; }
	size_t EntityCount() const { return m_entities.size(); }
	size_t NodeCount() const { return m_nodes.size(); }

private:
	struct NodeRecord {
		std::weak_ptr<SceneNode> node;
		int parentRecord = -1;          // -1: child of the scene root
		size_t index = 0;               // position among the parent's children at capture
		nlohmann::json own;             // SerializeNodeOwn at capture (without children)
		std::vector<size_t> children;   // records of the serialized children, in order
		std::string prefabPath;
	};

	bool m_captured = false;
	std::vector<EntityState> m_entities;
	std::set<EntityHandle> m_known;
	std::vector<NodeRecord> m_nodes;
	std::set<const SceneNode*> m_capturedNodes;   // every node in the tree at capture (serialized or not)
};
