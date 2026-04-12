#include "RigidBody.h"
#include "SceneNode.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

RigidBody::RigidBody() {
    // Initialize with default values
    computeAABB();
}

// ============== BODY TYPE ==============

void RigidBody::SetBodyType(BodyType type) {
    m_bodyType = type;
    
    // Static bodies have infinite mass
    if (type == BodyType::STATIC) {
        m_inverseMass = 0.0f;
        m_inverseInertiaLocal = glm::mat3(0.0f);
        m_linearVelocity = glm::vec3(0.0f);
        m_angularVelocity = glm::vec3(0.0f);
        m_isSleeping = true;
        m_transformOwner = TransformOwner::SCENE;  // Static bodies are scene-owned
    } else if (type == BodyType::KINEMATIC) {
        if (m_mass > 0.0f) {
            m_inverseMass = 1.0f / m_mass;
            computeInertiaTensor();
        }
        m_transformOwner = TransformOwner::SCENE;  // Kinematic bodies are scene-owned
    } else { // DYNAMIC
        if (m_mass > 0.0f) {
            m_inverseMass = 1.0f / m_mass;
            computeInertiaTensor();
        }
        m_transformOwner = TransformOwner::PHYSICS;  // Dynamic bodies are physics-owned
    }
}

// ============== SHAPE ==============

void RigidBody::setSphere(float radius) {
    m_shapeType = ShapeType::SPHERE;
    m_radius = radius;
    computeInertiaTensor();
    computeAABB();
}

void RigidBody::setBox(const glm::vec3& halfExtents) {
    m_shapeType = ShapeType::BOX;
    m_halfExtents = halfExtents;
    computeInertiaTensor();
    computeAABB();
}

void RigidBody::setPlane(const glm::vec3& normal, float distance) {
    m_shapeType = ShapeType::PLANE;
    m_planeNormal = glm::normalize(normal);
    m_planeDistance = distance;
    
    // Planes are always static
    m_bodyType = BodyType::STATIC;
    m_inverseMass = 0.0f;
    m_inverseInertiaLocal = glm::mat3(0.0f);
}

// ============== MASS PROPERTIES ==============

void RigidBody::setMass(float mass) {
    m_mass = std::max(0.0f, mass);
    
    if (m_bodyType == BodyType::STATIC || m_mass <= 0.0f) {
        m_inverseMass = 0.0f;
    } else {
        m_inverseMass = 1.0f / m_mass;
    }
    
    computeInertiaTensor();
}

void RigidBody::computeInertiaTensor() {
    if (m_bodyType == BodyType::STATIC || m_inverseMass <= 0.0f) {
        m_inertiaLocal = glm::mat3(0.0f);
        m_inverseInertiaLocal = glm::mat3(0.0f);
        return;
    }
    
    glm::mat3 inertia(0.0f);
    
    switch (m_shapeType) {
        case ShapeType::SPHERE: {
            // Solid sphere: I = (2/5) * m * r^2
            float I = (2.0f / 5.0f) * m_mass * m_radius * m_radius;
            inertia[0][0] = I;
            inertia[1][1] = I;
            inertia[2][2] = I;
            break;
        }
        
        case ShapeType::BOX: {
            // Solid box: I_x = (1/12) * m * (h^2 + d^2), etc.
            float x = m_halfExtents.x * 2.0f;
            float y = m_halfExtents.y * 2.0f;
            float z = m_halfExtents.z * 2.0f;
            float factor = m_mass / 12.0f;
            
            inertia[0][0] = factor * (y * y + z * z);
            inertia[1][1] = factor * (x * x + z * z);
            inertia[2][2] = factor * (x * x + y * y);
            break;
        }
        
        case ShapeType::PLANE: {
            // Planes have infinite inertia (zero inverse)
            m_inertiaLocal = glm::mat3(0.0f);
            m_inverseInertiaLocal = glm::mat3(0.0f);
            return;
        }
    }
    
    m_inertiaLocal = inertia;
    
    // Compute inverse (assuming diagonal inertia tensor for these primitives)
    m_inverseInertiaLocal = glm::mat3(0.0f);
    if (inertia[0][0] > 0.0f) m_inverseInertiaLocal[0][0] = 1.0f / inertia[0][0];
    if (inertia[1][1] > 0.0f) m_inverseInertiaLocal[1][1] = 1.0f / inertia[1][1];
    if (inertia[2][2] > 0.0f) m_inverseInertiaLocal[2][2] = 1.0f / inertia[2][2];
}

