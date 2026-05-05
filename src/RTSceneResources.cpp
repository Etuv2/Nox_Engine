#include "RTSceneResources.h"
#include "BVHBuilder.h"
#include "LightManager.h"
#include "Scene.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <numeric>

namespace {
bool Vec3Equals(const glm::vec3& lhs, const glm::vec3& rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool Mat4Equals(const glm::mat4& lhs, const glm::mat4& rhs)
{
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (lhs[column][row] != rhs[column][row]) {
                return false;
            }
        }
    }
    return true;
}

void GetMeshBoundsSignature(const MeshComponent& mesh, glm::vec3& boundsMin, glm::vec3& boundsMax)
{
    if (mesh.boundingVolumeValid) {
        boundsMin = mesh.boundingMin;
        boundsMax = mesh.boundingMax;
    }
    else {
        boundsMin = glm::vec3(-mesh.boundingRadius);
        boundsMax = glm::vec3(mesh.boundingRadius);
    }
}

template <typename Fn>
void ForEachRenderableMesh(const RenderableComponent& renderable, Fn&& fn)
{
    if (!renderable.model) {
        return;
    }

    if (renderable.renderWholeModel) {
        for (const auto& mesh : renderable.model->meshes) {
            fn(mesh);
        }
        return;
    }

    for (uint32_t meshIndex : renderable.meshIndices) {
        if (meshIndex < renderable.model->meshes.size()) {
            fn(renderable.model->meshes[meshIndex]);
        }
    }
}

glm::mat4 ResolveRenderableMeshLocalTransform(const RenderableComponent& renderable, const MeshComponent& mesh)
{
    if (!renderable.renderWholeModel || mesh.sourceNodeIndex < 0 || !renderable.model) {
        return glm::mat4(1.0f);
    }

    const int referenceNodeIndex = renderable.nodeIndex;
    if (referenceNodeIndex < 0 || referenceNodeIndex == mesh.sourceNodeIndex) {
        return mesh.localTransform;
    }

    const auto& nodeWorldTransforms = renderable.model->GetNodeWorldTransforms();
    if (referenceNodeIndex >= static_cast<int>(nodeWorldTransforms.size()) ||
        mesh.sourceNodeIndex >= static_cast<int>(nodeWorldTransforms.size())) {
        return mesh.localTransform;
    }

    glm::mat4 referenceInverse = glm::inverse(nodeWorldTransforms[referenceNodeIndex]);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(referenceInverse[column][row])) {
                return mesh.localTransform;
            }
        }
    }

    return referenceInverse * mesh.localTransform;
}

template <typename T>
void UploadPackedBuffer(GLuint buffer,
    GLenum target,
    std::size_t& capacityBytes,
    std::vector<T>& packedData,
    std::size_t& uploadedCount)
{
    const std::size_t elementCount = packedData.size();
    const std::size_t requiredBytes = elementCount * sizeof(T);
    const std::size_t uploadedBytes = uploadedCount * sizeof(T);

    glBindBuffer(target, buffer);

    if (requiredBytes == 0) {
        if (capacityBytes != 0) {
            glBufferData(target, 0, nullptr, GL_DYNAMIC_DRAW);
        }
        capacityBytes = 0;
        uploadedCount = 0;
        return;
    }

    if (requiredBytes < uploadedBytes || requiredBytes > capacityBytes) {
        std::size_t newCapacity = capacityBytes > 0 ? capacityBytes : requiredBytes;
        while (newCapacity < requiredBytes) {
            newCapacity = std::max(newCapacity * 2, requiredBytes);
        }

        glBufferData(target,
            static_cast<GLsizeiptr>(newCapacity),
            nullptr,
            GL_DYNAMIC_DRAW);
        glBufferSubData(target,
            0,
            static_cast<GLsizeiptr>(requiredBytes),
            packedData.data());

        capacityBytes = newCapacity;
        uploadedCount = elementCount;
        return;
    }

    if (requiredBytes > uploadedBytes) {
        glBufferSubData(target,
            static_cast<GLintptr>(uploadedBytes),
            static_cast<GLsizeiptr>(requiredBytes - uploadedBytes),
            packedData.data() + uploadedCount);
        uploadedCount = elementCount;
    }
}
}

