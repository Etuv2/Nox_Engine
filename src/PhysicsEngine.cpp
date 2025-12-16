#include "PhysicsEngine.h"
#include "PhysicsCollision.h"
#include "SceneNode.h"
#include "SceneGraph.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <cmath>
#include <sstream>

PhysicsEngine::PhysicsEngine(const SimulationConfig& config)
	: m_config(config)
	, m_gravity(config.gravityX, config.gravityY, config.gravityZ)
{
	m_bvh = std::make_unique<PhysicsBVH>();
}

PhysicsEngine::~PhysicsEngine() {
	Shutdown();
}

bool PhysicsEngine::Initialize() {
	m_bodies.clear();
	m_bodyIdToIndex.clear();
	m_bodyToProxy.clear();
	m_manifolds.clear();
	m_activeManifolds.clear();
	m_bvh->Clear();

	m_state = State::RUNNING;
	m_accumulator = 0.0f;
	m_nextBodyId = 1;

	return true;
}

void PhysicsEngine::Shutdown() {
	RemoveAllBodies();
	m_bvh->Clear();
	m_manifolds.clear();
	m_activeManifolds.clear();
	m_entityToBodyId.clear();
}

void PhysicsEngine::OnSceneLoaded(std::shared_ptr<SceneGraph> sceneGraph) {
	m_sceneGraph = sceneGraph;
	// Bodies are added via AddBody as nodes are created
}

void PhysicsEngine::OnSceneUnloaded() {
	RemoveAllBodies();
	m_sceneGraph.reset();
}

// ============== MAIN UPDATE ==============

void PhysicsEngine::Update(float deltaTime) {
	if (m_state == State::PAUSED) {
		return;
	}

	// Clamp frame time to prevent spiral of death
	deltaTime = std::min(deltaTime, m_config.maxAccumulatedTime);

	m_accumulator += deltaTime;

	// PRE-STEP SYNC: Push kinematic and edited bodies to physics
	// This must happen once per frame before any stepping
	PreStepSync();

	int substeps = 0;
	while (m_accumulator >= m_config.fixedDeltaTime && substeps < m_config.maxSubSteps) {
		Step(m_config.fixedDeltaTime);
		m_accumulator -= m_config.fixedDeltaTime;
		substeps++;
	}

	// POST-STEP SYNC: Pull dynamic bodies from physics
	// This happens after all substeps to get final state
	PostStepSync();

	// Calculate interpolation alpha for rendering
	m_alpha = m_accumulator / m_config.fixedDeltaTime;
}

void PhysicsEngine::SingleStep() {
	Step(m_config.fixedDeltaTime);
}

void PhysicsEngine::Step(float dt) {
	auto startTime = std::chrono::high_resolution_clock::now();

	// Store previous state for interpolation
	for (auto& body : m_bodies) {
		if (body && !body->isSleeping()) {
			body->storePreviousState();
		}
	}

	// 1. Integrate forces (gravity) - NO damping here
	IntegrateForces(dt);

	// 2. Broadphase
	auto broadphaseStart = std::chrono::high_resolution_clock::now();
	Broadphase();
	auto broadphaseEnd = std::chrono::high_resolution_clock::now();

	// 3. Narrowphase
	auto narrowphaseStart = std::chrono::high_resolution_clock::now();
	Narrowphase();
	auto narrowphaseEnd = std::chrono::high_resolution_clock::now();

	// 4. Prepare constraints (includes warm starting)
	PrepareConstraints(dt);

	// 5. Solve velocity constraints
	auto solverStart = std::chrono::high_resolution_clock::now();
	SolveVelocityConstraints();
	auto solverEnd = std::chrono::high_resolution_clock::now();

	// 6. Integrate velocities
	IntegrateVelocities(dt);

	// 7. Solve position constraints
	SolvePositionConstraints();

	// 8. Apply damping and velocity clamping AFTER solver to stabilize
	for (auto& body : m_bodies) {
		if (body && body->IsDynamic() && !body->isSleeping() && !body->isGizmoGrabbed()) {
			body->applyDamping(dt);

			// Clamp excessive velocities to prevent instability
			glm::vec3 vel = body->getVelocity();
			float velMag = glm::length(vel);

			// Dead-zone for micro-jitter prevention (match sleep thresholds)
			// Using slightly lower than sleep threshold to allow smooth approach to sleep
			if (velMag < m_config.sleepLinearThreshold * 0.5f) {
				body->setVelocity(glm::vec3(0.0f));
			}
			else if (velMag > 100.0f) { // Max 100 units/sec for safety
				body->setVelocity(vel * (100.0f / velMag));
			}

			glm::vec3 angVel = body->getAngularVelocity();
			float angVelMag = glm::length(angVel);

			// Dead-zone for micro-rotation jitter prevention
			if (angVelMag < m_config.sleepAngularThreshold * 0.5f) {
				body->setAngularVelocity(glm::vec3(0.0f));
			}
			else if (angVelMag > 50.0f) { // Max 50 rad/sec for safety
				body->setAngularVelocity(angVel * (50.0f / angVelMag));
			}
		}
	}

	// 9. Update BVH
	UpdateBVHProxies();

	// 10. Update sleep states
	UpdateSleepStates(dt);

	// 11. Clear forces
	for (auto& body : m_bodies) {
		if (body) {
			body->clearForces();
		}
	}

	// Update profiling
	m_profiling.broadphaseTimeMs = std::chrono::duration<float, std::milli>(broadphaseEnd - broadphaseStart).count();
	m_profiling.narrowphaseTimeMs = std::chrono::duration<float, std::milli>(narrowphaseEnd - narrowphaseStart).count();
	m_profiling.solverTimeMs = std::chrono::duration<float, std::milli>(solverEnd - solverStart).count();

	int sleeping = 0, active = 0;
	for (const auto& body : m_bodies) {
		if (body) {
			if (body->isSleeping()) sleeping++;
			else active++;
		}
	}
	m_profiling.activeBodyCount = active;
	m_profiling.sleepingBodyCount = sleeping;
}