glm::mat3 RigidBody::getInverseInertiaWorld() const {
    if (m_bodyType == BodyType::STATIC) {
        return glm::mat3(0.0f);
    }
    
    // Transform inverse inertia to world space: R * I^-1 * R^T
    glm::mat3 R = glm::mat3_cast(m_orientation);
    return R * m_inverseInertiaLocal * glm::transpose(R);
}

// ============== POSITION & ORIENTATION ==============

void RigidBody::setOrientation(const glm::quat& orient) {
    m_orientation = glm::normalize(orient);
}

void RigidBody::storePreviousState() {
    m_previousPosition = m_position;
    m_previousOrientation = m_orientation;
}

glm::vec3 RigidBody::getInterpolatedPosition(float alpha) const {
    return glm::mix(m_previousPosition, m_position, alpha);
}

glm::quat RigidBody::getInterpolatedOrientation(float alpha) const {
    return glm::slerp(m_previousOrientation, m_orientation, alpha);
}

glm::mat4 RigidBody::getTransformMatrix() const {
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), m_position);
    glm::mat4 rotation = glm::mat4_cast(m_orientation);
    return translation * rotation;
}

// ============== FORCES & IMPULSES ==============

void RigidBody::applyForce(const glm::vec3& force) {
    if (m_bodyType != BodyType::DYNAMIC || m_isSleeping) return;
    // Validate force is finite
    if (!std::isfinite(force.x) || !std::isfinite(force.y) || !std::isfinite(force.z)) return;
    m_force += force;
}

void RigidBody::applyForceAtPoint(const glm::vec3& force, const glm::vec3& worldPoint) {
    if (m_bodyType != BodyType::DYNAMIC || m_isSleeping) return;
    // Validate inputs are finite
    if (!std::isfinite(force.x) || !std::isfinite(force.y) || !std::isfinite(force.z)) return;
    if (!std::isfinite(worldPoint.x) || !std::isfinite(worldPoint.y) || !std::isfinite(worldPoint.z)) return;
    m_force += force;
    m_torque += glm::cross(worldPoint - m_position, force);
}

void RigidBody::applyTorque(const glm::vec3& torque) {
    if (m_bodyType != BodyType::DYNAMIC || m_isSleeping) return;
    // Validate torque is finite
    if (!std::isfinite(torque.x) || !std::isfinite(torque.y) || !std::isfinite(torque.z)) return;
    m_torque += torque;
}

void RigidBody::applyImpulse(const glm::vec3& impulse) {
    if (m_bodyType != BodyType::DYNAMIC || isEditorControlled()) return;
    // Validate impulse is finite
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) || !std::isfinite(impulse.z)) return;
    wakeUp();
    m_linearVelocity += impulse * m_inverseMass;
    
    // Clamp velocity to prevent explosion
    float velMag = glm::length(m_linearVelocity);
    if (velMag > 100.0f) {
        m_linearVelocity *= (100.0f / velMag);
    }
}

void RigidBody::applyImpulseAtPoint(const glm::vec3& impulse, const glm::vec3& worldPoint) {
    if (m_bodyType != BodyType::DYNAMIC || isEditorControlled()) return;
    // Validate inputs are finite
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) || !std::isfinite(impulse.z)) return;
    if (!std::isfinite(worldPoint.x) || !std::isfinite(worldPoint.y) || !std::isfinite(worldPoint.z)) return;
    wakeUp();
    m_linearVelocity += impulse * m_inverseMass;
    m_angularVelocity += getInverseInertiaWorld() * glm::cross(worldPoint - m_position, impulse);
    
    // Clamp velocities to prevent explosion
    float velMag = glm::length(m_linearVelocity);
    if (velMag > 100.0f) {
        m_linearVelocity *= (100.0f / velMag);
    }
    float angVelMag = glm::length(m_angularVelocity);
    if (angVelMag > 50.0f) {
        m_angularVelocity *= (50.0f / angVelMag);
    }
}

void RigidBody::applyAngularImpulse(const glm::vec3& impulse) {
    if (m_bodyType != BodyType::DYNAMIC || isEditorControlled()) return;
    // Validate impulse is finite
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) || !std::isfinite(impulse.z)) return;
    wakeUp();
    m_angularVelocity += getInverseInertiaWorld() * impulse;
    
    // Clamp angular velocity
    float angVelMag = glm::length(m_angularVelocity);
    if (angVelMag > 50.0f) {
        m_angularVelocity *= (50.0f / angVelMag);
    }
}

