#include "SceneRuntimeData.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <chrono>

namespace {
glm::mat4 IdentityMatrix()
{
    return glm::mat4(1.0f);
}
}

SceneRuntimeData::SceneRuntimeData(ComponentManager* componentManager)
    : m_componentManager(componentManager)
{
}

void SceneRuntimeData::SetComponentManager(ComponentManager* componentManager)
{
    m_componentManager = componentManager;
    MarkHierarchyDirty();
}

void SceneRuntimeData::MarkHierarchyDirty()
{
    m_compiledHierarchyRevision = 0;
}

bool SceneRuntimeData::EnsureCompiled()
{
    if (!m_componentManager) {
        return false;
    }

    const uint64_t revision = m_componentManager->GetHierarchyRevision();
    if (m_compiledHierarchyRevision == revision && !m_nodes.empty()) {
        return true;
    }

    Rebuild();
    return !m_nodes.empty();
}

uint32_t SceneRuntimeData::FindRuntimeIndex(EntityID entity) const
{
    const auto it = m_entityToRuntime.find(entity);
    return (it != m_entityToRuntime.end()) ? it->second : INVALID_RUNTIME_NODE_INDEX;
}

const RuntimeTransformNode* SceneRuntimeData::GetRuntimeNode(EntityID entity) const
{
    return GetRuntimeNodeByIndex(FindRuntimeIndex(entity));
}

const RuntimeTransformNode* SceneRuntimeData::GetRuntimeNodeByIndex(uint32_t index) const
{
    return (index < m_nodes.size()) ? &m_nodes[index] : nullptr;
}

const glm::mat4& SceneRuntimeData::GetWorldTransform(EntityID entity) const
{
    return GetWorldTransformByIndex(FindRuntimeIndex(entity));
}

const glm::mat4& SceneRuntimeData::GetWorldTransformByIndex(uint32_t index) const
{
    static const glm::mat4 kIdentity = IdentityMatrix();
    return (index < m_worldTransforms.size()) ? m_worldTransforms[index] : kIdentity;
}

const glm::mat4& SceneRuntimeData::GetPreviousWorldTransformByIndex(uint32_t index) const
{
    static const glm::mat4 kIdentity = IdentityMatrix();
    return (index < m_prevWorldTransforms.size()) ? m_prevWorldTransforms[index] : kIdentity;
}

const glm::mat4& SceneRuntimeData::GetLocalTransformByIndex(uint32_t index) const
{
    static const glm::mat4 kIdentity = IdentityMatrix();
    return (index < m_localTransforms.size()) ? m_localTransforms[index] : kIdentity;
}

size_t SceneRuntimeData::EvaluateDirtySubtrees(const std::vector<EntityID>& dirtyRoots,
                                               std::vector<EntityID>* changedEntities,
                                               std::vector<uint32_t>* changedRuntimeIndices)
{
    const auto evalStart = std::chrono::high_resolution_clock::now();
    m_diagnostics.lastDirtyRootInputCount = dirtyRoots.size();
    m_diagnostics.lastDirtySpanCount = 0;
    m_diagnostics.lastDirtySpanCoverageNodes = 0;
    m_diagnostics.lastRecomputedNodes = 0;
    m_diagnostics.evaluateDirtySubtreesMs = 0.0f;

    if (!EnsureCompiled()) {
        return 0;
    }

    if (changedEntities) {
        changedEntities->clear();
    }
    if (changedRuntimeIndices) {
        changedRuntimeIndices->clear();
    }

    const std::vector<DirtySpan> spans = BuildDirtySpans(dirtyRoots);
    m_diagnostics.lastDirtySpanCount = spans.size();
    for (const DirtySpan& span : spans) {
        if (span.end > span.begin) {
            m_diagnostics.lastDirtySpanCoverageNodes += static_cast<size_t>(span.end - span.begin);
        }
    }
    size_t recomputed = 0;

    for (const DirtySpan& span : spans) {
        if (span.rootIndex == INVALID_RUNTIME_NODE_INDEX) {
            continue;
        }

        RefreshLocalStateForSubtree(span.rootIndex);
        for (uint32_t traversalPos = span.begin; traversalPos < span.end; ++traversalPos) {
            const uint32_t runtimeIndex = m_traversalOrder[traversalPos];
            RuntimeTransformNode& node = m_nodes[runtimeIndex];

            const glm::mat4 local = m_hasAnimation[runtimeIndex] ? m_animatedTransforms[runtimeIndex] : m_localTransforms[runtimeIndex];
            const glm::mat4 parentWorld = (node.parent != INVALID_RUNTIME_NODE_INDEX)
                ? m_worldTransforms[node.parent]
                : IdentityMatrix();

            m_prevWorldTransforms[runtimeIndex] = m_worldTransforms[runtimeIndex];
            m_worldTransforms[runtimeIndex] = parentWorld * local;
            node.worldGeneration = ++m_latestWorldGeneration;
            ++recomputed;

            if (changedEntities) {
                changedEntities->push_back(node.entity);
            }
            if (changedRuntimeIndices) {
                changedRuntimeIndices->push_back(runtimeIndex);
            }

            if (TransformComponent* transform = m_componentManager->GetTransform(node.entity)) {
                transform->prevWorldTransform = m_prevWorldTransforms[runtimeIndex];
                transform->worldTransform = m_worldTransforms[runtimeIndex];
                transform->localTransform = m_localTransforms[runtimeIndex];
                transform->animatedTransform = m_animatedTransforms[runtimeIndex];
                transform->hasAnimation = (m_hasAnimation[runtimeIndex] != 0u);
                transform->isDirty = false;
            }
        }
    }

    m_diagnostics.lastRecomputedNodes = recomputed;
    const auto evalEnd = std::chrono::high_resolution_clock::now();
    m_diagnostics.evaluateDirtySubtreesMs =
        std::chrono::duration<float, std::milli>(evalEnd - evalStart).count();

    return recomputed;
}

