#include "RigidBody.h"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include "SceneNode.h"

// Constructor/Destructor
RigidBody::RigidBody()
	: m_position(0.0f), m_prevPosition(0.0f),
	m_velocity(0.0f), m_acceleration(0.0f),
	m_linearDamping(0.05f), m_angularDamping(0.05f), m_friction(0.5f),  // Better default damping
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
	}
	else {
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
	}
	else {
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
	}
	else {
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
	// Do NOT update scene node here - let interpolation handle visual updates
	// This prevents conflict between physics position and interpolated rendering position
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
	// Do NOT update scene node here - let interpolation handle visual updates
	// This prevents conflict between physics orientation and interpolated rendering orientation
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
	}
	else {
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
		I = glm::mat3(Ival);  // Creates diagonal matrix with Ival on diagonal
		break;
	}
	case ShapeType::BOX: {
		glm::vec3 s = m_halfExtents * 2.0f;  // full sizes
		float ix = (1.0f / 12.0f) * m_mass * (s.y * s.y + s.z * s.z);
		float iy = (1.0f / 12.0f) * m_mass * (s.x * s.x + s.z * s.z);
		float iz = (1.0f / 12.0f) * m_mass * (s.x * s.x + s.y * s.y);
		
		//Properly construct diagonal matrix
		m_inertiaTensor = glm::mat3(
			ix,  0.0f, 0.0f,
			0.0f, iy,  0.0f,
			0.0f, 0.0f, iz
		);
		
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
	}
	else {
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
	// Linear motion: Semi-implicit Euler (velocity first, then position)
	m_velocity += m_acceleration * dt;
	m_position += m_velocity * dt;

	// Angular motion: Properly use world-space inertia tensor
	if (m_mass > 0.0f && glm::length2(m_torque) > 1e-12f) {
		glm::mat3 R = glm::toMat3(m_orientation);
		glm::mat3 iInvWorld = R * m_inertiaTensorInv * glm::transpose(R);
		glm::vec3 angularAcc = iInvWorld * m_torque;

		m_angularVelocity += angularAcc * dt;
	}
	
	// Update orientation from angular velocity
	if (glm::length2(m_angularVelocity) > 1e-12f) {
		glm::quat wQuat(0.0f, m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z);
		m_orientation = glm::normalize(m_orientation + 0.5f * dt * wQuat * m_orientation);
	}
}

// RK2 integrator (Midpoint) 
void RigidBody::integrateRK2(float dt) {
	// Linear motion with constant acceleration
	glm::vec3 v0 = m_velocity;
	glm::vec3 p0 = m_position;
	glm::vec3 a = m_acceleration;

	// Midpoint method
	glm::vec3 v_half = v0 + a * (dt * 0.5f);
	glm::vec3 p_half = p0 + v0 * (dt * 0.5f);

	// Full step using midpoint values
	m_velocity = v0 + a * dt;
	m_position = p0 + v_half * dt;

	// Angular motion with proper world-space inertia
	if (m_mass > 0.0f && glm::length2(m_torque) > 1e-12f) {
		glm::vec3 w0 = m_angularVelocity;
		glm::quat q0 = m_orientation;

		glm::mat3 R = glm::toMat3(m_orientation);
		glm::mat3 iInvWorld = R * m_inertiaTensorInv * glm::transpose(R);
		glm::vec3 alpha = iInvWorld * m_torque;

		// Midpoint angular velocity
		glm::vec3 w_half = w0 + alpha * (dt * 0.5f);

		// Full step
		m_angularVelocity = w0 + alpha * dt;

		// Update orientation using midpoint angular velocity
		glm::quat wHalfQuat(0.0f, w_half.x, w_half.y, w_half.z);
		m_orientation = glm::normalize(q0 + 0.5f * dt * wHalfQuat * q0);
	} else if (glm::length2(m_angularVelocity) > 1e-12f) {
		// No torque, but still rotating
		glm::quat wQuat(0.0f, m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z);
		m_orientation = glm::normalize(m_orientation + 0.5f * dt * wQuat * m_orientation);
	}
}