RTSceneResources::~RTSceneResources()
{
    Release();
}

void RTSceneResources::EnsureBuffers()
{
    if (m_triangleSSBO == 0) {
        glGenBuffers(1, &m_triangleSSBO);
    }
    if (m_bvhSSBO == 0) {
        glGenBuffers(1, &m_bvhSSBO);
    }
}

void RTSceneResources::EnsureIncrementalBuffers()
{
    if (m_incrementalTriangleSSBO == 0) {
        glGenBuffers(1, &m_incrementalTriangleSSBO);
    }
    if (m_incrementalBVHSSBO == 0) {
        glGenBuffers(1, &m_incrementalBVHSSBO);
    }
    if (m_instanceSSBO == 0) {
        glGenBuffers(1, &m_instanceSSBO);
    }
    if (m_instanceNodeSSBO == 0) {
        glGenBuffers(1, &m_instanceNodeSSBO);
    }
}

std::size_t RTSceneResources::EstimateSceneTriangleCount(const std::shared_ptr<SceneGraph>& sceneGraph)
{
    const ComponentManager* componentManager = sceneGraph ? sceneGraph->GetComponentManager() : nullptr;
    if (!componentManager) {
        return 0;
    }

    std::size_t triangleCount = 0;
    const auto& renderablePool = componentManager->GetRenderablePool();
    for (const auto& entry : renderablePool) {
        const RenderableComponent& renderable = entry.component;
        if (!renderable.model) {
            continue;
        }

        ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
            const std::size_t indexCount = !mesh.rawIndices.empty() ? mesh.rawIndices.size() : mesh.indexCount;
            if (indexCount < 3u || mesh.rawVertices.empty()) {
                return;
            }
            triangleCount += indexCount / 3u;
        });
    }

    return triangleCount;
}

bool RTSceneResources::EnsureBuilt(const std::shared_ptr<SceneGraph>& sceneGraph, bool forceRebuild, std::size_t maxTriangleCount)
{
    if (!sceneGraph) {
        return false;
    }

    if (m_hasBuiltScene && !forceRebuild && IsReady()) {
        return true;
    }

    m_lastEstimatedTriangleCount = EstimateSceneTriangleCount(sceneGraph);
    if (maxTriangleCount > 0 &&
        m_lastSkippedTriangleCount == m_lastEstimatedTriangleCount &&
        m_lastSkippedTriangleCap == maxTriangleCount) {
        return false;
    }
    if (maxTriangleCount > 0 && m_lastEstimatedTriangleCount > maxTriangleCount) {
        if (!m_loggedOversizedScene) {
            std::cerr << "[RTSceneResources] Skipping shared RT BVH build for Surfel GI: scene has approximately "
                << m_lastEstimatedTriangleCount << " triangles, cap is " << maxTriangleCount
                << ". Increase RenderContext::surfelGIRTMaxTriangles or use a smaller scene to enable surfel RT rays.\n";
            m_loggedOversizedScene = true;
        }
        m_lastSkippedTriangleCount = m_lastEstimatedTriangleCount;
        m_lastSkippedTriangleCap = maxTriangleCount;
        return false;
    }

    EnsureBuffers();

    BVHBuilder::BuildParams params;
    params.maxLeafPrimitives = 2;
    params.maxDepth = 24;
    params.sahBuckets = 32;

    RT::BVHData bvhData = BVHBuilder::BuildFromScene(sceneGraph, params);
    if (bvhData.triangles.empty() || bvhData.nodes.empty()) {
        std::cerr << "[RTSceneResources] Failed to build shared RT scene - no geometry.\n";
        m_triangleCount = 0;
        m_nodeCount = 0;
        m_hasBuiltScene = false;
        return false;
    }

    m_triangleCount = bvhData.triangles.size();
    m_nodeCount = bvhData.nodes.size();

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_triangleSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bvhData.GetTriangleBufferSize(), bvhData.triangles.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_bvhSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bvhData.GetNodeBufferSize(), bvhData.nodes.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    m_hasBuiltScene = true;
    m_loggedOversizedScene = false;
    m_lastSkippedTriangleCount = 0;
    m_lastSkippedTriangleCap = 0;
    std::cout << "[RTSceneResources] Shared BVH uploaded: "
        << m_triangleCount << " triangles, "
        << m_nodeCount << " nodes, max depth "
        << bvhData.maxDepth << "\n";
    return true;
}

