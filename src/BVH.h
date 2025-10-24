#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include "RayCast.h"

// Forward declarations
class SceneNode;

/**
 * @brief Bounding Volume Hierarchy for efficient spatial queries
 * 
 * This class provides a hierarchical spatial data structure that allows
 * for fast ray casting, frustum culling, and nearest neighbor queries
 * on collections of scene nodes or arbitrary objects.
 */
class BVH {
public:
    /**
     * @brief Represents a bounding volume in the hierarchy
     */
    struct BoundingVolume {
        glm::vec3 min = glm::vec3(std::numeric_limits<float>::max());
        glm::vec3 max = glm::vec3(std::numeric_limits<float>::lowest());
        
        BoundingVolume() = default;
        BoundingVolume(const glm::vec3& minBounds, const glm::vec3& maxBounds) 
            : min(minBounds), max(maxBounds) {}
        
        // Get center of bounding volume
        glm::vec3 GetCenter() const {
            return (min + max) * 0.5f;
        }
        
        // Get size/extents of bounding volume
        glm::vec3 GetSize() const {
            return max - min;
        }
        
        // Get surface area of bounding volume
        float GetSurfaceArea() const {
            glm::vec3 size = GetSize();
            return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
        }
        
        // Check if this volume contains a point
        bool Contains(const glm::vec3& point) const {
            return point.x >= min.x && point.x <= max.x &&
                   point.y >= min.y && point.y <= max.y &&
                   point.z >= min.z && point.z <= max.z;
        }
        
        // Check if this volume intersects another
        bool Intersects(const BoundingVolume& other) const {
            return !(max.x < other.min.x || min.x > other.max.x ||
                     max.y < other.min.y || min.y > other.max.y ||
                     max.z < other.min.z || min.z > other.max.z);
        }
        
        // Merge with another bounding volume
        void Merge(const BoundingVolume& other) {
            min = glm::min(min, other.min);
            max = glm::max(max, other.max);
        }
        
        // Create merged bounding volume
        static BoundingVolume Merge(const BoundingVolume& a, const BoundingVolume& b) {
            return BoundingVolume(glm::min(a.min, b.min), glm::max(a.max, b.max));
        }
    };
    
    /**
     * @brief Represents an object in the BVH
     */
    template<typename T>
    struct BVHObject {
        BoundingVolume bounds;
        T data;
        
        BVHObject() = default;
        BVHObject(const BoundingVolume& b, const T& d) : bounds(b), data(d) {}
    };
    
    /**
     * @brief Node in the BVH tree
     */
    template<typename T>
    struct BVHNode {
        BoundingVolume bounds;
        std::unique_ptr<BVHNode> left = nullptr;
        std::unique_ptr<BVHNode> right = nullptr;
        std::vector<BVHObject<T>> objects;
        bool isLeaf = false;
        
        BVHNode() = default;
        BVHNode(const BoundingVolume& b) : bounds(b) {}
    };
    
    /**
     * @brief Construction parameters for BVH
     */
    struct BuildParams {
        size_t maxObjectsPerLeaf = 4;
        size_t maxDepth = 20;
        enum SplitMethod { 
            SPLIT_MIDDLE,       // Split at middle of longest axis
            SPLIT_MEDIAN,       // Split at median object position
            SPLIT_SAH          // Surface Area Heuristic (most expensive but best quality)
        } splitMethod = SPLIT_SAH;
    };
    
    /**
     * @brief Generic BVH class template
     */
    template<typename T>
    class BVHTree {
    public:
        using ObjectType = T;
        using NodeType = BVHNode<T>;
        using ObjectExtractor = std::function<BoundingVolume(const T&)>;
        using QueryCallback = std::function<bool(const T&)>;
        
        /**
         * @brief Constructor
         * @param extractor Function to extract bounding volume from object
         */
        BVHTree(ObjectExtractor extractor) : m_extractor(extractor) {}
        
        /**
         * @brief Build BVH from collection of objects
         * @param objects Vector of objects to build BVH from
         * @param params Build parameters
         */
        void Build(const std::vector<T>& objects, const BuildParams& params = BuildParams()) {
            m_objects.clear();
            m_objects.reserve(objects.size());
            
            // Extract bounding volumes for all objects
            for (const auto& obj : objects) {
                BoundingVolume bounds = m_extractor(obj);
                m_objects.emplace_back(bounds, obj);
            }
            
            if (!m_objects.empty()) {
                m_root = BuildRecursive(m_objects, 0, params);
            }
        }
        
