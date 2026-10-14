#pragma once
#include "ComponentManager.h"
#include "TransformSystem.h"
#include "SceneRuntimeData.h"
#include "SimdKernels.h"
#include "MDIBatch.h"
#include "GpuBuffer.h"
#include "Vertex.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <array>
#include <limits>
#include <cstdint>

class Scene;
class MeshComponent;

/**
 * Render System - Handles all rendering operations using ECS components
 * 
 * This system processes RenderableComponent and TransformComponent data
 * to draw entities efficiently. It supports:
 * - Forward rendering pass
 * - Deferred geometry pass  
 * - Shadow cascade pass
 * - Velocity pass for TAA
 * - MDI batch collection
 * 
 * Designed for cache-friendly iteration over component pools.
 */
class RenderSystem {
public:
    struct Diagnostics {
        float cameraCacheBuildMs = 0.0f;
        float shadowCacheBuildMs = 0.0f;
        float transparentSortMs = 0.0f;
        size_t renderItemCount = 0;
        size_t visibleAllCount = 0;
        size_t visibleOpaqueCount = 0;
        size_t visibleTransparentCount = 0;
        size_t frustumCulledCount = 0;
        size_t shadowVisibleCount = 0;
        uint64_t forwardDrawCalls = 0;
        uint64_t geometryDrawCalls = 0;
        uint64_t shadowDrawCalls = 0;
        uint64_t velocityDrawCalls = 0;
        uint64_t transparentDrawCalls = 0;
        uint64_t materialUploadCount = 0;
        uint64_t materialCacheHitCount = 0;
        uint64_t textureBindCount = 0;
        uint64_t transformFullUploads = 0;
        uint64_t transformPartialUploads = 0;
        uint64_t transformUploadBytes = 0;
    };

    struct GpuTransformRecord {
        glm::mat4 world{ 1.0f };
        glm::mat4 prevWorld{ 1.0f };
        glm::uvec4 metadata{ 0u, 0u, 0u, 0u }; // x=flags, y=skinPaletteOffset, z=generation
    };

    RenderSystem(ComponentManager* componentManager, TransformSystem* transformSystem);
    ~RenderSystem();
    void SetRuntimeScene(SceneRuntimeData* runtimeScene);
    void BeginFrameDiagnostics();
    const Diagnostics& GetDiagnostics() const { return m_diagnostics; }

    struct RenderItem {
        EntityID entity = INVALID_ENTITY;
        uint32_t runtimeNodeIndex = INVALID_RUNTIME_NODE_INDEX;
        const MeshComponent* mesh = nullptr;
        std::shared_ptr<Scene> model;
        glm::mat4 localTransform{ 1.0f };
        uint32_t meshIndex = 0;
        uint32_t materialKey = 0;
        uint64_t sortKey = 0;
        CullingOverride cullingOverride = CullingOverride::CULLING_INHERIT;
        bool transparent = false;
        bool skinned = false;
    };

    struct VisibilityList {
        std::vector<uint32_t> items;
        void Clear() { items.clear(); }
    };

    struct FrameSubmissionCache {
        VisibilityList visibleAllItems;
        VisibilityList visibleOpaqueItems;
        VisibilityList sortedOpaqueItems;
        VisibilityList visibleTransparentItems;
        VisibilityList velocityItems;
        VisibilityList shadowVisibleItems;
        uint64_t cameraBuildGeneration = 0;
        uint64_t shadowBuildGeneration = 0;
        uint64_t frustumSerial = 0;
        size_t drawCount = 0;
        size_t bytesUploaded = 0;
    };

    // Forward rendering pass
    void RenderForward(const glm::mat4& view, 
                       const glm::mat4& projection,
                       rhi::ProgramHandle defaultShader);

    // Deferred geometry pass (G-buffer fill)
    void RenderGeometry(rhi::ProgramHandle geometryShader);
    void RenderGeometryBatchedGBuffer(rhi::ProgramHandle geometryShader);

    // Shadow cascade pass
    void RenderShadowCascade(const glm::mat4& lightSpaceMatrix,
                             rhi::ProgramHandle shadowShader);

    // Velocity pass for TAA
    void RenderVelocity(const glm::mat4& view,
                        const glm::mat4& projection,
                        const glm::mat4& prevView,
                        const glm::mat4& prevProjection,
                        rhi::ProgramHandle velocityShader);
    // When set, RenderVelocity draws only items whose world transform changed since the previous
    // frame (or that are skinned); the caller supplies static/camera motion separately.
    void SetVelocityMovingItemsOnly(bool movingOnly) { m_velocityMovingItemsOnly = movingOnly; }

