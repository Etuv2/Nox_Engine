#pragma once

#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <cstdint>

// Forward declarations
class RigidBody;

/**
 * ContactPoint - Single contact point between two bodies
 */
struct ContactPoint {
    glm::vec3 point;              // World-space contact point
    glm::vec3 localPointA;        // Contact point in body A's local space
    glm::vec3 localPointB;        // Contact point in body B's local space
    float penetration = 0.0f;     // Penetration depth (positive = penetrating)
    
    // Warm starting - cached impulses from previous frame
    float normalImpulseAccum = 0.0f;     // Accumulated normal impulse
    float tangentImpulseAccum1 = 0.0f;   // Accumulated tangent impulse (first axis)
    float tangentImpulseAccum2 = 0.0f;   // Accumulated tangent impulse (second axis)
    
    // Constraint data (computed per-frame)
    float normalMass = 0.0f;      // Effective mass for normal constraint
    float tangentMass1 = 0.0f;    // Effective mass for first tangent
    float tangentMass2 = 0.0f;    // Effective mass for second tangent
    float velocityBias = 0.0f;    // Bias for position correction
    
    // For contact point matching between frames
    uint32_t featureId = 0;       // Feature identifier for warm starting
    
    ContactPoint() = default;
    ContactPoint(const glm::vec3& p, float pen) : point(p), penetration(pen) {}
};

/**
 * ContactManifold - Collection of contact points between two bodies
 * 
 * Stores up to 4 contact points for stable collision response.
 * Implements contact caching for warm starting between frames.
 */
struct ContactManifold {
    static constexpr int MAX_CONTACTS = 4;
    
    // Bodies involved in contact
    std::shared_ptr<RigidBody> bodyA;
    std::shared_ptr<RigidBody> bodyB;
    
    // Contact geometry
    glm::vec3 normal = glm::vec3(0.0f, 1.0f, 0.0f);  // Contact normal (from A to B)
    glm::vec3 tangent1 = glm::vec3(1.0f, 0.0f, 0.0f); // First tangent direction
    glm::vec3 tangent2 = glm::vec3(0.0f, 0.0f, 1.0f); // Second tangent direction
    
    // Contact points
    std::array<ContactPoint, MAX_CONTACTS> points;
    int pointCount = 0;
    
    // Combined material properties
    float friction = 0.5f;
    float restitution = 0.3f;
    
    // Constraint identification
    uint64_t pairKey = 0;         // Unique key for this body pair
    int lifetime = 0;              // Frames this manifold has existed
    
    ContactManifold() = default;
    
    /**
     * Add a contact point to the manifold
     * Maintains up to MAX_CONTACTS points, replacing worst if full
     */
    void addPoint(const ContactPoint& point);
    
    /**
     * Clear all contact points
     */
    void clear() { pointCount = 0; }
    
    /**
     * Compute tangent basis from normal
     */
    void computeTangentBasis();
    
    /**
     * Generate unique pair key from two body IDs
     */
    static uint64_t makePairKey(uint32_t idA, uint32_t idB);
    
    /**
     * Refresh contact points after bodies have moved
     * Removes points that have separated too far
     */
    void refreshContacts(float breakingThreshold);
    
    /**
     * Warm start contact constraints using cached impulses
     */
    void warmStart(float warmStartFactor);
    
    /**
     * Update point positions from local space
     */
    void updateWorldPositions();
};

/**
 * Contact - Legacy single-point contact structure
 * Kept for backward compatibility
 */
struct Contact {
    glm::vec3 point;
    glm::vec3 normal;
    float penetration;
    
    Contact() : point(0.0f), normal(0.0f, 1.0f, 0.0f), penetration(0.0f) {}
    Contact(const glm::vec3& p, const glm::vec3& n, float pen) 
        : point(p), normal(n), penetration(pen) {}
};