// ============== STATE CONTROL ==============

void PhysicsEngine::Pause() {
	m_state = State::PAUSED;
}

void PhysicsEngine::Resume() {
	m_state = State::RUNNING;
	m_accumulator = 0.0f; // Reset accumulator on resume
}

void PhysicsEngine::Interpolate(float alpha) {
	m_alpha = alpha;
	// Interpolation is applied when reading from bodies for rendering
}

// ============== BODY MANAGEMENT ==============

PhysicsEngine::BodyID PhysicsEngine::AddBody(std::shared_ptr<RigidBody> body) {
	if (!body) return RigidBody::INVALID_BODY_ID;

	BodyID id = m_nextBodyId++;
	body->SetBodyID(id);

	// Add to bodies list
	size_t index = m_bodies.size();
	m_bodies.push_back(body);
	m_bodyIdToIndex[id] = index;

	// Compute initial AABB
	body->computeAABB();

	// Add to BVH
	PhysicsBVH::ProxyID proxyId = m_bvh->CreateProxy(body, m_config.fatAABBMargin);
	m_bodyToProxy[id] = proxyId;

	if (m_config.verboseLogging) {
		std::cout << "[PhysicsEngine] Added body " << id << " at position "
			<< body->getPosition().x << ", " << body->getPosition().y << ", " << body->getPosition().z
			<< std::endl;
	}

	return id;
}

void PhysicsEngine::RemoveBody(std::shared_ptr<RigidBody> body) {
	if (body) {
		RemoveBody(body->GetBodyID());
	}
}

void PhysicsEngine::RemoveBody(BodyID bodyId) {
	auto indexIt = m_bodyIdToIndex.find(bodyId);
	if (indexIt == m_bodyIdToIndex.end()) return;

	size_t index = indexIt->second;

	// Remove from BVH
	auto proxyIt = m_bodyToProxy.find(bodyId);
	if (proxyIt != m_bodyToProxy.end()) {
		m_bvh->DestroyProxy(proxyIt->second);
		m_bodyToProxy.erase(proxyIt);
	}

	// Remove manifolds involving this body
	for (auto it = m_manifolds.begin(); it != m_manifolds.end();) {
		auto& manifold = it->second;
		if ((manifold.bodyA && manifold.bodyA->GetBodyID() == bodyId) ||
			(manifold.bodyB && manifold.bodyB->GetBodyID() == bodyId)) {
			it = m_manifolds.erase(it);
		}
		else {
			++it;
		}
	}

	// Swap-and-pop removal from bodies vector
	if (index < m_bodies.size() - 1) {
		std::swap(m_bodies[index], m_bodies.back());
		BodyID swappedId = m_bodies[index]->GetBodyID();
		m_bodyIdToIndex[swappedId] = index;
	}
	m_bodies.pop_back();
	m_bodyIdToIndex.erase(bodyId);

	if (m_config.verboseLogging) {
		std::cout << "[PhysicsEngine] Removed body " << bodyId << std::endl;
	}
}

void PhysicsEngine::RemoveAllBodies() {
	for (auto& body : m_bodies) {
		if (body) {
			auto proxyIt = m_bodyToProxy.find(body->GetBodyID());
			if (proxyIt != m_bodyToProxy.end()) {
				m_bvh->DestroyProxy(proxyIt->second);
			}
		}
	}

	m_bodies.clear();
	m_bodyIdToIndex.clear();
	m_bodyToProxy.clear();
	m_manifolds.clear();
	m_activeManifolds.clear();
	m_entityToBodyId.clear();
	m_bvh->Clear();

	if (m_config.verboseLogging) {
		std::cout << "[PhysicsEngine] Removed all bodies" << std::endl;
	}
}

std::shared_ptr<RigidBody> PhysicsEngine::GetBody(BodyID bodyId) const {
	auto it = m_bodyIdToIndex.find(bodyId);
	if (it != m_bodyIdToIndex.end() && it->second < m_bodies.size()) {
		return m_bodies[it->second];
	}
	return nullptr;
}

