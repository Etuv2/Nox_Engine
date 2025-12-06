#pragma once
#include "ComponentManager.h"
#include "TransformSystem.h"
#include <glm/glm.hpp>
#include <vector>
#include <functional>

/**
 * Hierarchy System - Manages parent-child relationships in the ECS
 * 
 * This system handles:
 * - Parent-child relationship management
 * - Hierarchical traversal (depth-first, breadth-first)
 * - Scene graph queries (find by name, find by type)
 * - Reparenting operations with transform preservation
 * - Orphan detection and cleanup
 */
class HierarchySystem {
public:
    HierarchySystem(ComponentManager* componentManager, TransformSystem* transformSystem);
    ~HierarchySystem() = default;

    // Parent-child operations
    void SetParent(EntityID child, EntityID parent, bool preserveWorldTransform = true);
    void RemoveParent(EntityID child, bool preserveWorldTransform = true);
    EntityID GetParent(EntityID entity) const;
    std::vector<EntityID> GetChildren(EntityID entity) const;
    std::vector<EntityID> GetAllDescendants(EntityID entity) const;
    std::vector<EntityID> GetAncestors(EntityID entity) const;
    
    // Root entities
    std::vector<EntityID> GetRootEntities() const;
    bool IsRootEntity(EntityID entity) const;

    // Hierarchy queries
    EntityID FindEntityByName(const std::string& name) const;
    std::vector<EntityID> FindEntitiesByType(NodeType type) const;
    EntityID FindFirstChildByName(EntityID parent, const std::string& name) const;
    
    // Traversal
    using TraversalCallback = std::function<bool(EntityID entity, int depth)>;
    void TraverseDepthFirst(EntityID root, TraversalCallback callback) const;
    void TraverseBreadthFirst(EntityID root, TraversalCallback callback) const;
    void TraverseAll(TraversalCallback callback) const;

    // Hierarchy manipulation
    void MoveToTop(EntityID entity);    // Move to first child position
    void MoveToBottom(EntityID entity); // Move to last child position
    void MoveUp(EntityID entity);       // Swap with previous sibling
    void MoveDown(EntityID entity);     // Swap with next sibling

    // Validation and cleanup
    bool ValidateHierarchy() const;
    void RemoveOrphanedEntities();
    size_t GetDepth(EntityID entity) const;

    // Statistics
    size_t GetTotalEntityCount() const;
    size_t GetMaxDepth() const;

private:
    ComponentManager* m_componentManager;
    TransformSystem* m_transformSystem;

    // Cached children lists for fast lookup
    mutable std::unordered_map<EntityID, std::vector<EntityID>> m_childrenCache;
    mutable bool m_cacheValid = false;

    void InvalidateCache();
    void RebuildCache() const;
    
    // Internal traversal helpers
    void TraverseDepthFirstRecursive(EntityID entity, int depth, TraversalCallback& callback) const;
};
