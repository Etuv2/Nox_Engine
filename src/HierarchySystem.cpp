#include "HierarchySystem.h"
#include <queue>
#include <algorithm>
#include <iostream>

HierarchySystem::HierarchySystem(ComponentManager* componentManager, TransformSystem* transformSystem)
    : m_componentManager(componentManager)
    , m_transformSystem(transformSystem)
{
}

void HierarchySystem::SetParent(EntityID child, EntityID parent, bool preserveWorldTransform)
{
    if (child == INVALID_ENTITY || child == parent) return;

    auto* childTransform = m_componentManager->GetTransform(child);
    if (!childTransform) return;

    glm::mat4 worldTransform;
    if (preserveWorldTransform) {
        worldTransform = m_transformSystem->GetWorldTransform(child);
    }

    // Update parent ID
    childTransform->parentID = parent;
    InvalidateCache();

    if (preserveWorldTransform && parent != INVALID_ENTITY) {
        // Recompute local transform to preserve world position
        m_transformSystem->SetWorldTransform(child, worldTransform);
    }

    // Mark child subtree as dirty
    m_transformSystem->MarkSubtreeDirty(child);
}

void HierarchySystem::RemoveParent(EntityID child, bool preserveWorldTransform)
{
    SetParent(child, INVALID_ENTITY, preserveWorldTransform);
}

EntityID HierarchySystem::GetParent(EntityID entity) const
{
    const auto* transform = m_componentManager->GetTransform(entity);
    return transform ? transform->parentID : INVALID_ENTITY;
}

std::vector<EntityID> HierarchySystem::GetChildren(EntityID entity) const
{
    if (!m_cacheValid) {
        RebuildCache();
    }

    auto it = m_childrenCache.find(entity);
    if (it != m_childrenCache.end()) {
        return it->second;
    }
    return {};
}

std::vector<EntityID> HierarchySystem::GetAllDescendants(EntityID entity) const
{
    std::vector<EntityID> descendants;
    
    std::queue<EntityID> queue;
    queue.push(entity);

    while (!queue.empty()) {
        EntityID current = queue.front();
        queue.pop();

        auto children = GetChildren(current);
        for (EntityID child : children) {
            descendants.push_back(child);
            queue.push(child);
        }
    }

    return descendants;
}

std::vector<EntityID> HierarchySystem::GetAncestors(EntityID entity) const
{
    std::vector<EntityID> ancestors;
    
    EntityID current = GetParent(entity);
    while (current != INVALID_ENTITY) {
        ancestors.push_back(current);
        current = GetParent(current);
    }

    return ancestors;
}

std::vector<EntityID> HierarchySystem::GetRootEntities() const
{
    std::vector<EntityID> roots;

    const auto& transformPool = m_componentManager->GetTransformPool();
    for (const auto& entry : transformPool) {
        if (entry.component.parentID == INVALID_ENTITY) {
            roots.push_back(entry.entity);
        }
    }

    return roots;
}

bool HierarchySystem::IsRootEntity(EntityID entity) const
{
    const auto* transform = m_componentManager->GetTransform(entity);
    return transform && transform->parentID == INVALID_ENTITY;
}

EntityID HierarchySystem::FindEntityByName(const std::string& name) const
{
    return m_componentManager->FindEntityByName(name);
}

std::vector<EntityID> HierarchySystem::FindEntitiesByType(NodeType type) const
{
    return m_componentManager->FindEntitiesByType(type);
}

EntityID HierarchySystem::FindFirstChildByName(EntityID parent, const std::string& name) const
{
    auto children = GetChildren(parent);
    for (EntityID child : children) {
        std::string childName = m_componentManager->GetEntityName(child);
        if (childName == name) {
            return child;
        }
    }
    return INVALID_ENTITY;
}

void HierarchySystem::TraverseDepthFirst(EntityID root, TraversalCallback callback) const
{
    if (root == INVALID_ENTITY) {
        // Traverse all root entities
        auto roots = GetRootEntities();
        for (EntityID r : roots) {
            TraverseDepthFirstRecursive(r, 0, callback);
        }
    } else {
        TraverseDepthFirstRecursive(root, 0, callback);
    }
}

