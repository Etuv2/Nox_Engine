#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <functional>

// Forward declarations
class RigidBody;

/**
 * PhysicsBVH - Dynamic AABB Tree for physics broadphase
 * 
 * Implements an incrementally updatable BVH for efficient
 * pair finding in physics simulations. Features:
 * - Fat AABBs to reduce update frequency
 * - Incremental insertion/removal
 * - Efficient pair query with caching
 * - Layer/mask filtering
 */
class PhysicsBVH {
public:
    using BodyID = uint32_t;
    using ProxyID = int32_t;
    static constexpr ProxyID NULL_PROXY = -1;
    
    /**
     * Proxy - Entry in the BVH representing a collider
     */
    struct Proxy {
        glm::vec3 aabbMin;          // Current tight AABB
        glm::vec3 aabbMax;
        glm::vec3 fatAABBMin;       // Fat AABB (includes margin)
        glm::vec3 fatAABBMax;
        BodyID bodyId = 0;
        std::weak_ptr<RigidBody> body;
        bool isStatic = false;
        
        Proxy() = default;
    };
    
    /**
     * Pair - Collision pair between two proxies
     */
    struct Pair {
        ProxyID proxyA;
        ProxyID proxyB;
        
        bool operator==(const Pair& other) const {
            return (proxyA == other.proxyA && proxyB == other.proxyB) ||
                   (proxyA == other.proxyB && proxyB == other.proxyA);
        }
    };
    
    struct PairHash {
        size_t operator()(const Pair& p) const {
            ProxyID minId = std::min(p.proxyA, p.proxyB);
            ProxyID maxId = std::max(p.proxyA, p.proxyB);
            return std::hash<uint64_t>()(
                (static_cast<uint64_t>(minId) << 32) | static_cast<uint64_t>(maxId));
        }
    };

private:
    /**
     * Internal BVH Node
     */
    struct Node {
        glm::vec3 aabbMin;
        glm::vec3 aabbMax;
        
        int32_t parent = NULL_PROXY;
        int32_t left = NULL_PROXY;
        int32_t right = NULL_PROXY;
        int32_t height = 0;
        
        // For leaves: index into proxy array
        ProxyID proxyIndex = NULL_PROXY;
        
        bool IsLeaf() const { return left == NULL_PROXY; }
        
        float GetSurfaceArea() const {
            glm::vec3 d = aabbMax - aabbMin;
            return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
        }
    };
    
public:
    PhysicsBVH();
    ~PhysicsBVH() = default;
    
    /**
     * Create a proxy for a rigid body
     * @param body The rigid body
     * @param margin Fat AABB margin
     * @return Proxy ID
     */
    ProxyID CreateProxy(const std::shared_ptr<RigidBody>& body, float margin);
    
    /**
     * Destroy a proxy
     * @param proxyId Proxy to destroy
     */
    void DestroyProxy(ProxyID proxyId);
    
    /**
     * Update a proxy's AABB
     * Returns true if the proxy was actually moved in the tree
     * @param proxyId Proxy to update
     * @param newMin New tight AABB min
     * @param newMax New tight AABB max
     * @param margin Fat AABB margin
     * @return true if tree was modified
     */
    bool UpdateProxy(ProxyID proxyId, const glm::vec3& newMin, const glm::vec3& newMax, float margin);
    
    /**
     * Query all pairs that might be colliding
     * @param pairs Output vector of pairs
     */
    void QueryPairs(std::vector<Pair>& pairs) const;
    
    /**
     * Query all proxies overlapping a given AABB
     * @param min AABB min
     * @param max AABB max
     * @param callback Called for each overlapping proxy
     */
    void Query(const glm::vec3& min, const glm::vec3& max, 
               std::function<bool(ProxyID)> callback) const;
    
    /**
     * Raycast through the BVH
     * @param origin Ray origin
     * @param direction Ray direction (normalized)
     * @param maxDistance Maximum ray distance
     * @param callback Called for each hit proxy, return false to stop
     */
    void RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                 std::function<bool(ProxyID, float)> callback) const;
    
    /**
     * Get proxy data
     */
    const Proxy* GetProxy(ProxyID id) const;
    Proxy* GetProxy(ProxyID id);
    
    /**
     * Get statistics
     */
    int GetProxyCount() const { return m_proxyCount; }
    int GetNodeCount() const { return static_cast<int>(m_nodes.size()) - m_freeCount; }
    int GetHeight() const;
    float GetAreaRatio() const;
    
    /**
     * Clear all proxies
     */
    void Clear();
    
    /**
     * Validate tree structure (debug)
     */
    bool Validate() const;
    
private:
    // Node allocation
    int32_t AllocateNode();
    void FreeNode(int32_t nodeId);
    
    // Tree operations
    void InsertLeaf(int32_t leafId);
    void RemoveLeaf(int32_t leafId);
    int32_t Balance(int32_t nodeId);
    
    // AABB helpers
    static glm::vec3 Union(const glm::vec3& minA, const glm::vec3& maxA,
                          const glm::vec3& minB, const glm::vec3& maxB,
                          glm::vec3& outMin, glm::vec3& outMax);
    static float SurfaceArea(const glm::vec3& min, const glm::vec3& max);
    static bool Overlaps(const glm::vec3& minA, const glm::vec3& maxA,
                        const glm::vec3& minB, const glm::vec3& maxB);
    
    // Raycast helper
    bool RayAABBIntersect(const glm::vec3& origin, const glm::vec3& invDir,
                          const glm::vec3& min, const glm::vec3& max,
                          float& tMin) const;

private:
    std::vector<Node> m_nodes;
    std::vector<Proxy> m_proxies;
    std::unordered_map<ProxyID, int32_t> m_proxyToNode;
    
    int32_t m_root = NULL_PROXY;
    int32_t m_freeList = NULL_PROXY;
    int32_t m_freeCount = 0;
    int32_t m_proxyCount = 0;
    ProxyID m_nextProxyId = 1;
};
