#include "RigidBody.h"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include "SceneNode.h"

// Constructor/Destructor
RigidBody::RigidBody()
    : m_position(0.0f), m_prevPosition(0.0f),
    m_velocity(0.0f), m_acceleration(0.0f),
    m_linearDamping(0.0f), m_angularDamping(0.0f), m_friction(0.5f),
    m_mass(1.0f), m_radius(1.0f),
    m_orientation(1, 0, 0, 0), m_prevOrientation(1, 0, 0, 0),
    m_angularVelocity(0.0f), m_torque(0.0f),
    m_inertiaTensor(1.0f), m_inertiaTensorInv(1.0f),
    m_shapeType(ShapeType::SPHERE), m_planeNormal(0.0f), m_planeHeight(0.0f),
    m_halfExtents(1.0f, 1.0f, 1.0f)
{
    computeInertiaTensor();
}

RigidBody::~RigidBody() {}

// Attach a scene node (for visualization, if any)
void RigidBody::AttachNode(const std::shared_ptr<SceneNode>& node) {
    m_node = node;
}
void RigidBody::DetachNode() {
    m_node.reset();
}

// Set and get mass
void RigidBody::setMass(float m) {
    m_mass = m;
    computeInertiaTensor();
    // Recompute inverse inertia (treat zero/negative mass as static => zero inverse)
    if (m_mass <= 0.0f) {
        m_inertiaTensorInv = glm::mat3(0.0f);
    } else {
        // Avoid singular matrix inversion if components are zero
        // For diagonal inertia tensor this is safe
        m_inertiaTensorInv = glm::inverse(m_inertiaTensor);
    }
}
float RigidBody::getMass() const { return m_mass; }

// Set and get sphere radius
void RigidBody::setBoundingRadius(float r) {
    m_radius = r;
    computeInertiaTensor();
    if (m_mass <= 0.0f) {
        m_inertiaTensorInv = glm::mat3(0.0f);
    } else {
        m_inertiaTensorInv = glm::inverse(m_inertiaTensor);
    }
}
float RigidBody::getBoundingRadius() const { return m_radius; }

// Set box half extents
void RigidBody::setBox(const glm::vec3& halfExtents)
{
    m_halfExtents = halfExtents;
    m_radius = glm::length(halfExtents);          // half–diagonal for broad-phase
    m_shapeType = ShapeType::BOX;
    computeInertiaTensor();
    if (m_mass <= 0.0f) {
        m_inertiaTensorInv = glm::mat3(0.0f);
    } else {
        m_inertiaTensorInv = glm::inverse(m_inertiaTensor);
    }
}
glm::vec3 RigidBody::getHalfExtents() const { return m_halfExtents; }

// Linear motion getters/setters
void RigidBody::setAcceleration(const glm::vec3& a) { m_acceleration = a; }
glm::vec3 RigidBody::getAcceleration() const { return m_acceleration; }
void RigidBody::setVelocity(const glm::vec3& v) {
    m_velocity = v;
}
glm::vec3 RigidBody::getVelocity() const { return m_velocity; }
void RigidBody::setPosition(const glm::vec3& pos) {
    m_position = pos;
    // Update scene node transform if exists
    if (auto node = m_node.lock()) {
        // Set flag to indicate we're updating from physics
        node->SetUpdatingFromPhysics(true);
        
        glm::mat4 T = glm::translate(glm::mat4(1.0f), m_position);
        glm::mat4 R = glm::toMat4(m_orientation);
        node->SetTransform(T * R);
        
        // Clear flag
        node->SetUpdatingFromPhysics(false);
    }
}
glm::vec3 RigidBody::getPosition() const { return m_position; }

void RigidBody::setLinearDamping(float d)
{
    m_linearDamping = d;

}

float RigidBody::getLinearDamping() const
{
	return m_linearDamping;

}

// Orientation getters/setters
void RigidBody::setOrientation(const glm::quat& q) {
    m_orientation = glm::normalize(q);
    if (auto node = m_node.lock()) {
        // Set flag to indicate we're updating from physics
        node->SetUpdatingFromPhysics(true);
        
        glm::mat4 T = glm::translate(glm::mat4(1.0f), m_position);
        glm::mat4 R = glm::toMat4(m_orientation);
        node->SetTransform(T * R);
        
        // Clear flag
        node->SetUpdatingFromPhysics(false);
    }
}
glm::quat RigidBody::getOrientation() const { return m_orientation; }
void RigidBody::setAngularVelocity(const glm::vec3& w) { m_angularVelocity = w; }
glm::vec3 RigidBody::getAngularVelocity() const { return m_angularVelocity; }