void RTSceneResources::TransformBounds(
    const glm::vec3& localMin,
    const glm::vec3& localMax,
    const glm::mat4& transform,
    glm::vec3& worldMin,
    glm::vec3& worldMax)
{
    const glm::vec3 corners[8] = {
        { localMin.x, localMin.y, localMin.z },
        { localMax.x, localMin.y, localMin.z },
        { localMin.x, localMax.y, localMin.z },
        { localMax.x, localMax.y, localMin.z },
        { localMin.x, localMin.y, localMax.z },
        { localMax.x, localMin.y, localMax.z },
        { localMin.x, localMax.y, localMax.z },
        { localMax.x, localMax.y, localMax.z },
    };

    worldMin = glm::vec3(std::numeric_limits<float>::max());
    worldMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const glm::vec3& corner : corners) {
        const glm::vec3 worldCorner = glm::vec3(transform * glm::vec4(corner, 1.0f));
        worldMin = glm::min(worldMin, worldCorner);
        worldMax = glm::max(worldMax, worldCorner);
    }
}

void RTSceneResources::RebuildIncrementalInstanceList(
    const std::shared_ptr<SceneGraph>& sceneGraph,
    const IncrementalBuildSettings& settings)
{
    const ComponentManager* componentManager = sceneGraph ? sceneGraph->GetComponentManager() : nullptr;
    const uint64_t hierarchyRevision = componentManager ? componentManager->GetHierarchyRevision() : 0;
    const uint64_t renderableRevision = componentManager ? componentManager->GetRenderableRevision() : 0;
    TransformSystem* transformSystem = sceneGraph ? sceneGraph->GetTransformSystem() : nullptr;
    const uint64_t transformPublication = transformSystem ? transformSystem->GetWorldPublicationGeneration() : 0;

    if (!sceneGraph || !componentManager || !transformSystem) {
        m_tlasInstances.clear();
        m_entityInstanceRanges.clear();
        m_lastEstimatedTriangleCount = 0;
        m_skippedSkinnedInstances = 0;
        m_incrementalInstanceTopologyDirty = true;
        m_cachedIncrementalSceneGraph = sceneGraph.get();
        m_cachedIncrementalRoot = nullptr;
        m_cachedHierarchyRevision = hierarchyRevision;
        m_cachedRenderableRevision = renderableRevision;
        m_cachedTransformPublication = transformPublication;
        m_cachedIncludeSkinnedMeshes = settings.includeSkinnedMeshes;
        return;
    }
    const bool sceneSignatureChanged =
        !sceneGraph ||
        !componentManager ||
        !transformSystem ||
        hierarchyRevision != m_cachedHierarchyRevision ||
        renderableRevision != m_cachedRenderableRevision ||
        settings.includeSkinnedMeshes != m_cachedIncludeSkinnedMeshes;

    auto updateInstanceFromRenderable = [&](TLASInstance& instance,
        const RenderableComponent& renderable,
        const MeshComponent& mesh,
        const glm::mat4& entityWorldTransform,
        std::size_t blasIndex,
        bool isSkinnedMesh) {
        const glm::mat4 meshLocalTransform = ResolveRenderableMeshLocalTransform(renderable, mesh);
        const glm::mat4 instanceWorldTransform = entityWorldTransform * meshLocalTransform;
        instance.mesh = &mesh;
        instance.blasIndex = blasIndex;
        instance.worldTransform = instanceWorldTransform;
        instance.inverseWorldTransform = glm::inverse(instanceWorldTransform);
        instance.isSkinnedMesh = isSkinnedMesh;

        const glm::vec3 localMin = mesh.boundingVolumeValid ? mesh.boundingMin : glm::vec3(-mesh.boundingRadius);
        const glm::vec3 localMax = mesh.boundingVolumeValid ? mesh.boundingMax : glm::vec3(mesh.boundingRadius);
        TransformBounds(localMin, localMax, instanceWorldTransform, instance.worldBoundsMin, instance.worldBoundsMax);
    };

    auto ensureBlasRecord = [&](const MeshComponent* meshKey, const MeshComponent& mesh) {
        auto [lookupIt, inserted] = m_blasLookup.try_emplace(meshKey, m_blasRecords.size());
        if (inserted) {
            BLASRecord record;
            record.mesh = meshKey;
            record.triangleCount = mesh.rawIndices.size() / 3;
            const std::size_t recordIndex = m_blasRecords.size();
            m_blasRecords.push_back(std::move(record));
            m_pendingBLAS.push_back(recordIndex);
        }
        return lookupIt->second;
    };

    auto rebuildAllInstances = [&]() {
        m_tlasInstances.clear();
        m_entityInstanceRanges.clear();
        m_skippedSkinnedInstances = 0;
        m_incrementalInstanceTopologyDirty = true;

        const auto& renderablePool = componentManager->GetRenderablePool();
        std::size_t estimatedInstanceCount = 0;
        std::size_t estimatedTriangleCount = 0;
        for (const auto& entry : renderablePool) {
            const RenderableComponent& renderable = entry.component;
            if (!renderable.model) {
                continue;
            }

            ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
                const bool isSkinnedMesh = renderable.isSkinned || mesh.morphTargetCount > 0;
                if (isSkinnedMesh && !settings.includeSkinnedMeshes) {
                    ++m_skippedSkinnedInstances;
                    return;
                }
                if (mesh.rawVertices.empty() || mesh.rawIndices.empty()) {
                    return;
                }

                ++estimatedInstanceCount;
                estimatedTriangleCount += mesh.rawIndices.size() / 3;
                const MeshComponent* meshKey = &mesh;
                (void)ensureBlasRecord(meshKey, mesh);
            });
        }

        m_lastEstimatedTriangleCount = estimatedTriangleCount;
        m_tlasInstances.reserve(estimatedInstanceCount);
        m_entityInstanceRanges.reserve(renderablePool.Size());

        for (const auto& entry : renderablePool) {
            const EntityID entity = entry.entity;
            const RenderableComponent& renderable = entry.component;
            if (!renderable.model) {
                continue;
            }

            const glm::mat4 entityWorldTransform = transformSystem->GetWorldTransform(entity);
            const std::size_t rangeBegin = m_tlasInstances.size();
            std::size_t instanceCount = 0;

            ForEachRenderableMesh(renderable, [&](const MeshComponent& mesh) {
                const bool isSkinnedMesh = renderable.isSkinned || mesh.morphTargetCount > 0;
                if (isSkinnedMesh && !settings.includeSkinnedMeshes) {
                    return;
                }
                if (mesh.rawVertices.empty() || mesh.rawIndices.empty()) {
                    return;
                }

                const MeshComponent* meshKey = &mesh;
                const std::size_t blasIndex = ensureBlasRecord(meshKey, mesh);

                TLASInstance instance;
                updateInstanceFromRenderable(instance, renderable, mesh, entityWorldTransform, blasIndex, isSkinnedMesh);
                m_tlasInstances.push_back(instance);
                ++instanceCount;
            });

            if (instanceCount > 0) {
                m_entityInstanceRanges.emplace(entity, EntityInstanceRange{ rangeBegin, instanceCount });
            }
        }
    };

    if (sceneSignatureChanged || m_tlasInstances.empty() || m_entityInstanceRanges.empty()) {
        rebuildAllInstances();
        m_cachedHierarchyRevision = hierarchyRevision;
        m_cachedRenderableRevision = renderableRevision;
        m_cachedTransformPublication = transformPublication;
        m_cachedIncludeSkinnedMeshes = settings.includeSkinnedMeshes;
        m_cachedIncrementalSceneGraph = sceneGraph.get();
        m_cachedIncrementalRoot = nullptr;
        return;
    }

    if (transformPublication != m_cachedTransformPublication) {
        const auto& changedEntities = transformSystem->GetLastChangedEntities();
        bool anyUpdated = false;
        bool requiresFullRebuild = false;

        for (EntityID entity : changedEntities) {
            const auto rangeIt = m_entityInstanceRanges.find(entity);
            if (rangeIt == m_entityInstanceRanges.end()) {
                continue;
            }

            const RenderableComponent* renderable = componentManager->GetRenderable(entity);
            if (!renderable || !renderable->model) {
                requiresFullRebuild = true;
                break;
            }

            const glm::mat4 entityWorldTransform = transformSystem->GetWorldTransform(entity);
            std::size_t instanceIndex = rangeIt->second.begin;
            std::size_t updatedCount = 0;

            ForEachRenderableMesh(*renderable, [&](const MeshComponent& mesh) {
                const bool isSkinnedMesh = renderable->isSkinned || mesh.morphTargetCount > 0;
                if (isSkinnedMesh && !settings.includeSkinnedMeshes) {
                    return;
                }
                if (mesh.rawVertices.empty() || mesh.rawIndices.empty()) {
                    return;
                }

                if (instanceIndex >= m_tlasInstances.size()) {
                    requiresFullRebuild = true;
                    return;
                }

                const std::size_t blasIndex = m_tlasInstances[instanceIndex].blasIndex;
                updateInstanceFromRenderable(m_tlasInstances[instanceIndex], *renderable, mesh, entityWorldTransform, blasIndex, isSkinnedMesh);
                ++instanceIndex;
                ++updatedCount;
            });

            if (requiresFullRebuild) {
                break;
            }

            if (updatedCount != rangeIt->second.count) {
                requiresFullRebuild = true;
                break;
            }

            anyUpdated = true;
        }

        if (requiresFullRebuild) {
            rebuildAllInstances();
        }
        else if (anyUpdated) {
            m_incrementalInstanceTopologyDirty = true;
        }

        m_cachedTransformPublication = transformPublication;
        m_cachedHierarchyRevision = hierarchyRevision;
        m_cachedRenderableRevision = renderableRevision;
        m_cachedIncludeSkinnedMeshes = settings.includeSkinnedMeshes;
        m_cachedIncrementalSceneGraph = sceneGraph.get();
        m_cachedIncrementalRoot = nullptr;
    }
}

