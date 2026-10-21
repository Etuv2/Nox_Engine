#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <utility>

#include "SimulationConfig.h"
#include "RigidBody.h"
#include "Contact.h"
#include "PhysicsBVH.h"
#include "ComponentTypes.h"
#include "physics/CharacterSystem.h"
#include "physics/PhysicsDebugView.h"
#include "physics/TriggerSystem.h"

// Forward declarations
class SceneNode;
class SceneGraph;
class ComponentManager;

/**
 * A body-body contact that started (BEGIN) or stopped (END) touching since the previous physics step.
 * Collected across the substeps of one Update and handed out by TakeContactEvents().
 */
struct PhysicsContactEvent {
    enum class Type { BEGIN, END };
    Type type = Type::BEGIN;
    std::shared_ptr<RigidBody> bodyA;
    std::shared_ptr<RigidBody> bodyB;
};

/**
 * Closest ray hit against the physics world (PhysicsEngine::RayCastClosest).
 */
struct PhysicsRayHit {
    bool hit = false;
    std::shared_ptr<RigidBody> body;
    float distance = 0.0f;
    glm::vec3 point{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
};

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

    /**
     * Called once per actual fixed step (in Update's substep loop and by SingleStep), immediately
     * before that step integrates. Frames that accumulate less than one step make no call; a long
     * frame makes up to maxSubSteps calls. index counts substeps within the current Update (0 for
     * SingleStep). Used by gameplay runtimes for fixed_update; the callback must not add/remove bodies.
     */
    using SubstepCallback = std::function<void(float fixedDt, int index)>;
    void SetSubstepCallback(SubstepCallback callback) { m_substepCallback = std::move(callback); }
    
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
    float GetFixedDeltaTime() const { return m_config.fixedDeltaTime; }
    
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

    /**
     * Closest body hit by a ray within maxDistance. Only bodies whose collision layer intersects mask are
     * considered. Shapes are tested exactly (sphere, box, plane); the direction need not be normalized.
     * Returns false (hit.hit == false) when nothing is hit.
     */
    bool RayCastClosest(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                        uint32_t mask, PhysicsRayHit& hit) const;