void SceneRuntimeData::Rebuild()
{
    m_nodes.clear();
    m_traversalOrder.clear();
    m_entityToRuntime.clear();
    m_localTransforms.clear();
    m_worldTransforms.clear();
    m_prevWorldTransforms.clear();
    m_animatedTransforms.clear();
    m_localTranslations.clear();
    m_localRotations.clear();
    m_localScales.clear();
    m_hasAnimation.clear();

    if (!m_componentManager) {
        return;
    }

    auto& transformPool = m_componentManager->GetTransformPool();
    const size_t nodeCount = transformPool.Size();
    m_nodes.resize(nodeCount);
    m_localTransforms.resize(nodeCount, IdentityMatrix());
    m_worldTransforms.resize(nodeCount, IdentityMatrix());
    m_prevWorldTransforms.resize(nodeCount, IdentityMatrix());
    m_animatedTransforms.resize(nodeCount, IdentityMatrix());
    m_localTranslations.resize(nodeCount, glm::vec3(0.0f));
    m_localRotations.resize(nodeCount, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    m_localScales.resize(nodeCount, glm::vec3(1.0f));
    m_hasAnimation.resize(nodeCount, 0u);

    size_t denseIndex = 0;
    for (const auto& entry : transformPool) {
        const TransformComponent& transform = entry.component;
        RuntimeTransformNode& runtimeNode = m_nodes[denseIndex];
        runtimeNode.entity = entry.entity;
        runtimeNode.transformID = transform.transformID;
        m_entityToRuntime[entry.entity] = static_cast<uint32_t>(denseIndex);
        RefreshLocalStateForNode(static_cast<uint32_t>(denseIndex), transform);
        m_worldTransforms[denseIndex] = transform.worldTransform;
        m_prevWorldTransforms[denseIndex] = transform.prevWorldTransform;
        ++denseIndex;
    }

    for (size_t index = 0; index < m_nodes.size(); ++index) {
        const TransformComponent* transform = m_componentManager->GetTransform(m_nodes[index].entity);
        if (!transform || transform->parentID == INVALID_ENTITY) {
            continue;
        }

        const auto it = m_entityToRuntime.find(transform->parentID);
        if (it != m_entityToRuntime.end()) {
            m_nodes[index].parent = it->second;
        }
    }

    for (size_t index = 0; index < m_nodes.size(); ++index) {
        const EntityID entity = m_nodes[index].entity;
        const auto& children = m_componentManager->GetChildren(entity);
        uint32_t previousChild = INVALID_RUNTIME_NODE_INDEX;
        for (EntityID childEntity : children) {
            const auto it = m_entityToRuntime.find(childEntity);
            if (it == m_entityToRuntime.end()) {
                continue;
            }

            const uint32_t childIndex = it->second;
            if (m_nodes[index].firstChild == INVALID_RUNTIME_NODE_INDEX) {
                m_nodes[index].firstChild = childIndex;
            }
            if (previousChild != INVALID_RUNTIME_NODE_INDEX) {
                m_nodes[previousChild].nextSibling = childIndex;
            }
            previousChild = childIndex;
        }
    }

    m_traversalOrder.reserve(m_nodes.size());
    std::vector<uint32_t> roots;
    roots.reserve(m_nodes.size());
    for (size_t index = 0; index < m_nodes.size(); ++index) {
        if (m_nodes[index].parent == INVALID_RUNTIME_NODE_INDEX) {
            roots.push_back(static_cast<uint32_t>(index));
        }
    }

    struct StackEntry {
        uint32_t runtimeIndex;
        bool entered;
        uint32_t nextChild;
    };

    for (uint32_t rootIndex : roots) {
        std::vector<StackEntry> stack;
        stack.push_back({ rootIndex, false, INVALID_RUNTIME_NODE_INDEX });

        while (!stack.empty()) {
            StackEntry& top = stack.back();
            RuntimeTransformNode& node = m_nodes[top.runtimeIndex];

            if (!top.entered) {
                top.entered = true;
                node.subtreeBegin = static_cast<uint32_t>(m_traversalOrder.size());
                node.depth = (node.parent != INVALID_RUNTIME_NODE_INDEX) ? (m_nodes[node.parent].depth + 1u) : 0u;
                top.nextChild = node.firstChild;
                m_traversalOrder.push_back(top.runtimeIndex);
            }

            if (top.nextChild != INVALID_RUNTIME_NODE_INDEX) {
                const uint32_t childIndex = top.nextChild;
                top.nextChild = m_nodes[childIndex].nextSibling;
                stack.push_back({ childIndex, false, INVALID_RUNTIME_NODE_INDEX });
                continue;
            }

            node.subtreeEnd = static_cast<uint32_t>(m_traversalOrder.size());
            stack.pop_back();
        }
    }

    m_compiledHierarchyRevision = m_componentManager->GetHierarchyRevision();
    ++m_compileCount;
}

void SceneRuntimeData::RefreshLocalStateForSubtree(uint32_t rootIndex)
{
    if (rootIndex >= m_nodes.size()) {
        return;
    }

    const RuntimeTransformNode& rootNode = m_nodes[rootIndex];
    for (uint32_t traversalPos = rootNode.subtreeBegin; traversalPos < rootNode.subtreeEnd; ++traversalPos) {
        const uint32_t runtimeIndex = m_traversalOrder[traversalPos];
        if (const TransformComponent* transform = m_componentManager->GetTransform(m_nodes[runtimeIndex].entity)) {
            RefreshLocalStateForNode(runtimeIndex, *transform);
        }
    }
}

void SceneRuntimeData::RefreshLocalStateForNode(uint32_t runtimeIndex, const TransformComponent& transform)
{
    if (runtimeIndex >= m_nodes.size()) {
        return;
    }

    m_localTransforms[runtimeIndex] = transform.localTransform;
    m_animatedTransforms[runtimeIndex] = transform.animatedTransform;
    m_hasAnimation[runtimeIndex] = transform.hasAnimation ? 1u : 0u;
    m_nodes[runtimeIndex].transformID = transform.transformID;
    DecomposeLocalTransform(transform.localTransform,
                            m_localTranslations[runtimeIndex],
                            m_localRotations[runtimeIndex],
                            m_localScales[runtimeIndex]);
}

std::vector<SceneRuntimeData::DirtySpan> SceneRuntimeData::BuildDirtySpans(const std::vector<EntityID>& dirtyRoots) const
{
    std::vector<DirtySpan> spans;
    spans.reserve(dirtyRoots.size());

    for (EntityID entity : dirtyRoots) {
        const uint32_t runtimeIndex = FindRuntimeIndex(entity);
        if (runtimeIndex == INVALID_RUNTIME_NODE_INDEX) {
            continue;
        }

        const RuntimeTransformNode& node = m_nodes[runtimeIndex];
        spans.push_back({ runtimeIndex, node.subtreeBegin, node.subtreeEnd, node.depth });
    }

    std::sort(spans.begin(), spans.end(), [](const DirtySpan& a, const DirtySpan& b) {
        if (a.begin != b.begin) {
            return a.begin < b.begin;
        }
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.end > b.end;
    });

    std::vector<DirtySpan> filtered;
    filtered.reserve(spans.size());
    uint32_t maxCoveredEnd = 0;
    bool hasCoverage = false;
    for (const DirtySpan& span : spans) {
        if (hasCoverage && span.end <= maxCoveredEnd) {
            continue;
        }

        filtered.push_back(span);
        if (!hasCoverage || span.end > maxCoveredEnd) {
            maxCoveredEnd = span.end;
            hasCoverage = true;
        }
    }

    return filtered;
}

void SceneRuntimeData::DecomposeLocalTransform(const glm::mat4& localTransform,
                                               glm::vec3& translation,
                                               glm::quat& rotation,
                                               glm::vec3& scale) const
{
    glm::vec3 skew(0.0f);
    glm::vec4 perspective(0.0f);
    glm::decompose(localTransform, scale, rotation, translation, skew, perspective);
    if (glm::length(rotation) > 0.0f) {
        rotation = glm::normalize(rotation);
    } else {
        rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
}