    // Draws the mesh items whose entity is in sortedEntities (ascending) with `shader`, using the same
    // transform, skinning and culling uploads as the forward/velocity paths. view/projection are uploaded
    // for `shader`. Items outside the frustum are skipped when one is set. Returns the number of draws.
    // Used by SelectionOutlinePass.
    size_t RenderEntityItems(rhi::ProgramHandle shader, const std::vector<uint32_t>& sortedEntities,
        const glm::mat4& view, const glm::mat4& projection);

    // Collect renderable objects for MDI batching
    void CollectRenderables(MDIBatch& batch);

    // Render transparent objects (forward pass after deferred)
    void RenderTransparent(const glm::mat4& view,
                           const glm::mat4& projection,
                           rhi::ProgramHandle transparentShader);

    // Reflection probe capture: RenderForward skips models flagged excludeFromReflectionCapture.
    void SetReflectionCaptureActive(bool active) { m_reflectionCaptureActive = active; }

    // Culling
    void SetFrustumPlanes(const glm::mat4& viewProjection);
    void ClearFrustum() { m_frustumValid = false; ++m_frustumSerial; }
    bool IsSphereVisible(const glm::vec3& center, float radius) const;
    bool HasValidFrustum() const { return m_frustumValid; }

    // Statistics
    size_t GetVisibleEntityCount() const { return m_visibleCount; }
    size_t GetTotalEntityCount() const { return m_totalCount; }
    size_t GetCulledEntityCount() const { return m_totalCount - m_visibleCount; }
    rhi::BufferHandle GetTransformBufferID() const { return m_transformBuffer ? m_transformBuffer->GetID() : rhi::BufferHandle{}; }
    size_t GetTransformRecordCount() const { return m_gpuTransformRecords.size(); }
    size_t GetTransformUploadCount() const { return m_transformUploadCount; }
    const FrameSubmissionCache& GetFrameSubmissionCache() const { return m_submissionCache; }
    const std::vector<RenderItem>& GetRenderItems() const { return m_renderItems; }
    SimdBackend GetSimdBackend() const { return m_simdBackend; }

    // Rendering overrides
    void SetForceBackfaceCulling(bool force) { m_forceBackfaceCulling = force; }
    bool GetForceBackfaceCulling() const { return m_forceBackfaceCulling; }

private:
    ComponentManager* m_componentManager;
    TransformSystem* m_transformSystem;
    SceneRuntimeData* m_runtimeScene = nullptr;

    // Frustum planes for culling
    glm::vec4 m_frustumPlanes[6];
    bool m_frustumValid = false;
    
    // Rendering overrides
    bool m_forceBackfaceCulling = false;
    bool m_velocityMovingItemsOnly = false;

    // Statistics
    size_t m_visibleCount = 0;
    size_t m_totalCount = 0;

    // Internal helpers

    struct ShaderUniformCache {
        rhi::UniformLocation view = -1;
        rhi::UniformLocation projection = -1;
        rhi::UniformLocation model = -1;
        rhi::UniformLocation normalMatrix = -1;
        rhi::UniformLocation prevView = -1;
        rhi::UniformLocation prevProjection = -1;
        rhi::UniformLocation prevModel = -1;
        rhi::UniformLocation transformID = -1;
        rhi::UniformLocation useVertexTransformID = -1;
        rhi::UniformLocation useModelMatrixUniform = -1;
        rhi::UniformLocation materialID = -1;
        rhi::UniformLocation lightSpaceMatrix = -1;
        rhi::UniformLocation uEnableSkinning = -1;
        rhi::UniformLocation uBoneMatrices = -1;
        rhi::UniformLocation bones = -1;

        rhi::UniformLocation textureDiffuse = -1;
        rhi::UniformLocation textureNormal = -1;
        rhi::UniformLocation textureMetallicRoughness = -1;
        rhi::UniformLocation textureEmissive = -1;
        rhi::UniformLocation textureOcclusion = -1;
        rhi::UniformLocation textureSpecular = -1;
        rhi::UniformLocation textureSpecularColor = -1;
        rhi::UniformLocation textureTransmission = -1;

        rhi::UniformLocation hasBaseColorTexture = -1;
        rhi::UniformLocation hasNormalTexture = -1;
        rhi::UniformLocation hasMetallicRoughnessTexture = -1;
        rhi::UniformLocation hasEmissiveTexture = -1;
        rhi::UniformLocation hasOcclusionTexture = -1;
        rhi::UniformLocation hasSpecularTexture = -1;
        rhi::UniformLocation hasSpecularColorTexture = -1;
        rhi::UniformLocation hasTransmissionTexture = -1;

