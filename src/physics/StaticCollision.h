#pragma once
// Static collision: a "StaticCollision" component on a node makes the nodes under it collidable without authoring a
// collider on each one. For every node in the subtree that has a model (and no collider of its own), the system keeps
// one static mesh body placed at that node's world transform, with the model's glTF file as its geometry. Geometry is
// loaded once per file and shared (MeshCollider's cache).
//
// The generated bodies belong to the system, not to the nodes: they are not a node's RigidBody, so they are not saved
// into the scene (the component is). They follow the scene graph: the system resynchronises when the component or the
// scene changes (MarkDirty) and at least every kResyncSeconds, and PreStepSync moves their transforms each frame.
//
// Components: the "StaticCollision" descriptor (src/reflect/components/PhysicsComponents.cpp) adds and removes entries
// here; the state lives in this system, keyed by node, so it is owned by the PhysicsEngine.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

class PhysicsEngine;
class RigidBody;
class Scene;
class SceneNode;

struct StaticCollisionSettings {
	bool includeChildren = true;          // false: only the node's own model
	uint32_t layer = 1;                   // collision layer of the generated bodies
	uint32_t mask = 0xFFFFFFFFu;          // collision mask of the generated bodies
};

class StaticCollisionSystem {
public:
	// Returns the asset path of a model's file (empty when unknown). Set by the engine owner.
	using ModelPathFn = std::function<std::string(const Scene&)>;

	// Adds the component to the node with default settings; false when node is null. Adding twice keeps the settings.
	bool Add(const std::shared_ptr<SceneNode>& node);
	// Removes the component and the bodies it generated.
	void Remove(PhysicsEngine& physics, const SceneNode* node);
	bool Has(const SceneNode* node) const;
	StaticCollisionSettings* Find(const SceneNode* node);
	const StaticCollisionSettings* Find(const SceneNode* node) const;
	size_t Count() const { return m_entries.size(); }
	// Bodies the system currently holds (all components together).
	size_t GeneratedBodyCount() const;

	// Asks for a resynchronisation at the next Update (component edits, scene load, node moves into or out of a subtree).
	void MarkDirty() { m_dirty = true; }

	// Creates, updates and removes generated bodies so they match the components and the scene graph. Runs at most once
	// per kResyncSeconds unless marked dirty. Entries whose node is gone drop their bodies.
	void Update(PhysicsEngine& physics, const ModelPathFn& modelPath, float dt);

	// Removes every generated body and forgets the components (scene unload, shutdown).
	void Clear(PhysicsEngine& physics);

	static constexpr float kResyncSeconds = 0.25f;

private:
	struct GeneratedBody {
		std::shared_ptr<RigidBody> body;
		std::string key;   // mesh file and baked scale the body was built from
	};
	struct Entry {
		std::weak_ptr<SceneNode> node;
		StaticCollisionSettings settings;
		std::map<const SceneNode*, GeneratedBody> bodies;
	};

	void Sync(PhysicsEngine& physics, Entry& entry, const ModelPathFn& modelPath);
	static void Release(PhysicsEngine& physics, GeneratedBody& generated);

	std::map<const SceneNode*, Entry> m_entries;
	bool m_dirty = true;
	float m_sinceSync = 0.0f;
};
