#pragma once
// Shape casts (sweeps) of a sphere, capsule or box through the physics world. A probe moves along a direction and
// reports the first body it touches: distance, contact point, surface normal (pointing back at the probe) and body.
// Sphere and capsule probes also hit mesh colliders; a box probe does not (box against mesh is not supported).
//
// Method: the probe is tested at steps no longer than about half its radius (so it cannot skip a thin body), then the
// first overlapping step is refined by bisection. The result is exact to the bisection tolerance for convex bodies.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>

class PhysicsEngine;
class RigidBody;

namespace ShapeSweep {

enum class Kind { SPHERE, CAPSULE, BOX };

struct Probe {
	Kind kind = Kind::SPHERE;
	glm::vec3 center{ 0.0f };      // sphere centre, box centre, capsule midpoint
	float radius = 0.5f;           // sphere and capsule radius
	float halfSegment = 0.0f;      // capsule half length of the segment (excluding caps), along the rotated Y axis
	glm::vec3 halfExtents{ 0.5f }; // box half extents (box rotated by rotation)
	glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };   // box orientation; capsule axis is rotation * Y
};

struct Hit {
	bool hit = false;
	float distance = 0.0f;                 // distance travelled along the (normalised) direction
	glm::vec3 point{ 0.0f };               // approximate contact point on the probe's surface
	glm::vec3 normal{ 0.0f, 1.0f, 0.0f };  // unit surface normal at the contact, pointing toward the probe
	std::shared_ptr<RigidBody> body;
};

// Sweeps the probe along dir for up to maxDistance. Only bodies whose collision layer intersects mask are considered;
// ignore (optional) is skipped. A probe that already overlaps at the start reports distance 0.
bool Sweep(PhysicsEngine& physics, const Probe& probe, const glm::vec3& dir, float maxDistance, uint32_t mask,
	Hit& out, const RigidBody* ignore = nullptr);

}