// ============== SYNCHRONIZATION (Called from main update loop) ==============

void PhysicsEngine::PreStepSync() {
	// Push kinematic body transforms from ECS to physics (world space)
	for (auto& body : m_bodies) {
		if (!body) continue;
		if (body->isSleeping()) continue;

		// Skip gizmo-grabbed bodies - they're updated directly by UpdateGizmoTarget
		// which sets position/orientation directly
		if (body->isGizmoGrabbed()) {
			body->computeAABB();
			body->storePreviousState();
			continue;
		}

		// Sync kinematic bodies - scene is the source of truth
		if (body->IsKinematic()) {
			auto node = body->getAttachedNode();
			if (node) {
				glm::vec3 position = node->GetWorldPosition();
				glm::quat orientation = node->GetOrientation();

				// Update physics body with scene transform
				body->setPosition(position);
				body->setOrientation(orientation);
				body->setKinematicTarget(position, orientation);
				body->computeAABB();
			}
		}
	}
}

void PhysicsEngine::PostStepSync() {
	// Pull dynamic body transforms from physics to ECS
	// This runs after stepping, so transforms should be read from bodies
	// (not from interpolated state - that's for rendering)
	
	for (auto& body : m_bodies) {
		if (!body) continue;

		// Only sync dynamic bodies that are awake
		if (!body->IsDynamic() || body->isSleeping()) continue;

		// Skip gizmo-grabbed bodies (they're kinematic while grabbed)
		if (body->isGizmoGrabbed()) continue;

		auto node = body->getAttachedNode();
		if (!node) continue;

		// Mark that we're updating from physics to prevent feedback
		node->SetUpdatingFromPhysics(true);

		// Get physics body position and orientation (world space)
		glm::vec3 position = body->getPosition();
		glm::quat orientation = body->getOrientation();

		// Convert to local space if node has parent
		auto parent = node->parentNode.lock();
		if (parent) {
			glm::mat4 parentWorldTransform = parent->GetGlobalTransform(glm::mat4(1.0f));
			glm::mat4 parentInverse = glm::inverse(parentWorldTransform);
			glm::vec4 localPos = parentInverse * glm::vec4(position, 1.0f);
			position = glm::vec3(localPos);

			// Extract parent orientation and convert to local space
			glm::vec3 parentScale, parentTranslation, parentSkew;
			glm::vec4 parentPerspective;
			glm::quat parentWorldRot;
			glm::decompose(parentWorldTransform, parentScale, parentWorldRot,
				parentTranslation, parentSkew, parentPerspective);

			orientation = glm::inverse(parentWorldRot) * orientation;
		}

		// Update node transform with physics result
		glm::vec3 currentScale = node->GetScale();
		node->SetLocalTRS(position, orientation, currentScale);
		node->SyncToECS();

		node->SetUpdatingFromPhysics(false);
	}
}

// ============== SCENE SYNCHRONIZATION ==============

void PhysicsEngine::SyncFromSceneNodes() {
	auto syncStart = std::chrono::high_resolution_clock::now();

	for (auto& body : m_bodies) {
		if (!body) continue;

		auto node = body->getAttachedNode();
		if (!node) continue;

		// Use explicit ownership model to determine sync behavior
		// Only sync from scene for bodies that are NOT physics-owned
		bool shouldSync = false;

		switch (body->getTransformOwner()) {
		case RigidBody::TransformOwner::SCENE:
			// Kinematic bodies and sleeping dynamics are scene-owned
			shouldSync = true;
			break;

		case RigidBody::TransformOwner::EDITOR:
			// Editor is manipulating this body via gizmo
			shouldSync = true;
			break;

		case RigidBody::TransformOwner::PHYSICS:
			// Physics owns this body - don't sync FROM scene
			// (This prevents scene changes from fighting physics)
			shouldSync = false;
			break;
		}

		if (shouldSync) {
			glm::vec3 position = node->GetWorldPosition();
			glm::quat orientation = node->GetOrientation();

			if (body->hasKinematicTarget()) {
				// Use kinematic target for smooth motion
				body->setPosition(body->getKinematicTargetPosition());
				body->setOrientation(body->getKinematicTargetOrientation());
			}
			else {
				body->setPosition(position);
				body->setOrientation(orientation);
			}

			body->computeAABB();

			// Wake up scene-synced bodies only if kinematic or editor-grabbed
			if (body->IsKinematic() || body->isGizmoGrabbed()) {
				body->wakeUp();
			}
		}
	}

	auto syncEnd = std::chrono::high_resolution_clock::now();
	m_profiling.syncTimeMs = std::chrono::duration<float, std::milli>(syncEnd - syncStart).count();
}