void RTSceneResources::RefreshIncrementalDiagnostics(double buildMsThisFrame, std::size_t trianglesBuiltThisFrame)
{
    m_diagnostics.estimatedSceneTriangles = m_lastEstimatedTriangleCount;
    m_diagnostics.blasQueued = m_pendingBLAS.size();
    m_diagnostics.blasReady = 0;
    m_diagnostics.blasFailed = 0;
    for (const BLASRecord& record : m_blasRecords) {
        if (record.ready) {
            ++m_diagnostics.blasReady;
        }
        if (record.failed) {
            ++m_diagnostics.blasFailed;
        }
    }
    m_diagnostics.tlasInstances = m_tlasInstances.size();
    m_diagnostics.traceableInstances = m_incrementalInstanceCount;
    m_diagnostics.tlasNodes = m_incrementalInstanceNodeCount;
    m_diagnostics.skippedSkinnedInstances = m_skippedSkinnedInstances;
    m_diagnostics.residentBytes = m_incrementalResidentBytes;
    m_diagnostics.trianglesBuiltThisFrame = trianglesBuiltThisFrame;
    m_diagnostics.buildMsThisFrame = buildMsThisFrame;
    m_diagnostics.incrementalReady = !m_blasRecords.empty() && m_pendingBLAS.empty() && m_diagnostics.blasReady > 0;
}