void RigidBody::setAngularDamping(float d)
{
    m_angularDamping = d;

}
float RigidBody::getAngularDamping() const
{
	return m_angularDamping;

}

void RigidBody::setFriction(float f)
{
    m_friction = f;
}
float RigidBody::getFriction() const
{
	return m_friction;
}

// Store previous state (for interpolation)
void RigidBody::storePreviousState() {
    m_prevPosition = m_position;
    m_prevOrientation = m_orientation;
}

// Set interpolated state (for smooth rendering)
void RigidBody::setInterpolatedState(float alpha) {
    glm::vec3 interpPos = glm::mix(m_prevPosition, m_position, alpha);
    glm::quat interpRot = glm::slerp(m_prevOrientation, m_orientation, alpha);
    if (auto node = m_node.lock()) {
        glm::mat4 T = glm::translate(glm::mat4(1.0f), interpPos);
        glm::mat4 R = glm::toMat4(interpRot);
        node->SetTransform(T * R);
    }
}

// Apply torque
void RigidBody::applyTorque(const glm::vec3& torque) {
    m_torque += torque;
}

// Shape setter/getter
void RigidBody::setShape(ShapeType type) {
    m_shapeType = type;
    computeInertiaTensor();
    if (m_mass <= 0.0f) {
        m_inertiaTensorInv = glm::mat3(0.0f);
    } else {
        m_inertiaTensorInv = glm::inverse(m_inertiaTensor);
    }
}
RigidBody::ShapeType RigidBody::getShape() const {
    return m_shapeType;
}

// Plane definition setter/getter
void RigidBody::setPlane(const glm::vec3& normal, float height) {
    m_planeNormal = glm::normalize(normal);
    m_planeHeight = height;
    setShape(ShapeType::PLANE);
}
glm::vec3 RigidBody::getPlaneNormal() const { return m_planeNormal; }
float RigidBody::getPlaneHeight() const { return m_planeHeight; }

// Compute inertia tensor based on current shape
void RigidBody::computeInertiaTensor() {
    // Reset inertia
    glm::mat3 I(0.0f);
    switch (m_shapeType) {
    case ShapeType::SPHERE: {
        float Ival = (2.0f / 5.0f) * m_mass * m_radius * m_radius;
        I = glm::mat3(Ival);
        break;
    }
    case ShapeType::BOX: {
        glm::vec3 s = m_halfExtents * 2.0f;            // full sizes
        float ix = (1.0f / 12.0f) * m_mass * (s.y * s.y + s.z * s.z);
        float iy = (1.0f / 12.0f) * m_mass * (s.x * s.x + s.z * s.z);
        float iz = (1.0f / 12.0f) * m_mass * (s.x * s.x + s.y * s.y);
        m_inertiaTensor = glm::mat3(ix, 0, 0, 0, iy, 0, 0, 0, iz);
        if (m_mass <= 0.0f) {
            m_inertiaTensorInv = glm::mat3(0.0f);
        }
        else {
            m_inertiaTensorInv = glm::inverse(m_inertiaTensor);
        }
        return; // early return since we set members directly
    }
    case ShapeType::PLANE:
        // Treat plane as static (infinite inertia)
        m_inertiaTensor = glm::mat3(0.0f);
        m_inertiaTensorInv = glm::mat3(0.0f);
        return;
    }
    m_inertiaTensor = I;
    if (m_mass <= 0.0f) {
        m_inertiaTensorInv = glm::mat3(0.0f);
    } else {
        m_inertiaTensorInv = glm::inverse(m_inertiaTensor);
    }
}

// Integrate functions
void RigidBody::integrate(float dt, const std::string& integrator) {
    if (m_mass <= 0.0f) {
        // Static body: no integration
        m_torque = glm::vec3(0.0f);
        return;
    }
    if (integrator == "euler") {
        integrateEuler(dt);
    }
    else if (integrator == "rk2") {
        integrateRK2(dt);
    }
    else if (integrator == "rk4") {
        integrateRK4(dt);
    }
    else if (integrator == "verlet") {
        integrateVerlet(dt);
    }
    else {
        integrateEuler(dt);
    }
    // Clear torque (forces) after integration step
    m_torque = glm::vec3(0.0f);
}

