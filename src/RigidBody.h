#pragma once
#define GLM_ENABLE_EXPERIMENTAL
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>

// Forward‐declare scene node class (for rendering or scene graph, not used here)
class SceneNode;

class RigidBody {
public:
    enum class ShapeType {
        SPHERE,
        PLANE,
        BOX        // Added BOX shape
    };

    RigidBody();
    ~RigidBody();

    // Attach the SceneNode that this body will drive
    void AttachNode(const std::shared_ptr<SceneNode>& node);

    void DetachNode();

    // Basic linear properties
    void setMass(float m);
    float getMass() const;

    // For SPHERE shape: bounding radius
    void setBoundingRadius(float r);
    float getBoundingRadius() const;

    // For BOX shape: half-extents (half-size along each local axis)
    void setBox(const glm::vec3& halfExtents);
    glm::vec3 getHalfExtents() const;

    // Linear motion
    void setAcceleration(const glm::vec3& a);
    glm::vec3 getAcceleration() const;
    void setVelocity(const glm::vec3& v);
    glm::vec3 getVelocity() const;
    void setPosition(const glm::vec3& pos);
    glm::vec3 getPosition() const;
    void setLinearDamping(float d);
    float getLinearDamping() const;

    // Angular motion (orientation as quaternion)
    void setOrientation(const glm::quat& q);
    glm::quat getOrientation() const;
    void setAngularVelocity(const glm::vec3& w);
    glm::vec3 getAngularVelocity() const;
    void setAngularDamping(float d);
    float getAngularDamping() const;

    // Set the friction coefficient (0.0f = no friction, 1.0f = max friction)
    void setFriction(float f);
    float getFriction() const;

    // Integrate physics for time step dt using specified integrator
    void integrate(float dt, const std::string& integrator);
    // Recompute inertia tensor based on shape
    void computeInertiaTensor();
    // Handle collision with another rigid body (impulse resolution for sphere-sphere)
    void resolveCollision(RigidBody& other, float restitution);

    // Interpolation support (for smooth rendering)
    void storePreviousState();
    void setInterpolatedState(float alpha);

    // Apply torque to this body
    void applyTorque(const glm::vec3& torque);

    // Shape type setter/getter
    void setShape(ShapeType type);
    ShapeType getShape() const;
    ShapeType getShapeType() const { return m_shapeType; } // Alias for getShape

    // For PLANE shape: define plane with normal and height (plane: dot(x, normal) = height)
    void setPlane(const glm::vec3& normal, float height);
    glm::vec3 getPlaneNormal() const;
    float getPlaneHeight() const;

    const glm::mat3& getInertiaTensorInv() const { return m_inertiaTensorInv; }
    void setInertiaTensorInv(const glm::mat3& inv) { m_inertiaTensorInv = inv; }

    void Shutdown(); // cleanup

private:
    // Integration methods
    void integrateEuler(float dt);
    void integrateRK2(float dt);
    void integrateRK4(float dt);
    void integrateVerlet(float dt);

    // State (linear)
    glm::vec3 m_position;
    glm::vec3 m_prevPosition;   // for interpolation
    glm::vec3 m_velocity;
    glm::vec3 m_acceleration;
    float m_linearDamping; // for linear motion
    float m_angularDamping; // for angular motion
    float m_friction; // for friction (0.0f = no friction, 1.0f = max friction)
    float m_mass;
    float m_radius;             // for sphere

    // State (angular)
    glm::quat m_orientation;
    glm::quat m_prevOrientation; // for interpolation
    glm::vec3 m_angularVelocity;
    glm::vec3 m_torque;

    glm::mat3 m_inertiaTensor;    // inertia tensor in local/body coordinates
    glm::mat3 m_inertiaTensorInv; // inverse of inertia tensor

    std::weak_ptr<SceneNode> m_node; // scene node for rendering (if any)

    // Shape info
    ShapeType m_shapeType = ShapeType::SPHERE;
    glm::vec3 m_planeNormal = glm::vec3(0.0f);
    float m_planeHeight = 0.0f;
    glm::vec3 m_halfExtents = glm::vec3(1.0f); // for BOX shape

};