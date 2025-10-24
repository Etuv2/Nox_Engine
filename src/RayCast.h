#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <functional>

class SceneNode;
class Camera;

/**
 * @brief Utility class for 3D ray casting operations
 * 
 * This class provides a comprehensive set of ray casting utilities
 * that can be used for mouse picking, gizmo interactions, collision detection,
 * and other spatial queries in 3D space.
 */
class RayCast {
public:
    /**
     * @brief Represents a ray in 3D space
     */
    struct Ray {
        glm::vec3 origin;
        glm::vec3 direction;  // Should be normalized
        
        Ray() : origin(0.0f), direction(0.0f, 0.0f, -1.0f) {}
        Ray(const glm::vec3& orig, const glm::vec3& dir) 
            : origin(orig), direction(glm::normalize(dir)) {}
        
        // Get point along ray at distance t
        glm::vec3 GetPoint(float t) const {
            return origin + direction * t;
        }
    };
    
    /**
     * @brief Represents intersection result
     */
    struct HitResult {
        bool hit = false;
        float distance = 0.0f;
        glm::vec3 point = glm::vec3(0.0f);
        glm::vec3 normal = glm::vec3(0.0f);
        std::shared_ptr<SceneNode> node = nullptr;
        
        HitResult() = default;
        HitResult(bool h, float d, const glm::vec3& p, const glm::vec3& n, std::shared_ptr<SceneNode> sceneNode = nullptr)
            : hit(h), distance(d), point(p), normal(n), node(sceneNode) {}
    };
    
    /**
     * @brief Convert screen coordinates to world ray
     * @param mouseX Screen X coordinate
     * @param mouseY Screen Y coordinate  
     * @param camera Camera for view/projection matrices
     * @param windowWidth Window width in pixels
     * @param windowHeight Window height in pixels
     * @return Ray in world space
     */
    static Ray ScreenToWorldRay(int mouseX, int mouseY, std::shared_ptr<Camera> camera, 
                                int windowWidth, int windowHeight);
    
    /**
     * @brief Test ray intersection with axis-aligned bounding box
     * @param ray Ray to test
     * @param aabbMin Minimum corner of AABB
     * @param aabbMax Maximum corner of AABB
     * @return HitResult with intersection details
     */
    static HitResult RayIntersectAABB(const Ray& ray, const glm::vec3& aabbMin, const glm::vec3& aabbMax);
    
    /**
     * @brief Test ray intersection with oriented bounding box
     * @param ray Ray to test
     * @param center OBB center
     * @param halfExtents OBB half extents
     * @param orientation OBB rotation matrix
     * @return HitResult with intersection details
     */
    static HitResult RayIntersectOBB(const Ray& ray, const glm::vec3& center, 
                                   const glm::vec3& halfExtents, const glm::mat3& orientation);
    
    /**
     * @brief Test ray intersection with sphere
     * @param ray Ray to test
     * @param center Sphere center
     * @param radius Sphere radius
     * @return HitResult with intersection details
     */
    static HitResult RayIntersectSphere(const Ray& ray, const glm::vec3& center, float radius);
    
    /**
     * @brief Test ray intersection with plane
     * @param ray Ray to test
     * @param planePoint Point on plane
     * @param planeNormal Plane normal (normalized)
     * @return HitResult with intersection details
     */
    static HitResult RayIntersectPlane(const Ray& ray, const glm::vec3& planePoint, const glm::vec3& planeNormal);
    
    /**
     * @brief Test ray intersection with triangle
     * @param ray Ray to test
     * @param v0 Triangle vertex 0
     * @param v1 Triangle vertex 1
     * @param v2 Triangle vertex 2
     * @return HitResult with intersection details
     */
    static HitResult RayIntersectTriangle(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2);
    
    /**
     * @brief Test ray intersection with a single scene node
     * @param ray Ray to test
     * @param node SceneNode to test against
     * @param worldTransform World transformation matrix for the node
     * @return HitResult with intersection details
     */
    static HitResult RayIntersectNode(const Ray& ray, std::shared_ptr<SceneNode> node, const glm::mat4& worldTransform);
    
    /**
     * @brief Test ray intersection with scene graph (traverses hierarchy)
     * @param ray Ray to test
     * @param rootNode Root node of scene graph
     * @param filter Optional filter function to skip certain nodes
     * @return HitResult with closest intersection
     */
    static HitResult RayIntersectScene(const Ray& ray, std::shared_ptr<SceneNode> rootNode,
                                     std::function<bool(std::shared_ptr<SceneNode>)> filter = nullptr);
    
    /**
     * @brief Get all intersections along a ray (not just the closest)
     * @param ray Ray to test
     * @param rootNode Root node of scene graph
     * @param maxResults Maximum number of results to return (0 = unlimited)
     * @param filter Optional filter function to skip certain nodes
     * @return Vector of HitResults sorted by distance
     */
    static std::vector<HitResult> RayIntersectSceneAll(const Ray& ray, std::shared_ptr<SceneNode> rootNode,
                                                      size_t maxResults = 0,
                                                      std::function<bool(std::shared_ptr<SceneNode>)> filter = nullptr);
    
    /**
     * @brief Test if a point is inside an AABB
     * @param point Point to test
     * @param aabbMin Minimum corner of AABB
     * @param aabbMax Maximum corner of AABB
     * @return True if point is inside AABB
     */
    static bool PointInAABB(const glm::vec3& point, const glm::vec3& aabbMin, const glm::vec3& aabbMax);
    
    /**
     * @brief Get closest point on AABB to given point
     * @param point Point to test
     * @param aabbMin Minimum corner of AABB
     * @param aabbMax Maximum corner of AABB
     * @return Closest point on AABB surface
     */
    static glm::vec3 ClosestPointOnAABB(const glm::vec3& point, const glm::vec3& aabbMin, const glm::vec3& aabbMax);
    
    /**
     * @brief Calculate distance from point to AABB
     * @param point Point to test
     * @param aabbMin Minimum corner of AABB
     * @param aabbMax Maximum corner of AABB
     * @return Distance to AABB (0 if point is inside)
     */
    static float DistanceToAABB(const glm::vec3& point, const glm::vec3& aabbMin, const glm::vec3& aabbMax);

private:
    /**
     * @brief Internal helper for scene traversal
     */
    static void TraverseSceneForIntersection(const Ray& ray, std::shared_ptr<SceneNode> node, 
                                           const glm::mat4& parentTransform,
                                           std::vector<HitResult>& results,
                                           std::function<bool(std::shared_ptr<SceneNode>)> filter);
};