void RTSceneResources::BuildInstanceTLAS(const std::vector<RT::Instance>& instances, std::vector<RT::InstanceNode>& nodes) const
{
    nodes.clear();
    if (instances.empty()) {
        return;
    }

    std::vector<int> indices(instances.size());
    std::iota(indices.begin(), indices.end(), 0);

    auto computeBounds = [&](size_t begin, size_t end, glm::vec3& minBounds, glm::vec3& maxBounds) {
        minBounds = glm::vec3(std::numeric_limits<float>::max());
        maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
        for (size_t i = begin; i < end; ++i) {
            const RT::Instance& instance = instances[static_cast<size_t>(indices[i])];
            minBounds = glm::min(minBounds, glm::vec3(instance.boundsMin));
            maxBounds = glm::max(maxBounds, glm::vec3(instance.boundsMax));
        }
    };

    std::function<int(size_t, size_t)> buildRange;
    buildRange = [&](size_t begin, size_t end) -> int {
        glm::vec3 minBounds;
        glm::vec3 maxBounds;
        computeBounds(begin, end, minBounds, maxBounds);

        const int nodeIndex = static_cast<int>(nodes.size());
        RT::InstanceNode node;
        node.boundsMin = glm::vec4(minBounds, 0.0f);
        node.boundsMax = glm::vec4(maxBounds, 0.0f);
        node.children = glm::ivec4(-1);
        node.instances = glm::ivec4(-1);
        nodes.push_back(node);

        const size_t count = end - begin;
        if (count <= 4) {
            for (size_t i = 0; i < count; ++i) {
                nodes[static_cast<size_t>(nodeIndex)].instances[static_cast<int>(i)] = indices[begin + i];
            }
            return nodeIndex;
        }

        const glm::vec3 extent = maxBounds - minBounds;
        const int axis = (extent.x >= extent.y && extent.x >= extent.z) ? 0 : (extent.y >= extent.z ? 1 : 2);
        const size_t mid = begin + count / 2;
        std::nth_element(indices.begin() + static_cast<std::ptrdiff_t>(begin),
            indices.begin() + static_cast<std::ptrdiff_t>(mid),
            indices.begin() + static_cast<std::ptrdiff_t>(end),
            [&](int a, int b) {
                const glm::vec3 centerA = (glm::vec3(instances[static_cast<size_t>(a)].boundsMin) + glm::vec3(instances[static_cast<size_t>(a)].boundsMax)) * 0.5f;
                const glm::vec3 centerB = (glm::vec3(instances[static_cast<size_t>(b)].boundsMin) + glm::vec3(instances[static_cast<size_t>(b)].boundsMax)) * 0.5f;
                return centerA[axis] < centerB[axis];
            });

        const int left = buildRange(begin, mid);
        const int right = buildRange(mid, end);
        nodes[static_cast<size_t>(nodeIndex)].children = glm::ivec4(left, right, -1, -1);
        return nodeIndex;
    };

    nodes.reserve(instances.size() * 2);
    buildRange(0, indices.size());
}

