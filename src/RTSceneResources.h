#pragma once

#include <GL/glew.h>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "ComponentTypes.h"
#include "RTStructures.h"

class LightManager;
class MeshComponent;
class SceneGraph;
class SceneNode;

class RTSceneResources {
public:
    struct IncrementalBuildSettings {
        double maxBuildMsPerFrame = 2.0;
        std::size_t maxBlasTrianglesPerFrame = 50000;
        std::size_t maxResidentBytes = 512ull * 1024ull * 1024ull;
        bool includeSkinnedMeshes = false;
    };

    struct Diagnostics {
        std::size_t estimatedSceneTriangles = 0;
        std::size_t blasQueued = 0;
        std::size_t blasReady = 0;
        std::size_t blasFailed = 0;
        std::size_t tlasInstances = 0;
        std::size_t traceableInstances = 0;
        std::size_t tlasNodes = 0;
        std::size_t skippedSkinnedInstances = 0;
        std::size_t residentBytes = 0;
        std::size_t trianglesBuiltThisFrame = 0;
        double buildMsThisFrame = 0.0;
        bool incrementalReady = false;
    };

    RTSceneResources() = default;
    ~RTSceneResources();

    RTSceneResources(const RTSceneResources&) = delete;
    RTSceneResources& operator=(const RTSceneResources&) = delete;

    bool EnsureBuilt(const std::shared_ptr<SceneGraph>& sceneGraph, bool forceRebuild = false, std::size_t maxTriangleCount = 0);
    bool EnsureIncrementalBLAS(const std::shared_ptr<SceneGraph>& sceneGraph, const IncrementalBuildSettings& settings);
    void UpdateLights(const std::shared_ptr<LightManager>& lightManager);
    void BindForTracing(GLuint triangleBinding, GLuint bvhBinding) const;
    void BindIncrementalForTracing(GLuint triangleBinding, GLuint bvhBinding, GLuint instanceBinding, GLuint instanceNodeBinding) const;
    void BindLights(GLuint lightBinding) const;
    void Release();

    GLuint GetTriangleSSBO() const { return m_triangleSSBO; }
    GLuint GetBVHSSBO() const { return m_bvhSSBO; }
    GLuint GetLightSSBO() const { return m_lightSSBO; }
    std::size_t GetTriangleCount() const { return m_triangleCount; }
    std::size_t GetNodeCount() const { return m_nodeCount; }
    std::size_t GetLightCount() const { return m_lightCount; }
    bool IsReady() const { return m_triangleSSBO != 0 && m_bvhSSBO != 0 && m_triangleCount > 0 && m_nodeCount > 0; }
    bool IsIncrementalReady() const { return m_incrementalTriangleSSBO != 0 && m_incrementalBVHSSBO != 0 && m_instanceSSBO != 0 && m_instanceNodeSSBO != 0 && m_incrementalTriangleCount > 0 && m_incrementalNodeCount > 0 && m_incrementalInstanceCount > 0 && m_incrementalInstanceNodeCount > 0; }
    std::size_t GetLastEstimatedTriangleCount() const { return m_lastEstimatedTriangleCount; }
    std::size_t GetIncrementalTriangleCount() const { return m_incrementalTriangleCount; }
    std::size_t GetIncrementalNodeCount() const { return m_incrementalNodeCount; }
    std::size_t GetIncrementalInstanceCount() const { return m_incrementalInstanceCount; }
    std::size_t GetIncrementalInstanceNodeCount() const { return m_incrementalInstanceNodeCount; }
    const Diagnostics& GetDiagnostics() const { return m_diagnostics; }

private:
    struct BLASRecord {
        const MeshComponent* mesh = nullptr;
        RT::BVHData bvh;
        std::size_t triangleCount = 0;
        std::size_t residentBytes = 0;
        std::size_t nodeOffset = 0;
        std::size_t triangleOffset = 0;
        bool ready = false;
        bool failed = false;
        bool uploaded = false;
    };

    struct TLASInstance {
        const MeshComponent* mesh = nullptr;
        std::size_t blasIndex = 0;
        glm::mat4 worldTransform{ 1.0f };
        glm::mat4 inverseWorldTransform{ 1.0f };
        glm::vec3 worldBoundsMin{ 0.0f };
        glm::vec3 worldBoundsMax{ 0.0f };
        bool isSkinnedMesh = false;
    };