        rhi::UniformLocation baseColorFactor = -1;
        rhi::UniformLocation metallicFactor = -1;
        rhi::UniformLocation roughnessFactor = -1;
        rhi::UniformLocation emissiveFactor = -1;
        rhi::UniformLocation emissiveStrength = -1;
        rhi::UniformLocation occlusionStrength = -1;
        rhi::UniformLocation normalScale = -1;
        rhi::UniformLocation alphaCutoff = -1;
        rhi::UniformLocation alphaMode = -1;
        rhi::UniformLocation specularFactor = -1;
        rhi::UniformLocation specularColorFactor = -1;
        rhi::UniformLocation clearcoatFactor = -1;
        rhi::UniformLocation clearcoatRoughnessFactor = -1;
        rhi::UniformLocation transmissionFactor = -1;
        rhi::UniformLocation thicknessFactor = -1;
        rhi::UniformLocation attenuationDistance = -1;
        rhi::UniformLocation attenuationColor = -1;
        rhi::UniformLocation ior = -1;
        rhi::UniformLocation wetnessResponse = -1;
        rhi::UniformLocation puddleResponse = -1;
        rhi::UniformLocation rainBeadResponse = -1;
        rhi::UniformLocation flakeIntensity = -1;
        rhi::UniformLocation flakeDensity = -1;
        rhi::UniformLocation interiorMapping = -1;
        rhi::UniformLocation interiorDepth = -1;
        rhi::UniformLocation interiorStrength = -1;
        rhi::UniformLocation interiorSeed = -1;
        rhi::UniformLocation interiorLighting = -1;
        rhi::UniformLocation interiorBlinds = -1;
        rhi::UniformLocation interiorStyle = -1;
        rhi::UniformLocation interiorPane = -1;
        rhi::UniformLocation interiorSill = -1;
        rhi::UniformLocation interiorPaneUV = -1;
        rhi::UniformLocation interiorFarDistance = -1;

        rhi::UniformLocation baseColorUVSet = -1;
        rhi::UniformLocation normalUVSet = -1;
        rhi::UniformLocation metallicRoughnessUVSet = -1;
        rhi::UniformLocation emissiveUVSet = -1;
        rhi::UniformLocation occlusionUVSet = -1;
        rhi::UniformLocation specularUVSet = -1;
        rhi::UniformLocation specularColorUVSet = -1;
        rhi::UniformLocation transmissionUVSet = -1;
    };

    const ShaderUniformCache& GetShaderUniformCache(rhi::ProgramHandle shader);

    static constexpr bool VerboseLogging = false;
    bool m_runtimeVerboseLogging = false;
    void BindMaterialTextures(const MeshComponent& mesh, const ShaderUniformCache& uniforms);
    void UploadMaterialSamplerUniforms(const ShaderUniformCache& uniforms);
    void UploadMaterialUniforms(const MeshComponent& mesh, const ShaderUniformCache& uniforms);
    void UploadTransformUniforms(EntityID entity,
                                 const glm::mat4& modelTransform,
                                 const ShaderUniformCache& uniforms);
    void ApplyCullingState(const MeshComponent& mesh, CullingOverride override, const glm::mat4& modelTransform);
    void ApplyBakedBatchCulling(const MeshComponent& mesh, CullingOverride override, rhi::FrontFace frontFace);
    void UploadBoneMatrices(EntityID entity, const glm::mat4& meshWorldTransform, const ShaderUniformCache& uniforms);
    void UpdateGpuTransformBuffer(bool forceFullTransformUpload = false);
    void EnsureTransformBuffer();
    void PrepareFrameTransforms();
    void RebuildRenderItemsIfNeeded();
    void UpdateChangedRenderItemBounds();
    void UpdateAllRenderItemBounds();
    void PrepareCameraSubmissionCache();
    void PrepareShadowSubmissionCache(const glm::mat4& lightSpaceMatrix);
    void RenderGeometryPrepared(rhi::ProgramHandle geometryShader);
    
    // Batch processing helpers
    struct RenderBatch {
        EntityID entity;
        uint32_t runtimeNodeIndex = INVALID_RUNTIME_NODE_INDEX;
        const MeshComponent* mesh = nullptr;
        glm::mat4 localTransform{ 1.0f };
        float distanceToCamera;
        uint64_t sortKey = 0;
        bool isTransparent = false;
        CullingOverride cullingOverride = CullingOverride::CULLING_INHERIT;
        bool skinned = false;
    };
    std::vector<RenderBatch> m_renderQueue;
    void SortRenderQueue(const glm::vec3& cameraPos);
    