void RTSceneResources::UploadIncrementalScene()
{
    EnsureIncrementalBuffers();

    const bool geometryDirty = m_incrementalUploadDirty || m_incrementalTriangleCount == 0 || m_incrementalNodeCount == 0;
    const bool topologyDirty = m_incrementalInstanceTopologyDirty || m_incrementalInstanceCount == 0 || m_incrementalInstanceNodeCount == 0;

    if (!geometryDirty && !topologyDirty) {
        return;
    }

    bool geometryUpdated = false;
    if (geometryDirty) {
        for (BLASRecord& record : m_blasRecords) {
            if (!record.ready || record.uploaded || record.bvh.triangles.empty() || record.bvh.nodes.empty()) {
                continue;
            }
            record.nodeOffset = m_incrementalPackedNodes.size();
            record.triangleOffset = m_incrementalPackedTriangles.size();
            m_incrementalPackedNodes.insert(m_incrementalPackedNodes.end(), record.bvh.nodes.begin(), record.bvh.nodes.end());
            m_incrementalPackedTriangles.insert(m_incrementalPackedTriangles.end(), record.bvh.triangles.begin(), record.bvh.triangles.end());
            record.uploaded = true;
            geometryUpdated = true;
        }

        if (geometryUpdated) {
            m_incrementalTriangleCount = m_incrementalPackedTriangles.size();
            m_incrementalNodeCount = m_incrementalPackedNodes.size();

            UploadPackedBuffer(
                m_incrementalTriangleSSBO,
                GL_SHADER_STORAGE_BUFFER,
                m_incrementalTriangleBufferCapacityBytes,
                m_incrementalPackedTriangles,
                m_incrementalUploadedTriangleCount);
            UploadPackedBuffer(
                m_incrementalBVHSSBO,
                GL_SHADER_STORAGE_BUFFER,
                m_incrementalNodeBufferCapacityBytes,
                m_incrementalPackedNodes,
                m_incrementalUploadedNodeCount);
        }
    }

    const std::size_t previousTraceableInstanceCount = m_incrementalInstanceCount;
    std::vector<RT::Instance> instances;
    instances.reserve(m_tlasInstances.size());
    for (const TLASInstance& source : m_tlasInstances) {
        if (source.blasIndex >= m_blasRecords.size()) {
            continue;
        }
        const BLASRecord& record = m_blasRecords[source.blasIndex];
        if (!record.ready || record.bvh.nodes.empty() || record.bvh.triangles.empty()) {
            continue;
        }

        RT::Instance instance;
        instance.boundsMin = glm::vec4(source.worldBoundsMin, 0.0f);
        instance.boundsMax = glm::vec4(source.worldBoundsMax, 0.0f);
        instance.worldFromLocal = source.worldTransform;
        instance.localFromWorld = source.inverseWorldTransform;
        instance.metadata = glm::uvec4(
            static_cast<uint32_t>(record.nodeOffset),
            static_cast<uint32_t>(record.triangleOffset),
            source.isSkinnedMesh ? 1u : 0u,
            0u);
        instances.push_back(instance);
    }

    const bool readyInstanceSetChanged =
        geometryUpdated ||
        instances.size() != previousTraceableInstanceCount ||
        (instances.empty() && m_incrementalInstanceNodeCount != 0);
    m_incrementalInstanceCount = instances.size();

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_instanceSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
        instances.empty() ? 0 : instances.size() * sizeof(RT::Instance),
        instances.empty() ? nullptr : instances.data(),
        GL_DYNAMIC_DRAW);

    if (topologyDirty || readyInstanceSetChanged || m_incrementalInstanceNodeCount == 0) {
        std::vector<RT::InstanceNode> instanceNodes;
        BuildInstanceTLAS(instances, instanceNodes);
        m_incrementalInstanceNodeCount = instanceNodes.size();

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_instanceNodeSSBO);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
            instanceNodes.empty() ? 0 : instanceNodes.size() * sizeof(RT::InstanceNode),
            instanceNodes.empty() ? nullptr : instanceNodes.data(),
            GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    m_incrementalUploadDirty = false;
    m_incrementalInstanceTopologyDirty = false;
}

