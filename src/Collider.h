#pragma once
#include <glm/glm.hpp>
#include <memory>

class RigidBody;
struct Contact;

// Base class for colliders (Sphere, Box, Plane)
class Collider {
public:
    enum class Type { SPHERE, BOX, PLANE };
    virtual ~Collider() = default;

    std::weak_ptr<RigidBody> rigidBody;  // Back-reference to owning rigid body (if any)
    glm::vec3 center = glm::vec3(0.0f);  // World-space center of the collider

    virtual Type GetType() const = 0;
    // Test collision with another collider. If collision occurs, fill Contact info.
    virtual bool TestCollision(const Collider& other, Contact& contact) const = 0;
    // Compute inertia tensor for this shape (in local space) given mass
    virtual glm::mat3 ComputeInertiaTensor(float mass) const = 0;

    // Utility to set/get center
    void SetCenter(const glm::vec3& c) { center = c; }
    const glm::vec3& GetCenter() const { return center; }
};