    struct EntityInstanceRange {
        std::size_t begin = 0;
        std::size_t count = 0;
    };

    struct IncrementalInstanceCacheKey {
        EntityID entity = INVALID_ENTITY;
        const MeshComponent* mesh = nullptr;

        bool operator==(const IncrementalInstanceCacheKey& other) const
        {
            return entity == other.entity && mesh == other.mesh;
        }
    };

    struct IncrementalInstanceCacheKeyHash {
        std::size_t operator()(const IncrementalInstanceCacheKey& key) const noexcept
        {
            std::size_t hash = std::hash<EntityID>{}(key.entity);
            hash ^= std::hash<const MeshComponent*>{}(key.mesh) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    struct CachedTLASInstance {
        TLASInstance instance;
        glm::vec3 sourceBoundsMin{ 0.0f };
        glm::vec3 sourceBoundsMax{ 0.0f };
    };

    void EnsureBuffers();
    void EnsureIncrementalBuffers();
    static std::size_t EstimateSceneTriangleCount(const std::shared_ptr<SceneGraph>& sceneGraph);
    void RebuildIncrementalInstanceList(const std::shared_ptr<SceneGraph>& sceneGraph, const IncrementalBuildSettings& settings);
    void UploadIncrementalScene();
    void BuildInstanceTLAS(const std::vector<RT::Instance>& instances, std::vector<RT::InstanceNode>& nodes) const;
    static void TransformBounds(const glm::vec3& localMin, const glm::vec3& localMax, const glm::mat4& transform, glm::vec3& worldMin, glm::vec3& worldMax);
    void RefreshIncrementalDiagnostics(double buildMsThisFrame, std::size_t trianglesBuiltThisFrame);

    GLuint m_triangleSSBO = 0;
    GLuint m_bvhSSBO = 0;
    GLuint m_incrementalTriangleSSBO = 0;
    GLuint m_incrementalBVHSSBO = 0;
    GLuint m_instanceSSBO = 0;
    GLuint m_instanceNodeSSBO = 0;
    GLuint m_lightSSBO = 0;
    std::size_t m_triangleCount = 0;
    std::size_t m_nodeCount = 0;
    std::size_t m_lightCount = 0;
    std::size_t m_incrementalTriangleCount = 0;
    std::size_t m_incrementalNodeCount = 0;
    std::size_t m_incrementalInstanceCount = 0;
    std::size_t m_incrementalInstanceNodeCount = 0;
    std::size_t m_lastEstimatedTriangleCount = 0;
    std::size_t m_lastSkippedTriangleCount = 0;
    std::size_t m_lastSkippedTriangleCap = 0;
    bool m_hasBuiltScene = false;
    bool m_loggedOversizedScene = false;
    const SceneGraph* m_cachedIncrementalSceneGraph = nullptr;
    const SceneNode* m_cachedIncrementalRoot = nullptr;
    uint64_t m_cachedHierarchyRevision = 0;
    uint64_t m_cachedRenderableRevision = 0;
    uint64_t m_cachedTransformPublication = 0;
    bool m_cachedIncludeSkinnedMeshes = false;
    bool m_incrementalInstanceTopologyDirty = false;

    std::vector<RT::Triangle> m_incrementalPackedTriangles;
    std::vector<RT::BVHNode> m_incrementalPackedNodes;
    std::size_t m_incrementalTriangleBufferCapacityBytes = 0;
    std::size_t m_incrementalNodeBufferCapacityBytes = 0;
    std::size_t m_incrementalUploadedTriangleCount = 0;
    std::size_t m_incrementalUploadedNodeCount = 0;

    std::unordered_map<const MeshComponent*, std::size_t> m_blasLookup;
    std::vector<BLASRecord> m_blasRecords;
    std::deque<std::size_t> m_pendingBLAS;
    std::vector<TLASInstance> m_tlasInstances;
    std::unordered_map<EntityID, EntityInstanceRange> m_entityInstanceRanges;
    std::unordered_map<IncrementalInstanceCacheKey, CachedTLASInstance, IncrementalInstanceCacheKeyHash> m_incrementalInstanceCache;
    std::size_t m_incrementalResidentBytes = 0;
    std::size_t m_skippedSkinnedInstances = 0;
    bool m_incrementalUploadDirty = false;
    Diagnostics m_diagnostics;
};