bool RTSceneResources::EnsureIncrementalBLAS(
    const std::shared_ptr<SceneGraph>& sceneGraph,
    const IncrementalBuildSettings& settings)
{
    using Clock = std::chrono::steady_clock;
    const auto frameStart = Clock::now();
    std::size_t trianglesBuiltThisFrame = 0;

    const ComponentManager* componentManager = sceneGraph ? sceneGraph->GetComponentManager() : nullptr;
    TransformSystem* transformSystem = sceneGraph ? sceneGraph->GetTransformSystem() : nullptr;
    RebuildIncrementalInstanceList(sceneGraph, settings);

    BVHBuilder::BuildParams params;
    params.maxLeafPrimitives = 2;
    params.maxDepth = 24;
    params.sahBuckets = 16;
    params.verbose = false;

    while (!m_pendingBLAS.empty()) {
        const double elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
        if (elapsedMs >= settings.maxBuildMsPerFrame && trianglesBuiltThisFrame > 0) {
            break;
        }
        if (trianglesBuiltThisFrame >= settings.maxBlasTrianglesPerFrame && trianglesBuiltThisFrame > 0) {
            break;
        }
        if (settings.maxResidentBytes > 0 && m_incrementalResidentBytes >= settings.maxResidentBytes) {
            break;
        }

        const std::size_t recordIndex = m_pendingBLAS.front();
        m_pendingBLAS.pop_front();
        if (recordIndex >= m_blasRecords.size()) {
            continue;
        }

        BLASRecord& record = m_blasRecords[recordIndex];
        if (!record.mesh || record.ready || record.failed) {
            continue;
        }
        if (settings.maxBlasTrianglesPerFrame > 0 &&
            record.triangleCount > settings.maxBlasTrianglesPerFrame) {
            record.failed = true;
            continue;
        }

        RT::BVHData bvh = BVHBuilder::BuildFromMesh(*record.mesh, glm::mat4(1.0f), params);
        if (bvh.triangles.empty() || bvh.nodes.empty()) {
            record.failed = true;
            continue;
        }

        record.triangleCount = bvh.triangles.size();
        record.residentBytes = bvh.GetTriangleBufferSize() + bvh.GetNodeBufferSize();
        if (settings.maxResidentBytes > 0 &&
            m_incrementalResidentBytes + record.residentBytes > settings.maxResidentBytes) {
            record.failed = true;
            continue;
        }

        record.bvh = std::move(bvh);
        record.ready = true;
        m_incrementalResidentBytes += record.residentBytes;
        trianglesBuiltThisFrame += record.triangleCount;
        m_incrementalUploadDirty = true;
    }

    if (m_incrementalUploadDirty || m_incrementalInstanceTopologyDirty) {
        UploadIncrementalScene();
    }

    const double buildMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
    RefreshIncrementalDiagnostics(buildMs, trianglesBuiltThisFrame);
    return m_diagnostics.incrementalReady;
}