void PhysicsEngine::SyncToSceneNodes() {
	for (auto& body : m_bodies) {
		if (!body) continue;

		// Only sync TO scene for physics-owned bodies
		// This prevents double-writes and ensures single source of truth
		if (!body->isPhysicsOwned()) continue;

		// Skip sleeping bodies (they're now scene-owned)
		if (body->isSleeping()) continue;

		auto node = body->getAttachedNode();
		if (!node) continue;

		// Mark that we're updating from physics
		node->SetUpdatingFromPhysics(true);

		// Get interpolated transform
		glm::vec3 position = body->getInterpolatedPosition(m_alpha);
		glm::quat orientation = body->getInterpolatedOrientation(m_alpha);

		// Update node transform
		// For proper hierarchy support, we need to convert to local space
		auto parent = node->parentNode.lock();
		if (parent) {
			// Compute parent's actual world transform
			glm::mat4 parentWorldTransform = parent->GetGlobalTransform(glm::mat4(1.0f));

			// Convert world position to local
			glm::mat4 parentInverse = glm::inverse(parentWorldTransform);
			glm::vec4 localPos = parentInverse * glm::vec4(position, 1.0f);
			position = glm::vec3(localPos);

			// Extract world orientation from parent's world transform
			glm::vec3 parentScale, parentTranslation, parentSkew;
			glm::vec4 parentPerspective;
			glm::quat parentWorldRot;
			glm::decompose(parentWorldTransform, parentScale, parentWorldRot,
				parentTranslation, parentSkew, parentPerspective);

			// Convert world orientation to local
			orientation = glm::inverse(parentWorldRot) * orientation;
		}

		// Use SetLocalTRS for proper transform composition
		glm::vec3 currentScale = node->GetScale();
		node->SetLocalTRS(position, orientation, currentScale);

		// Sync to ECS system
		node->SyncToECS();

		node->SetUpdatingFromPhysics(false);
	}
}

// ============== GIZMO SUPPORT ==============

void PhysicsEngine::BeginGizmoGrab(std::shared_ptr<RigidBody> body) {
	if (!body) return;

	// Mark body as being gizmo-grabbed
	body->setGizmoGrabbed(true);
	
	// Switch to EDITOR ownership during gizmo manipulation
	body->setTransformOwner(RigidBody::TransformOwner::EDITOR);
	
	// Wake up the body to ensure it processes updates
	body->wakeUp();

	// DO NOT use kinematic target - it bypasses force integration!
	// Instead, we'll directly update position while forces continue to apply
	
	// Store the starting position for reference (used by UpdateGizmoTarget)
	// Dampen current velocities to smooth transition
	body->setVelocity(body->getVelocity() * 0.5f);
	body->setAngularVelocity(body->getAngularVelocity() * 0.5f);

	std::cout << "[PhysicsEngine] Begin gizmo grab on body " << body->GetBodyID() 
		<< " - IsDynamic: " << body->IsDynamic() 
		<< " - Vel: (" << body->getVelocity().x << ", " << body->getVelocity().y << ", " << body->getVelocity().z << ")"
		<< " - IsSleeping: " << body->isSleeping() << std::endl;
}

void PhysicsEngine::UpdateGizmoTarget(std::shared_ptr<RigidBody> body,
	const glm::vec3& position,
	const glm::quat& orientation) {
	if (!body || !body->isGizmoGrabbed()) return;

	glm::vec3 currentVel = body->getVelocity();
	float currentVelMag = glm::length(currentVel);

	// We directly set position to follow gizmo for visual feedback
	// BUT we still need to allow velocity to accumulate from forces (gravity, etc)
	// so that when gizmo is released, object continues with realistic motion
	
	// Set position exactly where gizmo is
	body->setPosition(position);
	body->setOrientation(orientation);
	
	// Do NOT dampen velocities!
	// We want to preserve ALL velocity accumulated from forces (especially gravity)
	// The velocity accumulates from integrateForces() each step
	
	static int frameCounter = 0;
	frameCounter++;
	if (frameCounter % 10 == 0) {
		std::cout << "[UpdateGizmoTarget] Vel: (" << currentVel.x << ", " << currentVel.y << ", " << currentVel.z 
			<< ") Mag: " << currentVelMag << std::endl;
	}
}