void RigidBody::Shutdown() {
    DetachNode();  // Ensure visual link is removed
    m_inertiaTensor = glm::mat3(0.0f);
    m_inertiaTensorInv = glm::mat3(0.0f);
    m_position = glm::vec3(0.0f);
    m_velocity = glm::vec3(0.0f);
    m_acceleration = glm::vec3(0.0f);
    m_angularVelocity = glm::vec3(0.0f);
    m_torque = glm::vec3(0.0f);
}


// Euler integrator
void RigidBody::integrateEuler(float dt) {
    m_velocity += m_acceleration * dt;
    m_position += m_velocity * dt;

    glm::mat3 R = glm::toMat3(m_orientation);
    glm::mat3 iInvWorld = R * m_inertiaTensorInv * glm::transpose(R);
    glm::vec3 angularAcc = iInvWorld * m_torque;

    m_angularVelocity += angularAcc * dt;

    glm::quat wQuat(0.0f, m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z);
    m_orientation = glm::normalize(m_orientation + 0.5f * wQuat * m_orientation * dt);
}


// RK2 integrator (Midpoint)
void RigidBody::integrateRK2(float dt) {
    // Linear half-step
    glm::vec3 v0 = m_velocity;
    glm::vec3 p0 = m_position;
    glm::vec3 a0 = m_acceleration;
    glm::vec3 v_half = v0 + a0 * (dt * 0.5f);
    glm::vec3 p_half = p0 + v0 * (dt * 0.5f);
    // Full step
    m_velocity = v0 + a0 * dt;
    m_position = p0 + v_half * dt;
    // Rotational half-step
    glm::vec3 w0 = m_angularVelocity;
    glm::vec3 alpha0 = m_inertiaTensorInv * m_torque;
    glm::vec3 w_half = w0 + alpha0 * (dt * 0.5f);
    glm::quat orient_half = glm::normalize(m_orientation + 0.5f * glm::quat(0, w0.x, w0.y, w0.z) * m_orientation * (dt * 0.5f));
    // Full step
    m_angularVelocity = w0 + alpha0 * dt;
    glm::quat wHalfQuat(0.0f, w_half.x, w_half.y, w_half.z);
    m_orientation = glm::normalize(m_orientation + 0.5f * wHalfQuat * orient_half * dt);
}

// RK4 integrator
void RigidBody::integrateRK4(float dt) {
    //RK4 integration for linear motion and angular motion
    glm::vec3 v0 = m_velocity;
    glm::vec3 p0 = m_position;
    // Linear motion
    glm::vec3 k1v = m_acceleration;
    glm::vec3 k1p = v0;
    glm::vec3 k2v = m_acceleration + 0.5f * k1v * dt;
    glm::vec3 k2p = v0 + 0.5f * k1p * dt;
    glm::vec3 k3v = m_acceleration + 0.5f * k2v * dt;
    glm::vec3 k3p = v0 + 0.5f * k2p * dt;
    glm::vec3 k4v = m_acceleration + k3v * dt;
    glm::vec3 k4p = v0 + k3p * dt;
    m_velocity = v0 + (k1v + 2.0f * k2v + 2.0f * k3v + k4v) * (dt / 6.0f);
    m_position = p0 + (k1p + 2.0f * k2p + 2.0f * k3p + k4p) * (dt / 6.0f);
    // Angular motion
    glm::vec3 w0 = m_angularVelocity;
    glm::vec3 alpha0 = m_inertiaTensorInv * m_torque;
    glm::vec3 k1w = alpha0;
    glm::vec3 k1o = w0;
    glm::vec3 k2w = alpha0 + 0.5f * k1w * dt;
    glm::vec3 k2o = w0 + 0.5f * k1o * dt;
    glm::vec3 k3w = alpha0 + 0.5f * k2w * dt;
    glm::vec3 k3o = w0 + 0.5f * k2o * dt;
    glm::vec3 k4w = alpha0 + k3w * dt;
    glm::vec3 k4o = w0 + k3o * dt;
    m_angularVelocity = w0 + (k1w + 2.0f * k2w + 2.0f * k3w + k4w) * (dt / 6.0f);
    glm::quat wQuat(0.0f, m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z);
    m_orientation = glm::normalize(m_orientation + 0.5f * wQuat * m_orientation * dt);
}

