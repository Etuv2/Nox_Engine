#pragma once
#include "ComponentManager.h"
#include "TransformSystem.h"
#include "SceneRuntimeData.h"
#include "SimdKernels.h"
#include "MDIBatch.h"
#include "GLBuffer.h"
#include <glm/glm.hpp>
#include <GL/glew.h>
#include <vector>
#include <memory>
#include <unordered_map>
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
    ~RenderSystem() = default;
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
                       GLuint defaultShader);

    // Deferred geometry pass (G-buffer fill)
    void RenderGeometry(GLuint geometryShader);

    // Shadow cascade pass
    void RenderShadowCascade(const glm::mat4& lightSpaceMatrix,
                             GLuint shadowShader);

    // Velocity pass for TAA
    void RenderVelocity(const glm::mat4& view,
                        const glm::mat4& projection,
                        const glm::mat4& prevView,
                        const glm::mat4& prevProjection,
                        GLuint velocityShader);

    // Collect renderable objects for MDI batching
    void CollectRenderables(MDIBatch& batch);

    // Render transparent objects (forward pass after deferred)
    void RenderTransparent(const glm::mat4& view,
                           const glm::mat4& projection,
                           GLuint transparentShader);

    // Culling
    void SetFrustumPlanes(const glm::mat4& viewProjection);
    bool IsSphereVisible(const glm::vec3& center, float radius) const;
    bool HasValidFrustum() const { return m_frustumValid; }

    // Statistics
    size_t GetVisibleEntityCount() const { return m_visibleCount; }
    size_t GetTotalEntityCount() const { return m_totalCount; }
    size_t GetCulledEntityCount() const { return m_totalCount - m_visibleCount; }
    GLuint GetTransformBufferID() const { return m_transformBuffer ? m_transformBuffer->GetID() : 0; }
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

    // Statistics
    size_t m_visibleCount = 0;
    size_t m_totalCount = 0;

    // Internal helpers

    struct ShaderUniformCache {
        GLint view = -1;
        GLint projection = -1;
        GLint model = -1;
        GLint normalMatrix = -1;
        GLint prevView = -1;
        GLint prevProjection = -1;
        GLint prevModel = -1;
        GLint transformID = -1;
        GLint materialID = -1;
        GLint lightSpaceMatrix = -1;
        GLint uEnableSkinning = -1;
        GLint uBoneMatrices = -1;
        GLint bones = -1;

        GLint textureDiffuse = -1;
        GLint textureNormal = -1;
        GLint textureMetallicRoughness = -1;
        GLint textureEmissive = -1;
        GLint textureOcclusion = -1;
        GLint textureSpecular = -1;
        GLint textureSpecularColor = -1;
        GLint textureTransmission = -1;

        GLint hasBaseColorTexture = -1;
        GLint hasNormalTexture = -1;
        GLint hasMetallicRoughnessTexture = -1;
        GLint hasEmissiveTexture = -1;
        GLint hasOcclusionTexture = -1;
        GLint hasSpecularTexture = -1;
        GLint hasSpecularColorTexture = -1;
        GLint hasTransmissionTexture = -1;

        GLint baseColorFactor = -1;
        GLint metallicFactor = -1;
        GLint roughnessFactor = -1;
        GLint emissiveFactor = -1;
        GLint emissiveStrength = -1;
        GLint occlusionStrength = -1;
        GLint normalScale = -1;
        GLint alphaCutoff = -1;
        GLint specularFactor = -1;
        GLint specularColorFactor = -1;
        GLint clearcoatFactor = -1;
        GLint clearcoatRoughnessFactor = -1;
        GLint transmissionFactor = -1;
        GLint thicknessFactor = -1;
        GLint attenuationDistance = -1;
        GLint attenuationColor = -1;
        GLint ior = -1;

        GLint baseColorUVSet = -1;
        GLint normalUVSet = -1;
        GLint metallicRoughnessUVSet = -1;
        GLint emissiveUVSet = -1;
        GLint occlusionUVSet = -1;
        GLint specularUVSet = -1;
        GLint specularColorUVSet = -1;
        GLint transmissionUVSet = -1;
    };

    const ShaderUniformCache& GetShaderUniformCache(GLuint shader);

    static constexpr bool VerboseLogging = false;
    bool m_runtimeVerboseLogging = false;
    void BindMaterialTextures(const MeshComponent& mesh, const ShaderUniformCache& uniforms);
    void UploadMaterialUniforms(const MeshComponent& mesh, const ShaderUniformCache& uniforms);
    void UploadTransformUniforms(EntityID entity,
                                 const glm::mat4& modelTransform,
                                 const ShaderUniformCache& uniforms);
    void ApplyCullingState(const MeshComponent& mesh, CullingOverride override, const glm::mat4& modelTransform);
    void UploadBoneMatrices(EntityID entity, const glm::mat4& meshWorldTransform, const ShaderUniformCache& uniforms);
    void UpdateGpuTransformBuffer();
    void EnsureTransformBuffer();
    void PrepareFrameTransforms();
    void RebuildRenderItemsIfNeeded();
    void UpdateChangedRenderItemBounds();
    void UpdateAllRenderItemBounds();
    void PrepareCameraSubmissionCache();
    void PrepareShadowSubmissionCache(const glm::mat4& lightSpaceMatrix);
    
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
        GLuint vao;
        GLuint shaderID;
        size_t indexCount;
        std::vector<glm::mat4> transforms;
        std::vector<EntityID> entities;
    };
    std::unordered_map<uint64_t, InstanceGroup> m_instanceGroups;
    std::unordered_map<GLuint, ShaderUniformCache> m_shaderUniformCaches;
    
    void BuildInstanceGroups();
    void RenderInstancedGroup(const InstanceGroup& group, GLuint shader);
    uint64_t ComputeMeshKey(GLuint vao, GLuint shaderID) const;
    
    // Instance buffer for GPU
    GLuint m_instanceVBO = 0;
    size_t m_instanceBufferCapacity = 0;
    void EnsureInstanceBuffer(size_t requiredSize);
    void ResetMaterialStateCache(GLuint shader);

    GLBufferPtr m_transformBuffer;
    std::vector<GpuTransformRecord> m_gpuTransformRecords;
    size_t m_transformUploadCount = 0;
    uint64_t m_lastPreparedTransformRevision = 0;
    std::array<GLuint, 8> m_boundMaterialTextures{};
    GLuint m_cachedMaterialShader = 0;
    uint32_t m_cachedMaterialID = std::numeric_limits<uint32_t>::max();
    GLenum m_cachedFrontFace = GL_CCW;
    bool m_cachedFrontFaceValid = false;
    GLenum m_cachedCullFace = GL_BACK;
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
};
