#pragma once
// Exact overlap tests between two rigid body shapes (sphere, box, plane) for queries and the character
// controller. Unlike PhysicsCollision::TestCollision these never go through the GJK prepass, so touching
// and near-touching shapes are answered exactly. Plane against plane never overlaps.

#include <glm/glm.hpp>

class RigidBody;

namespace ShapeOverlap {

	// True when a and b overlap or lie within `skin` of each other (skin 0: touching counts). On true, pushA is
	// the unit direction that moves a out of b and depth is the penetration along it (<= 0 within the skin).
	// Planes are treated as one-sided surfaces: a shape that is not above the plane is pushed out along +normal.
	bool Test(const RigidBody& a, const RigidBody& b, float skin, glm::vec3& pushA, float& depth);

}
