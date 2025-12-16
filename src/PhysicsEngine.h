#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>

#include "SimulationConfig.h"
#include "RigidBody.h"
#include "Contact.h"
#include "PhysicsBVH.h"
#include "ComponentTypes.h"

// Forward declarations
class SceneNode;
class SceneGraph;
class ComponentManager;

/**
 * PhysicsEngine - Production-ready rigid body physics simulation
 * 
 * Features:
 * - Fixed timestep simulation with accumulator
 * - Dynamic BVH broadphase
 * - Contact manifold narrowphase
 * - Impulse-based constraint solver
 * - Warm starting for stable stacks
 * - Positional correction (Baumgarte)
 * - Sleep management
 * - Scene node synchronization
 * - Gizmo manipulation support
 * - Pause/resume control
 */
class PhysicsEngine {
public:
    using BodyID = RigidBody::BodyID;
    
    /**
     * Engine state
     */
    enum class State {
        RUNNING,
        PAUSED,
        STEPPING   // Single step mode
    };

public:
    /**
     * Constructor with configuration
     */
    explicit PhysicsEngine(const SimulationConfig& config = SimulationConfig());
    ~PhysicsEngine();
    
    // ============== LIFECYCLE ==============
    
    /**
     * Initialize the physics engine
     */
    bool Initialize();
    
    /**
     * Shutdown and cleanup
     */
    void Shutdown();
    
    /**
     * Called when a scene is loaded
     */
    void OnSceneLoaded(std::shared_ptr<SceneGraph> sceneGraph);
    
    /**
     * Called when a scene is unloaded
     */
    void OnSceneUnloaded();
    
    // ============== MAIN UPDATE ==============
    
    /**
     * Update physics simulation
     * Uses fixed timestep with accumulator
     * @param deltaTime Frame time in seconds
     */
    void Update(float deltaTime);
    
    /**
     * Step physics by exactly one fixed timestep
     * Used for single-step debugging
     */
    void SingleStep();
    
    // ============== STATE CONTROL ==============
    
    /**
     * Pause simulation
     */
    void Pause();
    
    /**
     * Resume simulation
     */
    void Resume();
    
    /**
     * Check if paused
     */
    bool IsPaused() const { return m_state == State::PAUSED; }
    
    /**
     * Get current state
     */
    State GetState() const { return m_state; }
    
    /**
     * Get interpolation alpha for rendering
     */
    float GetAlpha() const { return m_alpha; }
    
    /**
     * Interpolate all bodies for rendering
     */
    void Interpolate(float alpha);
    
    // ============== BODY MANAGEMENT ==============
    
    /**
     * Add a rigid body to the simulation
     * @param body Rigid body to add
     * @return Body ID assigned
     */
    BodyID AddBody(std::shared_ptr<RigidBody> body);
    
    /**
     * Remove a rigid body from the simulation
     * @param body Body to remove
     */
    void RemoveBody(std::shared_ptr<RigidBody> body);
    
    /**
     * Remove a body by ID
     */
    void RemoveBody(BodyID bodyId);
    
    /**
     * Remove all bodies
     */
    void RemoveAllBodies();
    
    /**
     * Get body by ID
     */
    std::shared_ptr<RigidBody> GetBody(BodyID bodyId) const;
    
    /**
     * Get all bodies
     */
    const std::vector<std::shared_ptr<RigidBody>>& GetBodies() const { return m_bodies; }
    
    /**
     * Get body count
     */
    size_t GetBodyCount() const { return m_bodies.size(); }
    
    // ============== ECS INTEGRATION ==============
    
    /**
     * Create a physics body from ECS components
     * Links RigidBody to ECS via stable BodyID
     * @param entityID ECS entity ID
     * @param bodyType Static/Dynamic/Kinematic
     * @param colliderComp Collider component data
     * @param position World position
     * @param orientation World orientation
     * @return True if created successfully
     */
    bool CreatePhysicsBody(EntityID entityID, 
                          uint32_t bodyType,
                          const struct ColliderComponent& colliderComp,
                          const glm::vec3& position,
                          const glm::quat& orientation);
    
    /**
     * Destroy physics body associated with entity
     * @param entityID ECS entity ID
     * @param bodyID Physics body ID to verify
     * @return True if destroyed
     */
    bool DestroyPhysicsBody(EntityID entityID, BodyID bodyID);
    
    /**
     * Get physics body for ECS entity
     * Look up by bodyID from ECS component (do not search)
     */
    std::shared_ptr<RigidBody> GetPhysicsBodyForEntity(EntityID entityID) const;
    
    // ============== SYNCHRONIZATION (Called from main update loop) ==============
    
    /**
     * Synchronize before stepping physics
     * - Push kinematic body transforms from ECS to physics (world space)
     * - Update moved static colliders and broadphase proxies
     * - Clear previous state for gizmo-grabbed bodies
     * 
     * Must be called before Update() during fixed timestep
     */
    void PreStepSync();
    
    /**
     * Synchronize after stepping physics
     * - Pull dynamic body transforms from physics to ECS
     * - Convert world-space results to local-space for hierarchy
     * - Only sync if body still has both ECS components
     * 
     * Must be called after stepping, inside Update()
     */
    void PostStepSync();
    
    // ============== SCENE SYNCHRONIZATION ==============
    
    /**
     * Sync transforms from scene nodes to physics bodies
     * Called before simulation step for kinematic/edited objects
     * DEPRECATED: Use PreStepSync instead
     */
    void SyncFromSceneNodes();
    
    /**
     * Sync transforms from physics bodies to scene nodes
     * Called after simulation step for dynamic objects
     * DEPRECATED: Use PostStepSync instead
     */
    void SyncToSceneNodes();
    
