#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

// Forward declarations
class RigidBody;
struct ContactManifold;
struct ContactPoint;

/**
 * PhysicsCollision - Collision detection between primitive shapes
 * 
 * Implements:
 * - Sphere vs Sphere
 * - Sphere vs Box
 * - Sphere vs Plane
 * - Box vs Box (SAT with manifold generation)
 * - Box vs Plane
 * 
 * All methods generate contact manifolds for stable collision response.
 */
namespace PhysicsCollision {

    /**
     * Test collision between two rigid bodies
     * Dispatches to appropriate shape-specific test
     * @param bodyA First body
     * @param bodyB Second body
     * @param manifold Output contact manifold
     * @return true if collision detected
     */
    bool TestCollision(const std::shared_ptr<RigidBody>& bodyA,
                       const std::shared_ptr<RigidBody>& bodyB,
                       ContactManifold& manifold);
    
    // ============== SPHERE COLLISIONS ==============
    
    /**
     * Sphere vs Sphere collision
     */
    bool SphereSphere(const std::shared_ptr<RigidBody>& sphereA,
                      const std::shared_ptr<RigidBody>& sphereB,
                      ContactManifold& manifold);
    
    /**
     * Sphere vs Box collision
     */
    bool SphereBox(const std::shared_ptr<RigidBody>& sphere,
                   const std::shared_ptr<RigidBody>& box,
                   ContactManifold& manifold);
    
    /**
     * Sphere vs Plane collision
     */
    bool SpherePlane(const std::shared_ptr<RigidBody>& sphere,
                     const std::shared_ptr<RigidBody>& plane,
                     ContactManifold& manifold);
    
    // ============== BOX COLLISIONS ==============
    
    /**
     * Box vs Box collision using SAT (Separating Axis Theorem)
     * Generates up to 4 contact points for stable stacking
     */
    bool BoxBox(const std::shared_ptr<RigidBody>& boxA,
                const std::shared_ptr<RigidBody>& boxB,
                ContactManifold& manifold);
    
    /**
     * Box vs Plane collision
     * Generates face contacts for stable resting
     */
    bool BoxPlane(const std::shared_ptr<RigidBody>& box,
                  const std::shared_ptr<RigidBody>& plane,
                  ContactManifold& manifold);
    
    // ============== AABB TESTS ==============
    
    /**
     * Test if two AABBs overlap (broadphase)
     */
    bool AABBOverlap(const glm::vec3& minA, const glm::vec3& maxA,
                     const glm::vec3& minB, const glm::vec3& maxB);
    
    /**
     * Test if a point is inside an AABB
     */
    bool PointInAABB(const glm::vec3& point,
                     const glm::vec3& min, const glm::vec3& max);
    
    // ============== UTILITY ==============
    
    /**
     * Compute closest point on OBB to a given point
     */
    glm::vec3 ClosestPointOnOBB(const glm::vec3& point,
                                const glm::vec3& boxCenter,
                                const glm::mat3& boxRotation,
                                const glm::vec3& halfExtents);
    
    /**
     * Compute closest point on plane to a given point
     */
    glm::vec3 ClosestPointOnPlane(const glm::vec3& point,
                                  const glm::vec3& planeNormal,
                                  float planeDistance);
    
    /**
     * Compute signed distance from point to plane
     */
    float SignedDistanceToPlane(const glm::vec3& point,
                                const glm::vec3& planeNormal,
                                float planeDistance);
    
    /**
     * Clip a polygon against a plane
     * Used for box-box contact manifold generation
     */
    std::vector<glm::vec3> ClipPolygonAgainstPlane(
        const std::vector<glm::vec3>& polygon,
        const glm::vec3& planeNormal,
        float planeOffset);
    
    /**
     * Get the vertices of an OBB face
     */
    std::vector<glm::vec3> GetBoxFaceVertices(
        const glm::vec3& center,
        const glm::mat3& rotation,
        const glm::vec3& halfExtents,
        int faceIndex);
    
    /**
     * Project a point onto an axis and get the interval
     */
    void ProjectBoxOntoAxis(const glm::vec3& center,
                           const glm::mat3& rotation,
                           const glm::vec3& halfExtents,
                           const glm::vec3& axis,
                           float& outMin, float& outMax);
    
    /**
     * Combine friction coefficients (geometric mean)
     */
    inline float CombineFriction(float frictionA, float frictionB) {
        return std::sqrt(frictionA * frictionB);
    }
    
    /**
     * Combine restitution coefficients (maximum)
     */
    inline float CombineRestitution(float restA, float restB) {
        return std::max(restA, restB);
    }
    
} // namespace PhysicsCollision