void PhysicsEngine::EndGizmoGrab(std::shared_ptr<RigidBody> body) {

	glm::vec3 velBeforeEnd = body->getVelocity();
	float velMagBefore = glm::length(velBeforeEnd);

	// Clear gizmo state
	body->setGizmoGrabbed(false);
	body->clearKinematicTarget();

	// Restore transform ownership based on body type
	if (body->IsDynamic()) {
		// Dynamic bodies are physics-owned after grab
		body->setTransformOwner(RigidBody::TransformOwner::PHYSICS);
	} else if (body->IsKinematic()) {
		// Kinematic bodies are scene-owned
		body->setTransformOwner(RigidBody::TransformOwner::SCENE);
	} else {
		// Static bodies are scene-owned
		body->setTransformOwner(RigidBody::TransformOwner::SCENE);
	}

	// For dynamic bodies, prepare for normal physics simulation
	if (body->IsDynamic()) {
		// Velocities have been preserved and accumulated forces during manipulation
		// Let them continue naturally - the body will fall under gravity
		
		// Store current state as previous for smooth interpolation
		body->storePreviousState();

		// Recompute AABB at new position
		body->computeAABB();

		// Explicitly mark as NOT sleeping so gravity and forces apply immediately
		body->setSleeping(false);
	} else {
		// For kinematic and static bodies, just update AABB
		body->computeAABB();
	}

	glm::vec3 velAfterEnd = body->getVelocity();
	float velMagAfter = glm::length(velAfterEnd);
	
	std::cout << "[PhysicsEngine] End gizmo grab on body " << body->GetBodyID() 
		<< " - Vel Before: (" << velBeforeEnd.x << ", " << velBeforeEnd.y << ", " << velBeforeEnd.z << ") Mag: " << velMagBefore
		<< " - Vel After: (" << velAfterEnd.x << ", " << velAfterEnd.y << ", " << velAfterEnd.z << ") Mag: " << velMagAfter
		<< " - IsDynamic: " << body->IsDynamic() << " - IsSleeping: " << body->isSleeping() << std::endl;
}

// ============== QUERIES ==============

void PhysicsEngine::RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
	std::function<bool(std::shared_ptr<RigidBody>, float)> callback) {
	m_bvh->RayCast(origin, direction, maxDistance,
		[this, &callback](PhysicsBVH::ProxyID proxyId, float t) -> bool {
			const auto* proxy = m_bvh->GetProxy(proxyId);
			if (proxy) {
				auto body = proxy->body.lock();
				if (body) {
					return callback(body, t);
				}
			}
			return true;
		});
}

std::vector<std::shared_ptr<RigidBody>> PhysicsEngine::QueryAABB(const glm::vec3& min, const glm::vec3& max) {
	std::vector<std::shared_ptr<RigidBody>> results;

	m_bvh->Query(min, max, [this, &results](PhysicsBVH::ProxyID proxyId) -> bool {
		const auto* proxy = m_bvh->GetProxy(proxyId);
		if (proxy) {
			auto body = proxy->body.lock();
			if (body) {
				results.push_back(body);
			}
		}
		return true;
		});

	return results;
}

// ============== CONFIGURATION ==============

void PhysicsEngine::SetGravity(const glm::vec3& gravity) {
	m_gravity = gravity;
	m_config.gravityX = gravity.x;
	m_config.gravityY = gravity.y;
	m_config.gravityZ = gravity.z;
}

// ============== INTERNAL STEP ==============

void PhysicsEngine::IntegrateForces(float dt) {
	for (auto& body : m_bodies) {
		if (!body) continue;
		if (!body->IsDynamic()) continue;
		if (body->isSleeping()) continue;

		// We NEED to apply forces even during gizmo manipulation
		// so that gravity and external forces work correctly

		// Apply forces to all dynamic bodies, including gizmo-grabbed ones
		body->integrateForces(dt, m_gravity);
	}
}

void PhysicsEngine::Broadphase() {
	m_broadphasePairs.clear();
	m_bvh->QueryPairs(m_broadphasePairs);
	m_profiling.pairCount = static_cast<int>(m_broadphasePairs.size());
}

void PhysicsEngine::Narrowphase() {
	m_activeManifolds.clear();

	// Mark existing manifolds as not updated
	for (auto& [key, manifold] : m_manifolds) {
		manifold.lifetime++;
	}

	// Process broadphase pairs
	for (const auto& pair : m_broadphasePairs) {
		const auto* proxyA = m_bvh->GetProxy(pair.proxyA);
		const auto* proxyB = m_bvh->GetProxy(pair.proxyB);

		if (!proxyA || !proxyB) continue;

		auto bodyA = proxyA->body.lock();
		auto bodyB = proxyB->body.lock();

		if (!bodyA || !bodyB) continue;

		// Skip if both are sleeping
		if (bodyA->isSleeping() && bodyB->isSleeping()) continue;

		// Generate pair key
		uint64_t pairKey = ContactManifold::makePairKey(bodyA->GetBodyID(), bodyB->GetBodyID());

		// Get or create manifold
		auto& manifold = m_manifolds[pairKey];
		manifold.pairKey = pairKey;

		// Reset lifetime for active manifolds
		manifold.lifetime = 0;

		// Refresh existing contacts
		if (manifold.pointCount > 0) {
			manifold.refreshContacts(m_config.contactBreakingThreshold);
		}

		// Test collision
		ContactManifold newManifold;
		if (PhysicsCollision::TestCollision(bodyA, bodyB, newManifold)) {
			// Merge new contacts into existing manifold
			for (int i = 0; i < newManifold.pointCount; i++) {
				manifold.addPoint(newManifold.points[i]);
			}

			manifold.bodyA = bodyA;
			manifold.bodyB = bodyB;
			manifold.normal = newManifold.normal;
			manifold.computeTangentBasis();
			manifold.friction = newManifold.friction;
			manifold.restitution = newManifold.restitution;

			m_activeManifolds.push_back(&manifold);

			// Wake up bodies on contact
			bodyA->wakeUp();
			bodyB->wakeUp();
		}
	}

	// Remove stale manifolds
	for (auto it = m_manifolds.begin(); it != m_manifolds.end();) {
		if (it->second.pointCount == 0 || it->second.lifetime > 10) {
			it = m_manifolds.erase(it);
		}
		else {
			++it;
		}
	}

	m_profiling.contactCount = 0;
	for (const auto* manifold : m_activeManifolds) {
		m_profiling.contactCount += manifold->pointCount;
	}
}