    // ============== GIZMO SUPPORT ==============
    
    /**
     * Notify that a body is being grabbed by gizmo
     */
    void BeginGizmoGrab(std::shared_ptr<RigidBody> body);
    
    /**
     * Update gizmo target for a body
     */
    void UpdateGizmoTarget(std::shared_ptr<RigidBody> body, 
                           const glm::vec3& position, 
                           const glm::quat& orientation);
    
    /**
     * End gizmo grab
     */
    void EndGizmoGrab(std::shared_ptr<RigidBody> body);
    
    // ============== QUERIES ==============
    
    /**
     * Ray cast against physics world
     * @param origin Ray origin
     * @param direction Ray direction (normalized)
     * @param maxDistance Maximum distance
     * @param callback Called for each hit body (return false to stop)
     */
    void RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                 std::function<bool(std::shared_ptr<RigidBody>, float)> callback);
    
    /**
     * Query bodies overlapping an AABB
     */
    std::vector<std::shared_ptr<RigidBody>> QueryAABB(const glm::vec3& min, const glm::vec3& max);
    
    // ============== CONFIGURATION ==============
    
    /**
     * Get/Set configuration
     */
    const SimulationConfig& GetConfig() const { return m_config; }
    void SetConfig(const SimulationConfig& config) { m_config = config; }
    
    /**
     * Set gravity
     */
    void SetGravity(const glm::vec3& gravity);
    glm::vec3 GetGravity() const { return m_gravity; }
    
    // ============== DEBUG/PROFILING ==============
    
    struct ProfilingData {
        float broadphaseTimeMs = 0.0f;
        float narrowphaseTimeMs = 0.0f;
        float solverTimeMs = 0.0f;
        float syncTimeMs = 0.0f;
        int pairCount = 0;
        int contactCount = 0;
        int activeBodyCount = 0;
        int sleepingBodyCount = 0;
    };
    
    const ProfilingData& GetProfilingData() const { return m_profiling; }
    
    /**
     * Enable/disable verbose logging
     */
    void SetVerboseLogging(bool enabled) { m_config.verboseLogging = enabled; }
    
    // ============== DEBUG VISUALIZATION ==============
    
    /**
     * Get active contact manifolds for debug visualization
     * @return Vector of pointers to active manifolds (read-only)
     */
    const std::vector<ContactManifold*>& GetActiveManifolds() const { return m_activeManifolds; }
    
    /**
     * Enable/disable contact debug drawing
     */
    void SetDebugDrawContacts(bool enabled) { m_config.debugDrawContacts = enabled; }
    bool IsDebugDrawContactsEnabled() const { return m_config.debugDrawContacts; }
    
    // ============== DEBUG INSTRUMENTATION ==============
    
    /**
     * Get verbose debug info about active contacts
     * Used for diagnosing collision response issues
     * @return String with contact manifold and impulse information
     */
    std::string GetContactDebugInfo() const;
    
    /**
     * Check if any bodies are exhibiting hover behavior (position oscillation)
     * @return Vector of BodyIDs that appear to be hovering
     */
    std::vector<BodyID> GetHoveringBodies() const;

private:
    // ============== INTERNAL STEP ==============
    
    /**
     * Perform one physics step at fixed timestep
     */
    void Step(float dt);
    
    /**
     * Integrate forces (gravity, applied forces)
     */
    void IntegrateForces(float dt);
    
    /**
     * Broadphase collision detection
     */
    void Broadphase();
    
    /**
     * Narrowphase collision detection
     */
    void Narrowphase();
    
    /**
     * Solve velocity constraints
     */
    void SolveVelocityConstraints();
    
    /**
     * Integrate velocities to positions
     */
    void IntegrateVelocities(float dt);
    
    /**
     * Solve position constraints (Baumgarte correction)
     */
    void SolvePositionConstraints();
    
    /**
     * Update BVH proxies
     */
    void UpdateBVHProxies();
    
    /**
     * Update sleep states
     */
    void UpdateSleepStates(float dt);
    
    /**
     * Prepare constraints for solving
     */
    void PrepareConstraints(float dt);
    
    /**
     * Solve single contact velocity constraint
     */
    void SolveContactVelocity(ContactManifold& manifold, ContactPoint& cp);
    
    /**
     * Solve single contact position constraint
     */
    bool SolveContactPosition(ContactManifold& manifold, ContactPoint& cp);

private:
    // Configuration
    SimulationConfig m_config;
    glm::vec3 m_gravity;
    
    // State
    State m_state = State::RUNNING;
    float m_accumulator = 0.0f;
    float m_alpha = 0.0f;
    
    // Bodies
    std::vector<std::shared_ptr<RigidBody>> m_bodies;
    std::unordered_map<BodyID, size_t> m_bodyIdToIndex;
    BodyID m_nextBodyId = 1;
    
    // ECS integration: EntityID -> BodyID mapping for quick lookup
    // Used to find physics body from ECS entity without searching
    std::unordered_map<EntityID, BodyID> m_entityToBodyId;
    
    // Broadphase
    std::unique_ptr<PhysicsBVH> m_bvh;
    std::unordered_map<BodyID, PhysicsBVH::ProxyID> m_bodyToProxy;
    std::vector<PhysicsBVH::Pair> m_broadphasePairs;
    
    // Narrowphase
    std::unordered_map<uint64_t, ContactManifold> m_manifolds;
    std::vector<ContactManifold*> m_activeManifolds;
    
    // Profiling
    ProfilingData m_profiling;
    
    // Scene reference
    std::weak_ptr<SceneGraph> m_sceneGraph;
};
