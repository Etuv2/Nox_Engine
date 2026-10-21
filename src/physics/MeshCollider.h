#pragma once
// Mesh colliders: a static triangle mesh (CollisionWorld geometry in the mesh's local space) placed by its body's
// transform. Meshes are loaded once per asset path and shared by every body that uses that path.
// Sphere contacts are exact per triangle (closest point on the triangle); the physics engine builds contact
// manifolds from them and the character controller and sweeps use them directly.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <vector>

#include "../CollisionWorld.h"

class RigidBody;

namespace MeshCollider {

	// Loads (or returns the cached) mesh for a glTF/GLB path. Nodes named NOCOLLIDE are skipped. Null on failure.
	// A scale other than one is baked into the triangles (body transforms are rigid), cached per path and scale.
	std::shared_ptr<const CollisionWorld> LoadShared(const std::string& path, const glm::vec3& scale = glm::vec3(1.0f));
	// Forgets the cache (tests, scene unload). Bodies keep the meshes they hold.
	void ClearCache();
	// Forgets the cached meshes of one path (every scale), so the next LoadShared reads the file again (hot reload).
	void Invalidate(const std::string& path);

	// Local bounds of a loaded mesh (false when empty).
	bool LocalBounds(const CollisionWorld& mesh, glm::vec3& mn, glm::vec3& mx);

	// All sphere/triangle contacts between the sphere (center, radius, world space) and a mesh body. Normals and
	// points are returned in world space; the normal points from the surface toward the sphere centre.
	void SphereContacts(const RigidBody& meshBody, const glm::vec3& center, float radius, float skin,
		std::vector<CollisionWorld::SphereContact>& out);

	// The deepest of those contacts. False when none is within skin.
	bool DeepestSphereContact(const RigidBody& meshBody, const glm::vec3& center, float radius, float skin,
		CollisionWorld::SphereContact& out);

	// Box/triangle contacts between an oriented box (world centre, half extents, world rotation) and a mesh body.
	// Same conventions as SphereContacts: world-space points, normals from the mesh toward the box.
	void BoxContacts(const RigidBody& meshBody, const glm::vec3& center, const glm::vec3& halfExtents,
		const glm::quat& rotation, float skin, std::vector<CollisionWorld::SphereContact>& out);

}