    // Instanced rendering support
    struct InstanceGroup {
        rhi::VertexInputHandle vao;
        rhi::ProgramHandle shaderID;
        size_t indexCount;
        std::vector<glm::mat4> transforms;
        std::vector<EntityID> entities;
    };
    std::unordered_map<uint64_t, InstanceGroup> m_instanceGroups;
    std::map<rhi::ProgramHandle, ShaderUniformCache> m_shaderUniformCaches;
    
    void BuildInstanceGroups();
    void RenderInstancedGroup(const InstanceGroup& group, rhi::ProgramHandle shader);
    uint64_t ComputeMeshKey(rhi::VertexInputHandle vao, rhi::ProgramHandle shaderID) const;
    
    // Instance buffer for GPU
    rhi::BufferHandle m_instanceVBO;
    size_t m_instanceBufferCapacity = 0;
    void EnsureInstanceBuffer(size_t requiredSize);
    void ResetMaterialStateCache(rhi::ProgramHandle shader);

    struct BakedGBufferVertex {
        Vertex vertex;
        uint32_t runtimeTransformIndex = INVALID_RUNTIME_NODE_INDEX;
        uint32_t stableTransformID = INVALID_TRANSFORM_ID;
    };

    struct BakedGBufferBatch {
        rhi::VertexInputHandle vao;
        GpuBufferPtr vertexBuffer;
        GpuBufferPtr indexBuffer;
        size_t indexCount = 0;
        const MeshComponent* materialMesh = nullptr;
        CullingOverride cullingOverride = CullingOverride::CULLING_INHERIT;
        rhi::FrontFace frontFace = rhi::FrontFace::CounterClockwise;
    };

    void RebuildBakedGBufferBatches();
    void UpdateBakedGBufferTransformBuffer();
    void ClearBakedGBufferBatches();
    bool ShouldUseBakedGBufferPath() const;
    uint64_t ComputeBakedBatchKey(const MeshComponent& mesh, CullingOverride override, rhi::FrontFace frontFace) const;
    std::vector<BakedGBufferBatch> m_bakedGBufferBatches;
    GpuBufferPtr m_bakedGBufferTransformBuffer;
    std::vector<GpuTransformRecord> m_bakedGBufferTransformRecords;
    uint64_t m_bakedGBufferRenderableRevision = 0;
    uint64_t m_bakedGBufferTransformPublication = 0;

    GpuBufferPtr m_transformBuffer;
    std::vector<GpuTransformRecord> m_gpuTransformRecords;
    size_t m_transformUploadCount = 0;
    uint64_t m_lastPreparedTransformRevision = 0;
    std::array<rhi::TextureHandle, 8> m_boundMaterialTextures{};
    rhi::ProgramHandle m_cachedMaterialShader;
    uint32_t m_cachedMaterialID = std::numeric_limits<uint32_t>::max();
    rhi::FrontFace m_cachedFrontFace = rhi::FrontFace::CounterClockwise;
    bool m_cachedFrontFaceValid = false;
    rhi::CullFace m_cachedCullFace = rhi::CullFace::Back;
    bool m_cachedCullFaceValid = false;
    bool m_cachedCullEnabled = false;
    bool m_cachedCullEnabledValid = false;
    std::unordered_map<EntityID, std::vector<glm::mat4>> m_cachedBoneMatrices;
    std::unordered_set<EntityID> m_warnedBoneLimitEntities;
    SimdBackend m_simdBackend = SimdBackend::Scalar;
    FrameSubmissionCache m_submissionCache;
    std::vector<RenderItem> m_renderItems;
    std::vector<std::vector<uint32_t>> m_runtimeNodeToRenderItems;
    std::vector<uint32_t> m_renderItemRuntimeIndices;
    std::vector<float> m_renderItemLocalCenterX;
    std::vector<float> m_renderItemLocalCenterY;
    std::vector<float> m_renderItemLocalCenterZ;
    std::vector<float> m_renderItemLocalRadius;
    std::vector<float> m_renderItemWorldCenterX;
    std::vector<float> m_renderItemWorldCenterY;
    std::vector<float> m_renderItemWorldCenterZ;
    std::vector<float> m_renderItemWorldRadius;
    std::vector<uint8_t> m_renderItemVisibleMask;
    std::vector<uint32_t> m_changedRenderItemScratch;
    uint64_t m_lastPreparedTransformPublication = 0;
    uint64_t m_lastPreparedRenderableRevision = 0;
    uint64_t m_lastRenderItemRevision = 0;
    uint64_t m_lastBoundsUpdatePublication = 0;
    uint64_t m_lastCameraCachePublication = 0;
    uint64_t m_lastShadowCachePublication = 0;
    uint64_t m_frustumSerial = 0;
    glm::mat4 m_lastShadowLightSpace{ 1.0f };
    Diagnostics m_diagnostics;
    bool m_reflectionCaptureActive = false;
};