void RigidBody::clearForces() {
    m_force = glm::vec3(0.0f);
    m_torque = glm::vec3(0.0f);
}

// ============== COLLISION FILTERING ==============

bool RigidBody::canCollideWith(const RigidBody& other) const {
    // Check layer/mask compatibility
    return (m_collisionLayer & other.m_collisionMask) != 0 &&
           (other.m_collisionLayer & m_collisionMask) != 0;
}

// ============== SLEEP MANAGEMENT ==============

void RigidBody::setSleeping(bool sleeping) {
    if (m_bodyType == BodyType::STATIC) {
        m_isSleeping = true;
        m_transformOwner = TransformOwner::SCENE;
        return;
    }
    
    m_isSleeping = sleeping;
    if (sleeping) {
        m_linearVelocity = glm::vec3(0.0f);
        m_angularVelocity = glm::vec3(0.0f);
        // Sleeping bodies become scene-owned unless the editor is actively driving them.
        if (m_bodyType == BodyType::DYNAMIC && !m_isGizmoGrabbed) {
            m_transformOwner = TransformOwner::SCENE;
        }
    }
    m_sleepTime = 0.0f;
}

void RigidBody::wakeUp() {
    if (m_bodyType == BodyType::STATIC) return;
    m_isSleeping = false;
    m_sleepTime = 0.0f;
    // Waking dynamic bodies become physics-owned again (unless editor-grabbed)
    if (m_bodyType == BodyType::DYNAMIC && !m_isGizmoGrabbed) {
        m_transformOwner = TransformOwner::PHYSICS;
    }
}

void RigidBody::updateSleepState(float linearThreshold, float angularThreshold, float dt) {
    if (!m_sleepingEnabled || m_bodyType != BodyType::DYNAMIC) return;
    
    float linearSpeed = glm::length(m_linearVelocity);
    float angularSpeed = glm::length(m_angularVelocity);
    
    if (linearSpeed < linearThreshold && angularSpeed < angularThreshold) {
        m_sleepTime += dt;
    } else {
        m_sleepTime = 0.0f;
        m_isSleeping = false;
    }
}

// ============== GIZMO MANIPULATION ==============

void RigidBody::setKinematicTarget(const glm::vec3& position, const glm::quat& orientation) {
    m_kinematicTargetPosition = position;
    m_kinematicTargetOrientation = orientation;
    m_hasKinematicTarget = true;
}

// ============== AABB ==============

void RigidBody::computeAABB() {
    switch (m_shapeType) {
        case ShapeType::SPHERE: {
            m_aabbMin = m_position - glm::vec3(m_radius);
            m_aabbMax = m_position + glm::vec3(m_radius);
            break;
        }
        
        case ShapeType::BOX: {
            // Transform box corners to world space and compute AABB
            glm::mat3 R = glm::mat3_cast(m_orientation);
            glm::mat3 absR;
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    absR[i][j] = std::abs(R[i][j]);
                }
            }
            
            glm::vec3 worldExtent = absR * m_halfExtents;
            m_aabbMin = m_position - worldExtent;
            m_aabbMax = m_position + worldExtent;
            break;
        }
        
        case ShapeType::PLANE: {
            // Planes are infinite - use very large bounds
            constexpr float LARGE = 1e10f;
            m_aabbMin = glm::vec3(-LARGE);
            m_aabbMax = glm::vec3(LARGE);
            
            // Restrict in normal direction
            if (std::abs(m_planeNormal.y) > 0.9f) {
                m_aabbMin.y = m_planeDistance - 0.01f;
                m_aabbMax.y = m_planeDistance + 0.01f;
            }
            break;
        }
    }
}

void RigidBody::computeFatAABB(float margin) {
    computeAABB();
    m_fatAABBMin = m_aabbMin - glm::vec3(margin);
    m_fatAABBMax = m_aabbMax + glm::vec3(margin);
}