// Verlet integrator
void RigidBody::integrateVerlet(float dt) {
    // Simple Verlet: x += v*dt + 0.5*a*dt^2, then v += a*dt
    glm::vec3 newPos = m_position + m_velocity * dt + 0.5f * m_acceleration * dt * dt;
    glm::vec3 newVel = m_velocity + m_acceleration * dt;
    m_position = newPos;
    m_velocity = newVel;
    glm::vec3 angularAcc = m_inertiaTensorInv * m_torque;
    m_angularVelocity += angularAcc * dt;
    glm::quat wQuat(0.0f, m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z);
    m_orientation = glm::normalize(m_orientation + 0.5f * wQuat * m_orientation * dt);
}

// Collision resolution: currently handles sphere-sphere (impulse)
void RigidBody::resolveCollision(RigidBody& other, float restitution) {
    // Only sphere-sphere is implemented here
    if (m_shapeType != ShapeType::SPHERE || other.m_shapeType != ShapeType::SPHERE) {
        return; // no-op for other shapes
    }
    // Positions and radii
    glm::vec3 posA = getPosition();
    glm::vec3 posB = other.getPosition();
    float rA = getBoundingRadius();
    float rB = other.getBoundingRadius();

    glm::vec3 normal = posB - posA;
    float dist = glm::length(normal);
    if (dist < 1e-6f) {
        return; // too close, no collision
    }
    normal /= dist;

    // Penetration depth
    float penetration = (rA + rB) - dist;
    if (penetration <= 0.0f) return;

    // Relative velocity at contact (including rotation)
    glm::vec3 vA = getVelocity();
    glm::vec3 vB = other.getVelocity();
    glm::vec3 rA_vec = normal * rA;
    glm::vec3 rB_vec = -normal * rB;
    glm::vec3 vA_contact = vA + glm::cross(m_angularVelocity, rA_vec);
    glm::vec3 vB_contact = vB + glm::cross(other.m_angularVelocity, rB_vec);
    glm::vec3 relVel = vB_contact - vA_contact;
    float velAlongNormal = glm::dot(relVel, normal);
    if (velAlongNormal > 0.0f) return; // moving apart

    float invMassA = (m_mass <= 0.0f) ? 0.0f : 1.0f / m_mass;
    float invMassB = (other.m_mass <= 0.0f) ? 0.0f : 1.0f / other.m_mass;

    glm::vec3 rA_cross_n = glm::cross(rA_vec, normal);
    glm::vec3 rB_cross_n = glm::cross(rB_vec, normal);
    float angTermA = glm::dot(normal, glm::cross(m_inertiaTensorInv * rA_cross_n, rA_vec));
    float angTermB = glm::dot(normal, glm::cross(other.m_inertiaTensorInv * rB_cross_n, rB_vec));

    float j = -(1.0f + restitution) * velAlongNormal;
    float denom = invMassA + invMassB + angTermA + angTermB;
    if (denom > 0.0f) j /= denom; else j = 0.0f;
    glm::vec3 impulse = j * normal;

    // Apply linear impulses
    setVelocity(vA - impulse * invMassA);
    other.setVelocity(vB + impulse * invMassB);

    // Apply angular impulses
    m_angularVelocity += m_inertiaTensorInv * glm::cross(rA_vec, -impulse);
    other.m_angularVelocity += other.m_inertiaTensorInv * glm::cross(rB_vec, impulse);

    // Friction (Coulomb)
    glm::vec3 tangent = relVel - (velAlongNormal * normal);
    float tangentLength = glm::length(tangent);
    if (tangentLength > 1e-6f) {
        tangent /= tangentLength;
        float velT = glm::dot(relVel, tangent);
        float jt = -velT / denom;
        float mu = std::sqrt(std::max(0.0f, m_friction) * std::max(0.0f, other.m_friction));
        float maxF = mu * fabs(j);
        jt = glm::clamp(jt, -maxF, maxF);
        glm::vec3 frictionImpulse = jt * tangent;
        setVelocity(getVelocity() - frictionImpulse * invMassA);
        other.setVelocity(other.getVelocity() + frictionImpulse * invMassB);
        m_angularVelocity += m_inertiaTensorInv * glm::cross(rA_vec, -frictionImpulse);
        other.m_angularVelocity += other.m_inertiaTensorInv * glm::cross(rB_vec, frictionImpulse);
    }

    // Positional correction
    const float percent = 0.6f; // correction strength
    const float slop = 0.001f;  // penetration allowance
    float corrMag = std::max(penetration - slop, 0.0f) / (invMassA + invMassB) * percent;
    glm::vec3 correction = corrMag * normal;
    if (invMassA > 0.0f) m_position -= correction * invMassA;
    if (invMassB > 0.0f) other.m_position += correction * invMassB;
}