void PhysicsEngine::PrepareConstraints(float dt) {
	float invDt = (dt > 0.0f) ? 1.0f / dt : 0.0f;

	for (auto* manifold : m_activeManifolds) {
		if (!manifold->bodyA || !manifold->bodyB) continue;

		auto& bodyA = manifold->bodyA;
		auto& bodyB = manifold->bodyB;

		for (int i = 0; i < manifold->pointCount; i++) {
			ContactPoint& cp = manifold->points[i];

			glm::vec3 rA = cp.point - bodyA->getPosition();
			glm::vec3 rB = cp.point - bodyB->getPosition();

			// Compute effective mass for normal constraint
			glm::vec3 rnA = glm::cross(rA, manifold->normal);
			glm::vec3 rnB = glm::cross(rB, manifold->normal);

			float kNormal = bodyA->getInverseMass() + bodyB->getInverseMass();
			kNormal += glm::dot(rnA, bodyA->getInverseInertiaWorld() * rnA);
			kNormal += glm::dot(rnB, bodyB->getInverseInertiaWorld() * rnB);
			cp.normalMass = (kNormal > 0.0f) ? 1.0f / kNormal : 0.0f;

			// Compute effective mass for tangent constraints
			glm::vec3 rt1A = glm::cross(rA, manifold->tangent1);
			glm::vec3 rt1B = glm::cross(rB, manifold->tangent1);
			float kTangent1 = bodyA->getInverseMass() + bodyB->getInverseMass();
			kTangent1 += glm::dot(rt1A, bodyA->getInverseInertiaWorld() * rt1A);
			kTangent1 += glm::dot(rt1B, bodyB->getInverseInertiaWorld() * rt1B);
			cp.tangentMass1 = (kTangent1 > 0.0f) ? 1.0f / kTangent1 : 0.0f;

			glm::vec3 rt2A = glm::cross(rA, manifold->tangent2);
			glm::vec3 rt2B = glm::cross(rB, manifold->tangent2);
			float kTangent2 = bodyA->getInverseMass() + bodyB->getInverseMass();
			kTangent2 += glm::dot(rt2A, bodyA->getInverseInertiaWorld() * rt2A);
			kTangent2 += glm::dot(rt2B, bodyB->getInverseInertiaWorld() * rt2B);
			cp.tangentMass2 = (kTangent2 > 0.0f) ? 1.0f / kTangent2 : 0.0f;

			// Compute velocity bias for position correction (Baumgarte)
			float penetration = cp.penetration;
			if (penetration > m_config.allowedPenetration) {
				cp.velocityBias = m_config.baumgarteFactor * invDt *
					(penetration - m_config.allowedPenetration);
			}
			else {
				cp.velocityBias = 0.0f;
			}

			// Add restitution bias with correct sign
			// Restitution should OPPOSE incoming velocity (make it more negative)
			glm::vec3 velA = bodyA->getVelocityAtPoint(cp.point);
			glm::vec3 velB = bodyB->getVelocityAtPoint(cp.point);
			float relVelNormal = glm::dot(velB - velA, manifold->normal);

			// Only apply restitution if objects are approaching (relVel < 0)
			if (relVelNormal < -m_config.restitutionThreshold) {
				// Subtract to oppose the incoming velocity
				cp.velocityBias -= manifold->restitution * relVelNormal;
			}
		}

		// Warm start with scaled impulses
		manifold->warmStart(m_config.warmStartingFactor);
	}
}

void PhysicsEngine::SolveVelocityConstraints() {
	for (int iter = 0; iter < m_config.velocityIterations; iter++) {
		for (auto* manifold : m_activeManifolds) {
			for (int i = 0; i < manifold->pointCount; i++) {
				SolveContactVelocity(*manifold, manifold->points[i]);
			}
		}
	}
}