    /**
     * Bodies whose shape overlaps the given sphere / oriented box, filtered by collision layer against mask
     * (body layer & mask != 0). Static, dynamic, kinematic and plane bodies are all reported.
     */
    std::vector<BodyID> OverlapSphere(const glm::vec3& center, float radius, uint32_t mask = 0xFFFFFFFFu) const;
    std::vector<BodyID> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                   uint32_t mask = 0xFFFFFFFFu) const;

    /**
     * Body <-> entity lookup through the body's attached scene node. INVALID_ENTITY / INVALID_BODY_ID when none.
     */
    EntityID GetEntityForBody(BodyID bodyId) const;
    BodyID GetBodyForEntity(EntityID entityId) const;

    /**
     * Contact begin/end events produced since the last call (in step order, sorted by pair within a step).
     */
    std::vector<PhysicsContactEvent> TakeContactEvents();

    /**
     * Trigger volumes and character controllers (components on scene nodes). Driven by Update().
     */
    TriggerSystem& Triggers() { return *m_triggers; }
    const TriggerSystem& Triggers() const { return *m_triggers; }
    CharacterSystem& Characters() { return *m_characters; }
    const CharacterSystem& Characters() const { return *m_characters; }

    /**
     * Static level geometry that characters collide with, in addition to the bodies (not owned here). Pass nullptr to
     * clear. The collision world must stay alive while it is set. Used by the character controller and by queries
     * that ask for world triangles.
     */
    void SetWorldCollision(const CollisionWorld* world) { m_worldCollision = world; }
    const CollisionWorld* WorldCollision() const { return m_worldCollision; }
    
    // ============== CONFIGURATION ==============
    
    /**
     * Configuration. Three copies:
     * - GetConfig(): runtime values the solver uses (scripts may change them with SetRuntimeConfig).
     * - GetAuthoredConfig(): the editor/scene values; the scene "physics" block is written from these, so runtime
     *   script tweaks never leak into a saved scene.
     * - GetDefaultConfig(): config/config.json values; a scene block holds only what differs from these.
     * SetConfig changes both the authored and the runtime values (editor edits, undo, scene load).
     */
    const SimulationConfig& GetConfig() const { return m_config; }
    const SimulationConfig& GetAuthoredConfig() const { return m_authoredConfig; }
    const SimulationConfig& GetDefaultConfig() const { return m_defaultConfig; }
    void SetConfig(const SimulationConfig& config);
    void SetRuntimeConfig(const SimulationConfig& config);
    void ResetConfigToDefaults() { SetConfig(m_defaultConfig); }

    /**
     * Recomputes a body's bounds and moves its broadphase proxy. Call after changing a body's shape, mesh or
     * position outside the simulation: static bodies are not re-tested each step (see UpdateBVHProxies).
     */
    void RefreshBodyBounds(RigidBody& body);

    /**
     * Set gravity
     */
    void SetGravity(const glm::vec3& gravity);
    glm::vec3 GetGravity() const { return m_gravity; }

    /**
     * Editor view toggles for the physics debug overlay (drawn by Core in the Scene viewport; not saved).
     */
    PhysicsDebugView& DebugView() { return m_debugView; }
    const PhysicsDebugView& DebugView() const { return m_debugView; }

    /**
     * Trigger / character entries for read-only iteration (debug overlay, stats).
     */
    template <typename F> void ForEachTrigger(F&& f) const { m_triggers->ForEach(std::forward<F>(f)); }
    template <typename F> void ForEachCharacter(F&& f) const { m_characters->ForEach(std::forward<F>(f)); }
    
    // ============== DEBUG/PROFILING ==============
    
    struct ProfilingData {
        float broadphaseTimeMs = 0.0f;
        float narrowphaseTimeMs = 0.0f;
        float solverTimeMs = 0.0f;
        float syncTimeMs = 0.0f;
        int pairCount = 0;
        int contactCount = 0;
        int activeManifoldCount = 0;
        int manifoldsCreated = 0;
        int manifoldsUpdated = 0;
        int bodiesWokenByContacts = 0;
        int activeBodyCount = 0;
        int sleepingBodyCount = 0;
        float maxPenetration = 0.0f;
        float averagePenetration = 0.0f;
        float stepTimeMs = 0.0f;   // wall time of the last Update's physics steps (sync, substeps, triggers, characters)
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

    /**
     * Bodies overlapping a shape-only probe body (used by OverlapSphere / OverlapBox)
     */
    std::vector<BodyID> CollectOverlaps(const std::shared_ptr<RigidBody>& probe, uint32_t mask) const;

private:
    // Configuration (see GetConfig / GetAuthoredConfig / GetDefaultConfig)
    SimulationConfig m_config;
    SimulationConfig m_authoredConfig;
    SimulationConfig m_defaultConfig;
    PhysicsDebugView m_debugView;
    glm::vec3 m_gravity;
    
    // State
    State m_state = State::RUNNING;
    float m_accumulator = 0.0f;
    float m_alpha = 0.0f;
    SubstepCallback m_substepCallback;
    
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
    
    // Contact begin/end: touching pairs after the last step, and the events not yet taken
    struct ContactPairState {
        std::shared_ptr<RigidBody> bodyA;
        std::shared_ptr<RigidBody> bodyB;
    };
    std::unordered_map<uint64_t, ContactPairState> m_contactPairs;
    std::vector<PhysicsContactEvent> m_contactEvents;

    // Triggers and character controllers (node-keyed component state)
    std::unique_ptr<TriggerSystem> m_triggers;
    std::unique_ptr<CharacterSystem> m_characters;
    const CollisionWorld* m_worldCollision = nullptr;   // non-owning

    // Profiling
    ProfilingData m_profiling;
    
    // Scene reference
    std::weak_ptr<SceneGraph> m_sceneGraph;
};
