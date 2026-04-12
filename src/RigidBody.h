#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <cstdint>

// Forward declarations
class SceneNode;
class Collider;
struct Contact;

/**
 * RigidBody - Production-ready rigid body for physics simulation
 * 
 * Features:
 * - Three body types: Static, Dynamic, Kinematic
 * - Three shape types: Box, Sphere, Plane
 * - Full rotational dynamics with inertia tensors
 * - Sleep management for performance
 * - Gizmo manipulation support
 * - Scene node synchronization
 * - Explicit transform ownership model
 */
class RigidBody : public std::enable_shared_from_this<RigidBody> {
public:
    // Body type determines how physics affects the body
    enum class BodyType {
        STATIC,     // Never moves, infinite mass, used for environment
        DYNAMIC,    // Fully simulated, responds to forces and collisions
        KINEMATIC   // User-controlled motion, affects dynamics but not affected by them
    };
    
    // Collision shape type
    enum class ShapeType {
        SPHERE,     // Spherical collider
        BOX,        // Oriented bounding box
        PLANE       // Infinite plane (static only)
    };
    
    // Transform ownership - who is authoritative for this body's transform
    enum class TransformOwner {
        PHYSICS,    // Physics simulation owns transform (dynamic bodies while simulating)
        SCENE,      // Scene/ECS owns transform (kinematic, static, or sleeping bodies)
        EDITOR      // Editor/gizmo temporarily owns transform (during manipulation)
    };
    
    // Unique identifier for physics system
    using BodyID = uint32_t;
    static constexpr BodyID INVALID_BODY_ID = 0;

public:
    RigidBody();
    ~RigidBody() = default;
    
    // ============== IDENTIFICATION ==============
    
    BodyID GetBodyID() const { return m_bodyID; }
    void SetBodyID(BodyID id) { m_bodyID = id; }
    
    // ============== BODY TYPE ==============
    
    void SetBodyType(BodyType type);
    BodyType GetBodyType() const { return m_bodyType; }
    
    bool IsStatic() const { return m_bodyType == BodyType::STATIC; }
    bool IsDynamic() const { return m_bodyType == BodyType::DYNAMIC; }
    bool IsKinematic() const { return m_bodyType == BodyType::KINEMATIC; }
    
    // ============== SHAPE ==============
    
    void setShape(ShapeType type) { m_shapeType = type; }
    ShapeType getShapeType() const { return m_shapeType; }
    
    // Sphere shape
    void setSphere(float radius);
    float getRadius() const { return m_radius; }
    float getBoundingRadius() const { return m_radius; }
    void setBoundingRadius(float r) { m_radius = r; }
    
    // Box shape
    void setBox(const glm::vec3& halfExtents);
    glm::vec3 getHalfExtents() const { return m_halfExtents; }
    
    // Plane shape (static only)
    void setPlane(const glm::vec3& normal, float distance);
    glm::vec3 getPlaneNormal() const { return m_planeNormal; }
    float getPlaneHeight() const { return m_planeDistance; }
    float getPlaneDistance() const { return m_planeDistance; }
    
    // ============== MASS PROPERTIES ==============
    
    void setMass(float mass);
    float getMass() const { return m_mass; }
    float getInverseMass() const { return m_inverseMass; }
    
    void computeInertiaTensor();
    glm::mat3 getInertiaLocal() const { return m_inertiaLocal; }
    glm::mat3 getInverseInertiaLocal() const { return m_inverseInertiaLocal; }
    glm::mat3 getInverseInertiaWorld() const;
    
    // ============== POSITION & ORIENTATION ==============
    
    // Transform writes must respect ownership rules
    // setPosition/setOrientation can only be called by the owner
    // Physics: during solver and integration
    // Scene: during kinematic setup or scene sync
    // Editor: during gizmo manipulation
    
    void setPosition(const glm::vec3& pos) { 
        // Position can be set by any owner - no assertion
        // Ownership rules are enforced at higher level (PhysicsEngine sync)
        m_position = pos; 
    }
    glm::vec3 getPosition() const { return m_position; }
    
    void setOrientation(const glm::quat& orient);
    glm::quat getOrientation() const { return m_orientation; }
    
    // Previous state for interpolation
    glm::vec3 getPreviousPosition() const { return m_previousPosition; }
    glm::quat getPreviousOrientation() const { return m_previousOrientation; }
    void storePreviousState();
    
    // Interpolated state for rendering
    glm::vec3 getInterpolatedPosition(float alpha) const;
    glm::quat getInterpolatedOrientation(float alpha) const;
    
    // Transform matrix
    glm::mat4 getTransformMatrix() const;
    
    // ============== VELOCITY ==============
    
    void setVelocity(const glm::vec3& vel) { m_linearVelocity = vel; }
    glm::vec3 getVelocity() const { return m_linearVelocity; }
    