void RTSceneResources::UpdateLights(const std::shared_ptr<LightManager>& lightManager)
{
    if (!lightManager) {
        m_lightSSBO = 0;
        m_lightCount = 0;
        return;
    }

    m_lightSSBO = lightManager->GetLightDataSSBO();
    m_lightCount = lightManager->GetEnabledLights().size();
}

void RTSceneResources::BindForTracing(GLuint triangleBinding, GLuint bvhBinding) const
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, triangleBinding, m_triangleSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bvhBinding, m_bvhSSBO);
}

void RTSceneResources::BindIncrementalForTracing(GLuint triangleBinding, GLuint bvhBinding, GLuint instanceBinding, GLuint instanceNodeBinding) const
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, triangleBinding, m_incrementalTriangleSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bvhBinding, m_incrementalBVHSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, instanceBinding, m_instanceSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, instanceNodeBinding, m_instanceNodeSSBO);
}

void RTSceneResources::BindLights(GLuint lightBinding) const
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, lightBinding, m_lightSSBO);
}

void RTSceneResources::Release()
{
    if (m_triangleSSBO != 0) {
        glDeleteBuffers(1, &m_triangleSSBO);
        m_triangleSSBO = 0;
    }
    if (m_bvhSSBO != 0) {
        glDeleteBuffers(1, &m_bvhSSBO);
        m_bvhSSBO = 0;
    }
    if (m_incrementalTriangleSSBO != 0) {
        glDeleteBuffers(1, &m_incrementalTriangleSSBO);
        m_incrementalTriangleSSBO = 0;
    }
    if (m_incrementalBVHSSBO != 0) {
        glDeleteBuffers(1, &m_incrementalBVHSSBO);
        m_incrementalBVHSSBO = 0;
    }
    if (m_instanceSSBO != 0) {
        glDeleteBuffers(1, &m_instanceSSBO);
        m_instanceSSBO = 0;
    }
    if (m_instanceNodeSSBO != 0) {
        glDeleteBuffers(1, &m_instanceNodeSSBO);
        m_instanceNodeSSBO = 0;
    }
    m_lightSSBO = 0;
    m_triangleCount = 0;
    m_nodeCount = 0;
    m_lightCount = 0;
    m_incrementalTriangleCount = 0;
    m_incrementalNodeCount = 0;
    m_incrementalInstanceCount = 0;
    m_incrementalInstanceNodeCount = 0;
    m_lastEstimatedTriangleCount = 0;
    m_lastSkippedTriangleCount = 0;
    m_lastSkippedTriangleCap = 0;
    m_hasBuiltScene = false;
    m_loggedOversizedScene = false;
    m_cachedIncrementalSceneGraph = nullptr;
    m_cachedIncrementalRoot = nullptr;
    m_cachedHierarchyRevision = 0;
    m_cachedRenderableRevision = 0;
    m_cachedTransformPublication = 0;
    m_cachedIncludeSkinnedMeshes = false;
    m_blasLookup.clear();
    m_blasRecords.clear();
    m_pendingBLAS.clear();
    m_tlasInstances.clear();
    m_entityInstanceRanges.clear();
    m_incrementalResidentBytes = 0;
    m_skippedSkinnedInstances = 0;
    m_incrementalUploadDirty = false;
    m_incrementalInstanceTopologyDirty = false;
    m_diagnostics = Diagnostics{};
}