// RK4 integrator 
void RigidBody::integrateRK4(float dt) {
	// For linear motion with constant acceleration (gravity)
	// RK4 simplifies since acceleration doesn't depend on velocity
	glm::vec3 v0 = m_velocity;
	glm::vec3 p0 = m_position;
	glm::vec3 a = m_acceleration;  // Constant (gravity)

	// Linear motion RK4 with constant acceleration
	glm::vec3 k1v = a;
	glm::vec3 k1p = v0;

	glm::vec3 k2v = a;  // Acceleration is constant
	glm::vec3 k2p = v0 + 0.5f * k1v * dt;

	glm::vec3 k3v = a;  // Acceleration is constant
	glm::vec3 k3p = v0 + 0.5f * k2v * dt;

	glm::vec3 k4v = a;  // Acceleration is constant
	glm::vec3 k4p = v0 + k3v * dt;

	m_velocity = v0 + (k1v + 2.0f * k2v + 2.0f * k3v + k4v) * (dt / 6.0f);
	m_position = p0 + (k1p + 2.0f * k2p + 2.0f * k3p + k4p) * (dt / 6.0f);

	// Angular motion - FIXED to properly use world-space inertia tensor
	if (m_mass > 0.0f && glm::length2(m_torque) > 1e-12f) {
		glm::vec3 w0 = m_angularVelocity;
		glm::quat q0 = m_orientation;

		// Compute world-space inertia tensor inverse
		glm::mat3 R = glm::toMat3(m_orientation);
		glm::mat3 iInvWorld = R * m_inertiaTensorInv * glm::transpose(R);

		// Angular acceleration from torque
		glm::vec3 alpha = iInvWorld * m_torque;

		// RK4 for angular velocity (constant torque)
		glm::vec3 k1w = alpha;
		glm::vec3 k2w = alpha;
		glm::vec3 k3w = alpha;
		glm::vec3 k4w = alpha;

		m_angularVelocity = w0 + (k1w + 2.0f * k2w + 2.0f * k3w + k4w) * (dt / 6.0f);

		// Update orientation using average angular velocity
		glm::vec3 avgOmega = w0 + 0.5f * alpha * dt;
		glm::quat wQuat(0.0f, avgOmega.x, avgOmega.y, avgOmega.z);
		m_orientation = glm::normalize(q0 + 0.5f * dt * wQuat * q0);
	} else if (glm::length2(m_angularVelocity) > 1e-12f) {
		// No torque, but still rotating
		glm::quat wQuat(0.0f, m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z);
		m_orientation = glm::normalize(m_orientation + 0.5f * dt * wQuat * m_orientation);
	}
}

// Verlet integrator 
void RigidBody::integrateVerlet(float dt) {
	// Velocity Verlet for better stability
	// v(t+dt) = v(t) + a(t)*dt
	// x(t+dt) = x(t) + v(t)*dt + 0.5*a(t)*dt^2
	glm::vec3 newPos = m_position + m_velocity * dt + 0.5f * m_acceleration * dt * dt;
	glm::vec3 newVel = m_velocity + m_acceleration * dt;
	m_position = newPos;
	m_velocity = newVel;

	// Angular motion with proper world-space inertia
	if (m_mass > 0.0f && glm::length2(m_torque) > 1e-12f) {
		glm::mat3 R = glm::toMat3(m_orientation);
		glm::mat3 iInvWorld = R * m_inertiaTensorInv * glm::transpose(R);
		glm::vec3 angularAcc = iInvWorld * m_torque;

		m_angularVelocity += angularAcc * dt;
	}
	
	// Update orientation from angular velocity
	if (glm::length2(m_angularVelocity) > 1e-12f) {
		glm::quat wQuat(0.0f, m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z);
		m_orientation = glm::normalize(m_orientation + 0.5f * dt * wQuat * m_orientation);
	}
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

	//Use world-space inertia tensors
	glm::mat3 RA = glm::toMat3(m_orientation);
	glm::mat3 RB = glm::toMat3(other.m_orientation);
	glm::mat3 iInvWorldA = RA * m_inertiaTensorInv * glm::transpose(RA);
	glm::mat3 iInvWorldB = RB * other.m_inertiaTensorInv * glm::transpose(RB);
	
	glm::vec3 rA_cross_n = glm::cross(rA_vec, normal);
	glm::vec3 rB_cross_n = glm::cross(rB_vec, normal);
	float angTermA = glm::dot(normal, glm::cross(iInvWorldA * rA_cross_n, rA_vec));
	float angTermB = glm::dot(normal, glm::cross(iInvWorldB * rB_cross_n, rB_vec));

	float j = -(1.0f + restitution) * velAlongNormal;
	float denom = invMassA + invMassB + angTermA + angTermB;
	if (denom > 0.0f) j /= denom; else j = 0.0f;
	glm::vec3 impulse = j * normal;

	// Apply linear impulses
	setVelocity(vA - impulse * invMassA);
	other.setVelocity(vB + impulse * invMassB);

	// Apply angular impulses with world-space inertia
	m_angularVelocity += iInvWorldA * glm::cross(rA_vec, -impulse);
	other.m_angularVelocity += iInvWorldB * glm::cross(rB_vec, impulse);

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
		m_angularVelocity += iInvWorldA * glm::cross(rA_vec, -frictionImpulse);
		other.m_angularVelocity += iInvWorldB * glm::cross(rB_vec, frictionImpulse);
	}

	// Positional correction
	const float percent = 0.8f; // Stronger correction for better stability
	const float slop = 0.01f;   // Slightly larger penetration allowance
	float corrMag = std::max(penetration - slop, 0.0f) / (invMassA + invMassB) * percent;
	glm::vec3 correction = corrMag * normal;
	if (invMassA > 0.0f) m_position -= correction * invMassA;
	if (invMassB > 0.0f) other.m_position += correction * invMassB;
}