void HierarchySystem::TraverseDepthFirstRecursive(EntityID entity, int depth, TraversalCallback& callback) const
{
    if (!callback(entity, depth)) {
        return; // Callback returned false, stop traversal
    }

    auto children = GetChildren(entity);
    for (EntityID child : children) {
        TraverseDepthFirstRecursive(child, depth + 1, callback);
    }
}

void HierarchySystem::TraverseBreadthFirst(EntityID root, TraversalCallback callback) const
{
    std::queue<std::pair<EntityID, int>> queue;

    if (root == INVALID_ENTITY) {
        auto roots = GetRootEntities();
        for (EntityID r : roots) {
            queue.push({r, 0});
        }
    } else {
        queue.push({root, 0});
    }

    while (!queue.empty()) {
        auto [entity, depth] = queue.front();
        queue.pop();

        if (!callback(entity, depth)) {
            continue; // Skip children if callback returns false
        }

        auto children = GetChildren(entity);
        for (EntityID child : children) {
            queue.push({child, depth + 1});
        }
    }
}

void HierarchySystem::TraverseAll(TraversalCallback callback) const
{
    TraverseDepthFirst(INVALID_ENTITY, callback);
}

void HierarchySystem::MoveToTop(EntityID entity)
{
    // Reorder in parent's children list - implementation would need
    // additional data structure to track child ordering
    // For now, this is a placeholder
}

void HierarchySystem::MoveToBottom(EntityID entity)
{
    // Placeholder
}

void HierarchySystem::MoveUp(EntityID entity)
{
    // Placeholder
}

void HierarchySystem::MoveDown(EntityID entity)
{
    // Placeholder
}

bool HierarchySystem::ValidateHierarchy() const
{
    const auto& transformPool = m_componentManager->GetTransformPool();
    
    for (const auto& entry : transformPool) {
        EntityID parent = entry.component.parentID;
        
        if (parent != INVALID_ENTITY) {
            // Check that parent exists
            if (!m_componentManager->HasTransform(parent)) {
                std::cerr << "[HierarchySystem] Entity " << entry.entity 
                          << " has invalid parent " << parent << std::endl;
                return false;
            }

            // Check for cycles (entity is its own ancestor)
            auto ancestors = GetAncestors(entry.entity);
            if (std::find(ancestors.begin(), ancestors.end(), entry.entity) != ancestors.end()) {
                std::cerr << "[HierarchySystem] Cycle detected for entity " << entry.entity << std::endl;
                return false;
            }
        }
    }

    return true;
}

void HierarchySystem::RemoveOrphanedEntities()
{
    std::vector<EntityID> toRemove;

    const auto& transformPool = m_componentManager->GetTransformPool();
    for (const auto& entry : transformPool) {
        EntityID parent = entry.component.parentID;
        
        if (parent != INVALID_ENTITY && !m_componentManager->HasTransform(parent)) {
            toRemove.push_back(entry.entity);
        }
    }

    for (EntityID entity : toRemove) {
        std::cout << "[HierarchySystem] Removing orphaned entity " << entity << std::endl;
        m_componentManager->DestroyEntity(entity);
    }

    if (!toRemove.empty()) {
        InvalidateCache();
    }
}

size_t HierarchySystem::GetDepth(EntityID entity) const
{
    size_t depth = 0;
    EntityID current = GetParent(entity);
    
    while (current != INVALID_ENTITY) {
        depth++;
        current = GetParent(current);
    }

    return depth;
}

size_t HierarchySystem::GetTotalEntityCount() const
{
    return m_componentManager->GetTransformPool().Size();
}

size_t HierarchySystem::GetMaxDepth() const
{
    size_t maxDepth = 0;

    TraverseAll([&maxDepth](EntityID entity, int depth) {
        maxDepth = std::max(maxDepth, static_cast<size_t>(depth));
        return true;
    });

    return maxDepth;
}

void HierarchySystem::InvalidateCache()
{
    m_cacheValid = false;
    m_childrenCache.clear();
}

void HierarchySystem::RebuildCache() const
{
    m_childrenCache.clear();

    const auto& transformPool = m_componentManager->GetTransformPool();
    for (const auto& entry : transformPool) {
        EntityID parent = entry.component.parentID;
        if (parent != INVALID_ENTITY) {
            m_childrenCache[parent].push_back(entry.entity);
        }
    }

    m_cacheValid = true;
}
