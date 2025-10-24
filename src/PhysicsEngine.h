#pragma once
#include <vector>
#include <memory>
#include "SimulationConfig.h"
#include "RigidBody.h"
#include "BVH.h"

struct OBB {
    glm::vec3 pos;
    glm::vec3 axis[3];
    glm::vec3 halfSize;
};

// PhysicsEngine handles the collection of rigid bodies and simulation stepping
class PhysicsEngine {
public:
    PhysicsEngine(const SimulationConfig& cfg);
    void Shutdown();               // cleanup

    void AddBody(const std::shared_ptr<RigidBody>& body);
    void RemoveBody(const std::shared_ptr<RigidBody>& body);
    void RemoveAllBodies();
    void Update(float frameTime);
    void Interpolate(float alpha);

    const SimulationConfig& GetConfig() const { return m_config; }
    float GetAlpha() const { return m_accumulator / m_config.timeStep; }

    // Pause control
    void Pause() { m_paused = true; }
    void Resume() { m_paused = false; }
    void TogglePause() { m_paused = !m_paused; }
    bool IsPaused() const { return m_paused; }

    // BVH controls for performance tuning
    void SetUseBVH(bool useBVH) { m_useBVH = useBVH; }
    bool IsUsingBVH() const { return m_useBVH; }

    // Performance statistics
    struct PerformanceStats {
        size_t totalBodies = 0;
        size_t potentialPairs = 0;
        size_t bruteForceWouldBe = 0;
        float reductionPercentage = 0.0f;
        BVH::BVHTree<std::shared_ptr<RigidBody>>::Statistics bvhStats;
    };
    PerformanceStats GetPerformanceStats() const;

private:
    void step();                  // advance simulation by one fixed step
    void integrateRange(int startIndex, int endIndex);  // for multithreading
    
    // Broad-phase collision detection using BVH
    void rebuildBVH();
    std::vector<std::pair<int, int>> findPotentialCollisionPairs();
    static BVH::BoundingVolume extractBoundingVolume(const std::shared_ptr<RigidBody>& body);

    static bool SATCollisionAndClipping(RigidBody& A, RigidBody& B, std::vector<glm::vec3>& contacts, glm::vec3& collisionNormal, float& penetrationDepth);

    SimulationConfig m_config;
    std::vector<std::shared_ptr<RigidBody>> m_bodies;
    float m_accumulator = 0.0f;
    bool m_paused = false; // pause flag
    
    // BVH for broad-phase collision detection
    bool m_useBVH = true; // Enable BVH by default for better performance
    bool m_bvhNeedsRebuild = true;
    using RigidBodyBVH = BVH::BVHTree<std::shared_ptr<RigidBody>>;
    std::unique_ptr<RigidBodyBVH> m_rigidBodyBVH;
    
    // Performance tracking
    mutable size_t m_lastFramePotentialPairs = 0;
};
