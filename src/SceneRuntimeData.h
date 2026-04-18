#pragma once

#include "ComponentManager.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <unordered_map>
#include <cstdint>

constexpr uint32_t INVALID_RUNTIME_NODE_INDEX = UINT32_MAX;

struct RuntimeTransformNode {
    EntityID entity = INVALID_ENTITY;
    uint32_t parent = INVALID_RUNTIME_NODE_INDEX;
    uint32_t firstChild = INVALID_RUNTIME_NODE_INDEX;
    uint32_t nextSibling = INVALID_RUNTIME_NODE_INDEX;
    uint32_t subtreeBegin = 0;
    uint32_t subtreeEnd = 0;
    uint32_t depth = 0;
    uint32_t worldGeneration = 0;
    uint32_t dirtyGeneration = 0;
    TransformID transformID = INVALID_TRANSFORM_ID;
};

class SceneRuntimeData {
public:
    struct Diagnostics {
        float evaluateDirtySubtreesMs = 0.0f;
        size_t lastDirtyRootInputCount = 0;
        size_t lastDirtySpanCount = 0;
        size_t lastDirtySpanCoverageNodes = 0;
        size_t lastRecomputedNodes = 0;
    };

    explicit SceneRuntimeData(ComponentManager* componentManager = nullptr);

    void SetComponentManager(ComponentManager* componentManager);
    void MarkHierarchyDirty();
    bool EnsureCompiled();

    uint32_t FindRuntimeIndex(EntityID entity) const;
    const RuntimeTransformNode* GetRuntimeNode(EntityID entity) const;
    const RuntimeTransformNode* GetRuntimeNodeByIndex(uint32_t index) const;

    const glm::mat4& GetWorldTransform(EntityID entity) const;
    const glm::mat4& GetWorldTransformByIndex(uint32_t index) const;
    const glm::mat4& GetPreviousWorldTransformByIndex(uint32_t index) const;
    const glm::mat4& GetLocalTransformByIndex(uint32_t index) const;

    const std::vector<RuntimeTransformNode>& GetNodes() const { return m_nodes; }
    const std::vector<uint32_t>& GetTraversalOrder() const { return m_traversalOrder; }
    const std::vector<glm::mat4>& GetWorldTransforms() const { return m_worldTransforms; }
    size_t GetNodeCount() const { return m_nodes.size(); }
    uint64_t GetCompiledHierarchyRevision() const { return m_compiledHierarchyRevision; }
    uint64_t GetCompileCount() const { return m_compileCount; }
    uint32_t GetLatestWorldGeneration() const { return m_latestWorldGeneration; }
    const Diagnostics& GetDiagnostics() const { return m_diagnostics; }

    size_t EvaluateDirtySubtrees(const std::vector<EntityID>& dirtyRoots,
                                 std::vector<EntityID>* changedEntities,
                                 std::vector<uint32_t>* changedRuntimeIndices);

private:
    struct DirtySpan {
        uint32_t rootIndex = INVALID_RUNTIME_NODE_INDEX;
        uint32_t begin = 0;
        uint32_t end = 0;
        uint32_t depth = 0;
    };

    ComponentManager* m_componentManager = nullptr;

    std::vector<RuntimeTransformNode> m_nodes;
    std::vector<uint32_t> m_traversalOrder;
    std::unordered_map<EntityID, uint32_t> m_entityToRuntime;

    std::vector<glm::mat4> m_localTransforms;
    std::vector<glm::mat4> m_worldTransforms;
    std::vector<glm::mat4> m_prevWorldTransforms;
    std::vector<glm::mat4> m_animatedTransforms;
    std::vector<glm::vec3> m_localTranslations;
    std::vector<glm::quat> m_localRotations;
    std::vector<glm::vec3> m_localScales;
    std::vector<uint8_t> m_hasAnimation;

    uint64_t m_compiledHierarchyRevision = 0;
    uint64_t m_compileCount = 0;
    uint32_t m_latestWorldGeneration = 0;
    Diagnostics m_diagnostics;

    void Rebuild();
    void RefreshLocalStateForSubtree(uint32_t rootIndex);
    void RefreshLocalStateForNode(uint32_t runtimeIndex, const TransformComponent& transform);
    std::vector<DirtySpan> BuildDirtySpans(const std::vector<EntityID>& dirtyRoots) const;
    void DecomposeLocalTransform(const glm::mat4& localTransform,
                                 glm::vec3& translation,
                                 glm::quat& rotation,
                                 glm::vec3& scale) const;
};