bool RigidBody::needsFatAABBUpdate(float margin) const {
    // Check if current AABB has moved outside fat AABB
    return m_aabbMin.x < m_fatAABBMin.x || m_aabbMin.y < m_fatAABBMin.y || m_aabbMin.z < m_fatAABBMin.z ||
           m_aabbMax.x > m_fatAABBMax.x || m_aabbMax.y > m_fatAABBMax.y || m_aabbMax.z > m_fatAABBMax.z;
}

// ============== INTEGRATION ==============

void RigidBody::integrateForces(float dt, const glm::vec3& gravity) {
    if (m_bodyType != BodyType::DYNAMIC || m_isSleeping || isEditorControlled()) return;
    
    // Apply gravity
    glm::vec3 totalAcceleration = gravity * m_gravityScale + m_acceleration;
    
    // Integrate linear velocity: v += (F/m + g) * dt
    m_linearVelocity += (m_force * m_inverseMass + totalAcceleration) * dt;
    
    // Integrate angular velocity: ? += I^-1 * ? * dt
    m_angularVelocity += getInverseInertiaWorld() * m_torque * dt;
    
    // Validate and clamp velocities
    if (!std::isfinite(m_linearVelocity.x) || !std::isfinite(m_linearVelocity.y) || !std::isfinite(m_linearVelocity.z)) {
        m_linearVelocity = glm::vec3(0.0f);
    }
    if (!std::isfinite(m_angularVelocity.x) || !std::isfinite(m_angularVelocity.y) || !std::isfinite(m_angularVelocity.z)) {
        m_angularVelocity = glm::vec3(0.0f);
    }
}

void RigidBody::integrateVelocities(float dt) {
    if (m_bodyType == BodyType::STATIC || m_isSleeping || isEditorControlled()) return;
    
    if (m_bodyType == BodyType::KINEMATIC && m_hasKinematicTarget) {
        // Kinematic bodies move directly to target
        m_position = m_kinematicTargetPosition;
        m_orientation = m_kinematicTargetOrientation;
    } else {
        // Dynamic bodies integrate velocities
        m_position += m_linearVelocity * dt;
        
        // Integrate orientation: q += 0.5 * ? * q * dt
        glm::quat spin(0.0f, m_angularVelocity.x * 0.5f, m_angularVelocity.y * 0.5f, m_angularVelocity.z * 0.5f);
        m_orientation += spin * m_orientation * dt;
        m_orientation = glm::normalize(m_orientation);
    }
    
    // Validate position and orientation
    if (!std::isfinite(m_position.x) || !std::isfinite(m_position.y) || !std::isfinite(m_position.z)) {
        m_position = m_previousPosition; // Revert to previous valid state
        m_linearVelocity = glm::vec3(0.0f);
    }
    if (!std::isfinite(m_orientation.x) || !std::isfinite(m_orientation.y) || 
        !std::isfinite(m_orientation.z) || !std::isfinite(m_orientation.w)) {
        m_orientation = m_previousOrientation; // Revert to previous valid state
        m_angularVelocity = glm::vec3(0.0f);
    }
    
    // Clamp position to reasonable bounds
    m_position = glm::clamp(m_position, glm::vec3(-100000.0f), glm::vec3(100000.0f));
    
    computeAABB();
}

void RigidBody::applyDamping(float dt) {
    if (m_bodyType != BodyType::DYNAMIC) return;
    
    // Apply linear damping: v *= (1 - damping)^dt
    float linearFactor = std::pow(1.0f - m_linearDamping, dt);
    m_linearVelocity *= linearFactor;
    
    // Apply angular damping
    float angularFactor = std::pow(1.0f - m_angularDamping, dt);
    m_angularVelocity *= angularFactor;
}

// ============== UTILITY ==============

glm::vec3 RigidBody::getVelocityAtPoint(const glm::vec3& worldPoint) const {
    return m_linearVelocity + glm::cross(m_angularVelocity, worldPoint - m_position);
}

glm::vec3 RigidBody::worldToLocal(const glm::vec3& worldPoint) const {
    glm::quat invOrient = glm::conjugate(m_orientation);
    return invOrient * (worldPoint - m_position);
}

glm::vec3 RigidBody::localToWorld(const glm::vec3& localPoint) const {
    return m_position + m_orientation * localPoint;
}

glm::vec3 RigidBody::worldToLocalDirection(const glm::vec3& worldDir) const {
    return glm::conjugate(m_orientation) * worldDir;
}

glm::vec3 RigidBody::localToWorldDirection(const glm::vec3& localDir) const {
    return m_orientation * localDir;
}