        /**
         * @brief Query BVH with a ray
         * @param ray Ray to test against BVH
         * @param callback Function called for each potentially intersecting object
         * @return Vector of objects that potentially intersect the ray
         */
        std::vector<T> QueryRay(const RayCast::Ray& ray, QueryCallback callback = nullptr) const {
            std::vector<T> results;
            if (m_root) {
                QueryRayRecursive(ray, m_root.get(), results, callback);
            }
            return results;
        }
        
        /**
         * @brief Query BVH with a bounding volume
         * @param bounds Bounding volume to test against BVH
         * @param callback Function called for each potentially intersecting object
         * @return Vector of objects that potentially intersect the volume
         */
        std::vector<T> QueryVolume(const BoundingVolume& bounds, QueryCallback callback = nullptr) const {
            std::vector<T> results;
            if (m_root) {
                QueryVolumeRecursive(bounds, m_root.get(), results, callback);
            }
            return results;
        }
        
        /**
         * @brief Query BVH for objects within radius of a point
         * @param point Center point
         * @param radius Search radius
         * @param callback Function called for each potentially intersecting object
         * @return Vector of objects within radius
         */
        std::vector<T> QuerySphere(const glm::vec3& point, float radius, QueryCallback callback = nullptr) const {
            BoundingVolume sphereBounds(point - glm::vec3(radius), point + glm::vec3(radius));
            return QueryVolume(sphereBounds, callback);
        }
        
        /**
         * @brief Find closest object to a point
         * @param point Point to search from
         * @param maxDistance Maximum search distance
         * @return Closest object and distance, or null if none found
         */
        std::pair<T*, float> FindClosest(const glm::vec3& point, float maxDistance = std::numeric_limits<float>::max()) const {
            T* closest = nullptr;
            float closestDist = maxDistance;
            
            if (m_root) {
                FindClosestRecursive(point, m_root.get(), closest, closestDist);
            }
            
            return {closest, closestDist};
        }
        
        /**
         * @brief Get statistics about the BVH
         */
        struct Statistics {
            size_t nodeCount = 0;
            size_t leafCount = 0;
            size_t objectCount = 0;
            size_t maxDepth = 0;
            size_t maxObjectsInLeaf = 0;
            float avgObjectsPerLeaf = 0.0f;
        };
        
        Statistics GetStatistics() const {
            Statistics stats;
            if (m_root) {
                GatherStatistics(m_root.get(), stats, 0);
                if (stats.leafCount > 0) {
                    stats.avgObjectsPerLeaf = static_cast<float>(stats.objectCount) / stats.leafCount;
                }
            }
            return stats;
        }
        
        /**
         * @brief Clear the BVH
         */
        void Clear() {
            m_root.reset();
            m_objects.clear();
        }
        
        /**
         * @brief Check if BVH is empty
         */
        bool Empty() const {
            return !m_root || m_objects.empty();
        }
        
        /**
         * @brief Get root bounding volume
         */
        BoundingVolume GetRootBounds() const {
            return m_root ? m_root->bounds : BoundingVolume();
        }
        
    private:
        ObjectExtractor m_extractor;
        std::unique_ptr<NodeType> m_root;
        std::vector<BVHObject<T>> m_objects;
        