void PhysicsEngine::SolveContactVelocity(ContactManifold& manifold, ContactPoint& cp) {
	auto& bodyA = manifold.bodyA;
	auto& bodyB = manifold.bodyB;

	if (!bodyA || !bodyB) return;

	glm::vec3 rA = cp.point - bodyA->getPosition();
	glm::vec3 rB = cp.point - bodyB->getPosition();

	// Relative velocity at contact
	glm::vec3 velA = bodyA->getVelocity() + glm::cross(bodyA->getAngularVelocity(), rA);
	glm::vec3 velB = bodyB->getVelocity() + glm::cross(bodyB->getAngularVelocity(), rB);
	glm::vec3 relVel = velB - velA;

	// Normal impulse
	float vn = glm::dot(relVel, manifold.normal);
	float lambda = cp.normalMass * (-vn + cp.velocityBias);

	// Clamp accumulated impulse
	float oldImpulse = cp.normalImpulseAccum;
	cp.normalImpulseAccum = std::max(oldImpulse + lambda, 0.0f);
	lambda = cp.normalImpulseAccum - oldImpulse;

	// Apply normal impulse
	glm::vec3 impulse = manifold.normal * lambda;
	bodyA->applyImpulseAtPoint(-impulse, cp.point);
	bodyB->applyImpulseAtPoint(impulse, cp.point);

	// Friction impulses
	float maxFriction = manifold.friction * cp.normalImpulseAccum;

	// Tangent 1
	{
		velA = bodyA->getVelocity() + glm::cross(bodyA->getAngularVelocity(), rA);
		velB = bodyB->getVelocity() + glm::cross(bodyB->getAngularVelocity(), rB);
		relVel = velB - velA;

		float vt = glm::dot(relVel, manifold.tangent1);
		float lambdaT = cp.tangentMass1 * (-vt);

		float oldTangentImpulse = cp.tangentImpulseAccum1;
		cp.tangentImpulseAccum1 = glm::clamp(oldTangentImpulse + lambdaT, -maxFriction, maxFriction);
		lambdaT = cp.tangentImpulseAccum1 - oldTangentImpulse;

		impulse = manifold.tangent1 * lambdaT;
		bodyA->applyImpulseAtPoint(-impulse, cp.point);
		bodyB->applyImpulseAtPoint(impulse, cp.point);
	}

	// Tangent 2
	{
		velA = bodyA->getVelocity() + glm::cross(bodyA->getAngularVelocity(), rA);
		velB = bodyB->getVelocity() + glm::cross(bodyB->getAngularVelocity(), rB);
		relVel = velB - velA;

		float vt = glm::dot(relVel, manifold.tangent2);
		float lambdaT = cp.tangentMass2 * (-vt);

		float oldTangentImpulse = cp.tangentImpulseAccum2;
		cp.tangentImpulseAccum2 = glm::clamp(oldTangentImpulse + lambdaT, -maxFriction, maxFriction);
		lambdaT = cp.tangentImpulseAccum2 - oldTangentImpulse;

		impulse = manifold.tangent2 * lambdaT;
		bodyA->applyImpulseAtPoint(-impulse, cp.point);
		bodyB->applyImpulseAtPoint(impulse, cp.point);
	}
}

void PhysicsEngine::IntegrateVelocities(float dt) {
	for (auto& body : m_bodies) {
		if (!body) continue;
		if (body->IsStatic()) continue;
		if (body->isSleeping()) continue;

		body->integrateVelocities(dt);
	}
}

void PhysicsEngine::SolvePositionConstraints() {
	for (int iter = 0; iter < m_config.positionIterations; iter++) {
		for (auto* manifold : m_activeManifolds) {
			for (int i = 0; i < manifold->pointCount; i++) {
				SolveContactPosition(*manifold, manifold->points[i]);
			}
		}
	}
}

bool PhysicsEngine::SolveContactPosition(ContactManifold& manifold, ContactPoint& cp) {
	auto& bodyA = manifold.bodyA;
	auto& bodyB = manifold.bodyB;

	if (!bodyA || !bodyB) return true;

	// Use stored penetration depth directly
	// Don't recompute from local points which can drift
	// The penetration was computed accurately during narrowphase
	float penetration = cp.penetration;

	// Apply slop (allowed penetration) - objects can overlap by this much without correction
	float separation = m_config.allowedPenetration - penetration;

	// If separation >= 0, we're within allowed slop, no correction needed
	if (separation >= 0.0f) {
		return true;
	}

	// Recompute contact point for accurate constraint application
	glm::vec3 worldPointA = bodyA->localToWorld(cp.localPointA);
	glm::vec3 worldPointB = bodyB->localToWorld(cp.localPointB);
	glm::vec3 contactPoint = (worldPointA + worldPointB) * 0.5f;

	// Compute correction impulse
	glm::vec3 rA = contactPoint - bodyA->getPosition();
	glm::vec3 rB = contactPoint - bodyB->getPosition();

	glm::vec3 rnA = glm::cross(rA, manifold.normal);
	glm::vec3 rnB = glm::cross(rB, manifold.normal);

	float k = bodyA->getInverseMass() + bodyB->getInverseMass();
	k += glm::dot(rnA, bodyA->getInverseInertiaWorld() * rnA);
	k += glm::dot(rnB, bodyB->getInverseInertiaWorld() * rnB);

	if (k <= 0.0f) return true;

	// Compute position correction
	// separation is negative, so -separation is positive (the amount to correct)
	float correction = -separation / k;

	// Apply Baumgarte factor to prevent overcorrection
	correction *= m_config.baumgarteFactor;

	// Clamp correction to prevent explosive responses
	const float maxCorrection = 0.2f;  // Max 0.2 units per iteration
	correction = std::min(correction, maxCorrection);

	// Only correct if positive (separating direction)
	if (correction <= 0.0f) return true;

	glm::vec3 P = manifold.normal * correction;

	// Apply position correction (linear only for stability)
	if (bodyA->IsDynamic() && !bodyA->isGizmoGrabbed()) {
		glm::vec3 posA = bodyA->getPosition() - P * bodyA->getInverseMass();
		bodyA->setPosition(posA);
	}

	if (bodyB->IsDynamic() && !bodyB->isGizmoGrabbed()) {
		glm::vec3 posB = bodyB->getPosition() + P * bodyB->getInverseMass();
		bodyB->setPosition(posB);
	}

	// Return true if separation is resolved enough
	return separation > -0.005f;
}