    void setAngularVelocity(const glm::vec3& angVel) { m_angularVelocity = angVel; }
    glm::vec3 getAngularVelocity() const { return m_angularVelocity; }
    
    // Acceleration (for gravity)
    void setAcceleration(const glm::vec3& accel) { m_acceleration = accel; }
    glm::vec3 getAcceleration() const { return m_acceleration; }
    
    // ============== FORCES & IMPULSES ==============
    
    void applyForce(const glm::vec3& force);
    void applyForceAtPoint(const glm::vec3& force, const glm::vec3& worldPoint);
    void applyTorque(const glm::vec3& torque);
    void applyImpulse(const glm::vec3& impulse);
    void applyImpulseAtPoint(const glm::vec3& impulse, const glm::vec3& worldPoint);
    void applyAngularImpulse(const glm::vec3& impulse);
    
    void clearForces();
    glm::vec3 getForce() const { return m_force; }
    glm::vec3 getTorque() const { return m_torque; }
    
    // ============== MATERIAL PROPERTIES ==============
    
    void setFriction(float friction) { m_friction = glm::clamp(friction, 0.0f, 1.0f); }
    float getFriction() const { return m_friction; }
    
    void setRestitution(float restitution) { m_restitution = glm::clamp(restitution, 0.0f, 1.0f); }
    float getRestitution() const { return m_restitution; }
    
    void setLinearDamping(float damping) { m_linearDamping = glm::max(0.0f, damping); }
    float getLinearDamping() const { return m_linearDamping; }
    
    void setAngularDamping(float damping) { m_angularDamping = glm::max(0.0f, damping); }
    float getAngularDamping() const { return m_angularDamping; }
    
    void setGravityScale(float scale) { m_gravityScale = scale; }
    float getGravityScale() const { return m_gravityScale; }
    
    // ============== COLLISION LAYERS ==============
    
    void setCollisionLayer(uint32_t layer) { m_collisionLayer = layer; }
    uint32_t getCollisionLayer() const { return m_collisionLayer; }
    
    void setCollisionMask(uint32_t mask) { m_collisionMask = mask; }
    uint32_t getCollisionMask() const { return m_collisionMask; }
    
    bool canCollideWith(const RigidBody& other) const;
    
    // ============== SLEEP MANAGEMENT ==============
    
    void setSleepingEnabled(bool enabled) { m_sleepingEnabled = enabled; }
    bool isSleepingEnabled() const { return m_sleepingEnabled; }
    
    bool isSleeping() const { return m_isSleeping; }
    void setSleeping(bool sleeping);
    void wakeUp();
    
    void updateSleepState(float linearThreshold, float angularThreshold, float dt);
    float getSleepTime() const { return m_sleepTime; }
    
    // ============== GIZMO MANIPULATION ==============
    
    void setGizmoGrabbed(bool grabbed) { 
        m_isGizmoGrabbed = grabbed;
        // Update ownership based on gizmo state
        if (grabbed) {
            m_transformOwner = TransformOwner::EDITOR;
        } else if (IsDynamic() && !m_isSleeping) {
            m_transformOwner = TransformOwner::PHYSICS;
        } else {
            m_transformOwner = TransformOwner::SCENE;
        }
    }
    bool isGizmoGrabbed() const { return m_isGizmoGrabbed; }
    
    // ============== TRANSFORM OWNERSHIP ==============
    
    TransformOwner getTransformOwner() const { return m_transformOwner; }
    void setTransformOwner(TransformOwner owner) { m_transformOwner = owner; }
    
    // Check if physics should write transforms for this body
    bool isPhysicsOwned() const { return m_transformOwner == TransformOwner::PHYSICS; }
    
    // Check if scene/editor can write transforms for this body
    bool isSceneOwned() const { return m_transformOwner == TransformOwner::SCENE; }
    bool isEditorOwned() const { return m_transformOwner == TransformOwner::EDITOR; }
    bool isEditorControlled() const { return m_isGizmoGrabbed || m_transformOwner == TransformOwner::EDITOR; }
    
    /**
     * Validate that a transform write is permitted from the given owner
     * Returns false if ownership violation detected (should log and skip write)
     * @param requester Owner attempting the write (PHYSICS, SCENE, or EDITOR)
     * @return True if write is permitted
     */
    bool ValidateTransformOwnership(TransformOwner requester) const {
        // Enforce strict ownership
        // Physics can only write if it owns the body
        if (requester == TransformOwner::PHYSICS && !isPhysicsOwned()) {
            return false;
        }
        // Scene can write if scene owns or sleeping (quasi-owned)
        if (requester == TransformOwner::SCENE && !isSceneOwned()) {
            return false;
        }
        // Editor can only write if explicitly grabbed
        if (requester == TransformOwner::EDITOR && !isEditorOwned()) {
            return false;
        }
        return true;
    }
    
    // ============== KINEMATIC TARGET ==============
    