        std::unique_ptr<NodeType> BuildRecursive(std::vector<BVHObject<T>>& objects, size_t depth, const BuildParams& params) {
            auto node = std::make_unique<NodeType>();
            
            // Calculate bounding volume for all objects
            if (!objects.empty()) {
                node->bounds = objects[0].bounds;
                for (size_t i = 1; i < objects.size(); ++i) {
                    node->bounds.Merge(objects[i].bounds);
                }
            }
            
            // Check if we should make this a leaf
            if (objects.size() <= params.maxObjectsPerLeaf || depth >= params.maxDepth) {
                node->isLeaf = true;
                node->objects = std::move(objects);
                return node;
            }
            
            // Choose split axis and position
            int splitAxis = GetLongestAxis(node->bounds);
            float splitPos = CalculateSplitPosition(objects, splitAxis, params.splitMethod);
            
            // Partition objects
            auto partition = std::partition(objects.begin(), objects.end(),
                [splitAxis, splitPos](const BVHObject<T>& obj) {
                    return obj.bounds.GetCenter()[splitAxis] < splitPos;
                });
            
            size_t leftCount = std::distance(objects.begin(), partition);
            
            // Avoid creating empty partitions
            if (leftCount == 0 || leftCount == objects.size()) {
                leftCount = objects.size() / 2;
                partition = objects.begin() + leftCount;
            }
            
            // Create child nodes
            std::vector<BVHObject<T>> leftObjects(objects.begin(), partition);
            std::vector<BVHObject<T>> rightObjects(partition, objects.end());
            
            if (!leftObjects.empty()) {
                node->left = BuildRecursive(leftObjects, depth + 1, params);
            }
            if (!rightObjects.empty()) {
                node->right = BuildRecursive(rightObjects, depth + 1, params);
            }
            
            return node;
        }
        
        void QueryRayRecursive(const RayCast::Ray& ray, const NodeType* node, 
                              std::vector<T>& results, QueryCallback callback) const {
            if (!node) return;
            
            // Test ray against node bounds
            RayCast::HitResult hit = RayCast::RayIntersectAABB(ray, node->bounds.min, node->bounds.max);
            if (!hit.hit) return;
            
            if (node->isLeaf) {
                // Add all objects in leaf (with optional filtering)
                for (const auto& obj : node->objects) {
                    if (!callback || callback(obj.data)) {
                        results.push_back(obj.data);
                    }
                }
            } else {
                // Recurse into children
                QueryRayRecursive(ray, node->left.get(), results, callback);
                QueryRayRecursive(ray, node->right.get(), results, callback);
            }
        }
        
        void QueryVolumeRecursive(const BoundingVolume& bounds, const NodeType* node,
                                 std::vector<T>& results, QueryCallback callback) const {
            if (!node) return;
            
            // Test volume against node bounds
            if (!node->bounds.Intersects(bounds)) return;
            
            if (node->isLeaf) {
                // Add all objects in leaf that intersect the query volume
                for (const auto& obj : node->objects) {
                    if (obj.bounds.Intersects(bounds)) {
                        if (!callback || callback(obj.data)) {
                            results.push_back(obj.data);
                        }
                    }
                }
            } else {
                // Recurse into children
                QueryVolumeRecursive(bounds, node->left.get(), results, callback);
                QueryVolumeRecursive(bounds, node->right.get(), results, callback);
            }
        }
        
        void FindClosestRecursive(const glm::vec3& point, const NodeType* node,
                                 T*& closest, float& closestDist) const {
            if (!node) return;
            
            // Calculate distance to node bounds
            float distToBounds = RayCast::DistanceToAABB(point, node->bounds.min, node->bounds.max);
            if (distToBounds > closestDist) return; // Early exit
            
            if (node->isLeaf) {
                // Check all objects in leaf
                for (const auto& obj : node->objects) {
                    glm::vec3 objCenter = obj.bounds.GetCenter();
                    float dist = glm::distance(point, objCenter);
                    if (dist < closestDist) {
                        closest = const_cast<T*>(&obj.data);
                        closestDist = dist;
                    }
                }
            } else {
                // Visit children in order of proximity
                float leftDist = node->left ? RayCast::DistanceToAABB(point, node->left->bounds.min, node->left->bounds.max) : std::numeric_limits<float>::max();
                float rightDist = node->right ? RayCast::DistanceToAABB(point, node->right->bounds.min, node->right->bounds.max) : std::numeric_limits<float>::max();
                
                if (leftDist < rightDist) {
                    FindClosestRecursive(point, node->left.get(), closest, closestDist);
                    FindClosestRecursive(point, node->right.get(), closest, closestDist);
                } else {
                    FindClosestRecursive(point, node->right.get(), closest, closestDist);
                    FindClosestRecursive(point, node->left.get(), closest, closestDist);
                }
            }
        }
        