void PhysicsEngine::UpdateBVHProxies() {
	for (auto& body : m_bodies) {
		if (!body) continue;
		if (body->IsStatic()) continue;
		if (body->isSleeping()) continue;

		body->computeAABB();

		auto proxyIt = m_bodyToProxy.find(body->GetBodyID());
		if (proxyIt != m_bodyToProxy.end()) {
			m_bvh->UpdateProxy(proxyIt->second,
				body->getAABBMin(),
				body->getAABBMax(),
				m_config.fatAABBMargin);
		}
	}
}

void PhysicsEngine::UpdateSleepStates(float dt) {
	if (!m_config.enableSleeping) return;

	for (auto& body : m_bodies) {
		if (!body) continue;
		if (!body->IsDynamic()) continue;
		if (body->isGizmoGrabbed()) continue;

		body->updateSleepState(m_config.sleepLinearThreshold,
			m_config.sleepAngularThreshold,
			dt);

		if (body->getSleepTime() >= m_config.sleepTimeThreshold) {
			body->setSleeping(true);
		}
	}
}

// ============== DEBUG INSTRUMENTATION ==============

std::string PhysicsEngine::GetContactDebugInfo() const {
	std::ostringstream oss;
	
	oss << "=== CONTACT DEBUG INFO ===\n";
	oss << "Active manifolds: " << m_activeManifolds.size() << "\n";
	oss << "Total manifolds: " << m_manifolds.size() << "\n";
	oss << "Total contacts: " << m_profiling.contactCount << "\n\n";
	
	int manifoldIdx = 0;
	for (const auto* manifold : m_activeManifolds) {
		if (!manifold || !manifold->bodyA || !manifold->bodyB) continue;
		
		oss << "Manifold " << manifoldIdx << ":\n";
		oss << "  BodyA ID: " << manifold->bodyA->GetBodyID() << "\n";
		oss << "  BodyB ID: " << manifold->bodyB->GetBodyID() << "\n";
		oss << "  Contact count: " << manifold->pointCount << "\n";
		oss << "  Normal: [" << manifold->normal.x << ", " << manifold->normal.y 
			<< ", " << manifold->normal.z << "]\n";
		oss << "  Friction: " << manifold->friction << "\n";
		oss << "  Restitution: " << manifold->restitution << "\n";
		
		for (int i = 0; i < manifold->pointCount; i++) {
			const auto& cp = manifold->points[i];
			oss << "    Contact " << i << ":\n";
			oss << "      Penetration: " << cp.penetration << "\n";
			oss << "      Normal impulse: " << cp.normalImpulseAccum << "\n";
			oss << "      Position: [" << cp.point.x << ", " << cp.point.y 
				<< ", " << cp.point.z << "]\n";
		}
		oss << "\n";
		manifoldIdx++;
	}
	
	return oss.str();
}

std::vector<PhysicsEngine::BodyID> PhysicsEngine::GetHoveringBodies() const {
	std::vector<BodyID> hovering;
	
	// A body is considered "hovering" if it's in contact but has minimal motion
	// and is being corrected repeatedly (sign of excessive contact margin)
	
	for (const auto& body : m_bodies) {
		if (!body || body->IsStatic() || body->isSleeping()) continue;
		
		// Check if body is in contact
		bool isInContact = false;
		for (const auto* manifold : m_activeManifolds) {
			if (manifold && ((manifold->bodyA.get() == body.get()) || 
			                  (manifold->bodyB.get() == body.get()))) {
				isInContact = true;
				break;
			}
		}
		
		if (!isInContact) continue;
		
		// Check for minimal velocity (hovering characteristics)
		float velMag = glm::length(body->getVelocity());
		float angVelMag = glm::length(body->getAngularVelocity());
		
		// If in contact but velocity is very low, might be hovering
		// (this is a simple heuristic)
		if (velMag < 0.01f && angVelMag < 0.01f) {
			hovering.push_back(body->GetBodyID());
		}
	}
	
	return hovering;
}