    // Kinematic target for gizmo control and kinematic motion
    void setKinematicTarget(const glm::vec3& position, const glm::quat& orientation);
    glm::vec3 getKinematicTargetPosition() const { return m_kinematicTargetPosition; }
    glm::quat getKinematicTargetOrientation() const { return m_kinematicTargetOrientation; }
    bool hasKinematicTarget() const { return m_hasKinematicTarget; }
    void clearKinematicTarget() { m_hasKinematicTarget = false; }
    
    // ============== SCENE NODE ATTACHMENT ==============
    
    void AttachNode(std::shared_ptr<SceneNode> node) { m_attachedNode = node; }
    std::shared_ptr<SceneNode> getAttachedNode() const { return m_attachedNode.lock(); }
    bool hasAttachedNode() const { return !m_attachedNode.expired(); }
    
    // ============== AABB ==============
    
    void computeAABB();
    glm::vec3 getAABBMin() const { return m_aabbMin; }
    glm::vec3 getAABBMax() const { return m_aabbMax; }
    
    // Fat AABB for broadphase (includes margin)
    void computeFatAABB(float margin);
    glm::vec3 getFatAABBMin() const { return m_fatAABBMin; }
    glm::vec3 getFatAABBMax() const { return m_fatAABBMax; }
    bool needsFatAABBUpdate(float margin) const;
    
    // ============== INTEGRATION ==============
    
    void integrateForces(float dt, const glm::vec3& gravity);
    void integrateVelocities(float dt);
    void applyDamping(float dt);
    
    // ============== UTILITY ==============
    
    // Get velocity at world point (includes angular contribution)
    glm::vec3 getVelocityAtPoint(const glm::vec3& worldPoint) const;
    
    // Transform point between local and world space
    glm::vec3 worldToLocal(const glm::vec3& worldPoint) const;
    glm::vec3 localToWorld(const glm::vec3& localPoint) const;
    glm::vec3 worldToLocalDirection(const glm::vec3& worldDir) const;
    glm::vec3 localToWorldDirection(const glm::vec3& localDir) const;

private:
    // Identification
    BodyID m_bodyID = INVALID_BODY_ID;
    
    // Type
    BodyType m_bodyType = BodyType::DYNAMIC;
    ShapeType m_shapeType = ShapeType::SPHERE;
    
    // Mass properties
    float m_mass = 1.0f;
    float m_inverseMass = 1.0f;
    glm::mat3 m_inertiaLocal = glm::mat3(1.0f);
    glm::mat3 m_inverseInertiaLocal = glm::mat3(1.0f);
    
    // Shape parameters
    float m_radius = 0.5f;                              // Sphere radius
    glm::vec3 m_halfExtents = glm::vec3(0.5f);          // Box half-extents
    glm::vec3 m_planeNormal = glm::vec3(0.0f, 1.0f, 0.0f);  // Plane normal
    float m_planeDistance = 0.0f;                       // Plane distance from origin
    
    // Transform
    glm::vec3 m_position = glm::vec3(0.0f);
    glm::quat m_orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 m_previousPosition = glm::vec3(0.0f);
    glm::quat m_previousOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    
    // Velocity
    glm::vec3 m_linearVelocity = glm::vec3(0.0f);
    glm::vec3 m_angularVelocity = glm::vec3(0.0f);
    glm::vec3 m_acceleration = glm::vec3(0.0f);
    
    // Forces (accumulated per step)
    glm::vec3 m_force = glm::vec3(0.0f);
    glm::vec3 m_torque = glm::vec3(0.0f);
    
    // Material
    float m_friction = 0.5f;
    float m_restitution = 0.3f;
    float m_linearDamping = 0.0f;
    float m_angularDamping = 0.05f;
    float m_gravityScale = 1.0f;
    
    // Collision filtering
    uint32_t m_collisionLayer = 1;
    uint32_t m_collisionMask = 0xFFFFFFFF;
    
    // Sleep state
    bool m_sleepingEnabled = true;
    bool m_isSleeping = false;
    float m_sleepTime = 0.0f;
    
    // Gizmo state
    bool m_isGizmoGrabbed = false;
    
    // Transform ownership - explicit tracking of who controls this body's transform
    TransformOwner m_transformOwner = TransformOwner::SCENE;
    
    // Kinematic target (for motion interpolation)
    glm::vec3 m_kinematicTargetPosition = glm::vec3(0.0f);
    glm::quat m_kinematicTargetOrientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    bool m_hasKinematicTarget = false;
    
    // AABB
    glm::vec3 m_aabbMin = glm::vec3(0.0f);
    glm::vec3 m_aabbMax = glm::vec3(0.0f);
    glm::vec3 m_fatAABBMin = glm::vec3(0.0f);
    glm::vec3 m_fatAABBMax = glm::vec3(0.0f);
    
    // Scene attachment
    std::weak_ptr<SceneNode> m_attachedNode;
};