        void GatherStatistics(const NodeType* node, Statistics& stats, size_t depth) const {
            if (!node) return;
            
            stats.nodeCount++;
            stats.maxDepth = std::max(stats.maxDepth, depth);
            
            if (node->isLeaf) {
                stats.leafCount++;
                stats.objectCount += node->objects.size();
                stats.maxObjectsInLeaf = std::max(stats.maxObjectsInLeaf, node->objects.size());
            } else {
                GatherStatistics(node->left.get(), stats, depth + 1);
                GatherStatistics(node->right.get(), stats, depth + 1);
            }
        }
        
        int GetLongestAxis(const BoundingVolume& bounds) const {
            glm::vec3 size = bounds.GetSize();
            if (size.x >= size.y && size.x >= size.z) return 0;
            if (size.y >= size.z) return 1;
            return 2;
        }
        
        float CalculateSplitPosition(const std::vector<BVHObject<T>>& objects, int axis, BuildParams::SplitMethod method) const {
            switch (method) {
                case BuildParams::SPLIT_MIDDLE: {
                    float minVal = std::numeric_limits<float>::max();
                    float maxVal = std::numeric_limits<float>::lowest();
                    for (const auto& obj : objects) {
                        float center = obj.bounds.GetCenter()[axis];
                        minVal = std::min(minVal, center);
                        maxVal = std::max(maxVal, center);
                    }
                    return (minVal + maxVal) * 0.5f;
                }
                
                case BuildParams::SPLIT_MEDIAN: {
                    std::vector<float> positions;
                    positions.reserve(objects.size());
                    for (const auto& obj : objects) {
                        positions.push_back(obj.bounds.GetCenter()[axis]);
                    }
                    std::sort(positions.begin(), positions.end());
                    return positions[positions.size() / 2];
                }
                
                case BuildParams::SPLIT_SAH:
                default: {
                    // Surface Area Heuristic - find optimal split
                    float bestCost = std::numeric_limits<float>::max();
                    float bestSplit = 0.0f;
                    
                    // Try multiple split candidates
                    const int numCandidates = std::min(32, static_cast<int>(objects.size()));
                    for (int i = 1; i < numCandidates; ++i) {
                        float t = static_cast<float>(i) / numCandidates;
                        
                        float minVal = std::numeric_limits<float>::max();
                        float maxVal = std::numeric_limits<float>::lowest();
                        for (const auto& obj : objects) {
                            float center = obj.bounds.GetCenter()[axis];
                            minVal = std::min(minVal, center);
                            maxVal = std::max(maxVal, center);
                        }
                        
                        float splitPos = minVal + t * (maxVal - minVal);
                        float cost = CalculateSAHCost(objects, axis, splitPos);
                        
                        if (cost < bestCost) {
                            bestCost = cost;
                            bestSplit = splitPos;
                        }
                    }
                    
                    return bestSplit;
                }
            }
        }
        
        float CalculateSAHCost(const std::vector<BVHObject<T>>& objects, int axis, float splitPos) const {
            BoundingVolume leftBounds, rightBounds;
            size_t leftCount = 0, rightCount = 0;
            bool leftInit = false, rightInit = false;
            
            for (const auto& obj : objects) {
                if (obj.bounds.GetCenter()[axis] < splitPos) {
                    if (!leftInit) {
                        leftBounds = obj.bounds;
                        leftInit = true;
                    } else {
                        leftBounds.Merge(obj.bounds);
                    }
                    leftCount++;
                } else {
                    if (!rightInit) {
                        rightBounds = obj.bounds;
                        rightInit = true;
                    } else {
                        rightBounds.Merge(obj.bounds);
                    }
                    rightCount++;
                }
            }
            
            // Calculate cost using surface area heuristic
            float leftArea = leftInit ? leftBounds.GetSurfaceArea() : 0.0f;
            float rightArea = rightInit ? rightBounds.GetSurfaceArea() : 0.0f;
            
            return leftArea * leftCount + rightArea * rightCount;
        }
    };
    
    // Convenience type aliases for common use cases
    using SceneNodeBVH = BVHTree<std::shared_ptr<SceneNode>>;
    using Vec3BVH = BVHTree<glm::vec3>;
    
    /**
     * @brief Helper function to create BVH for SceneNodes
     */
    static std::unique_ptr<SceneNodeBVH> CreateSceneNodeBVH(const std::vector<std::shared_ptr<SceneNode>>& nodes);
};