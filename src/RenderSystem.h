#pragma once
#include "ComponentManager.h"
#include "TransformSystem.h"
#include "MDIBatch.h"
#include "GLBuffer.h"
#include <glm/glm.hpp>
#include <GL/glew.h>
#include <vector>
#include <memory>
#include <unordered_map>
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
    struct GpuTransformRecord {
        glm::mat4 world{ 1.0f };
        glm::mat4 prevWorld{ 1.0f };
        glm::uvec4 metadata{ 0u, 0u, 0u, 0u }; // x=flags, y=skinPaletteOffset, z=generation
    };

    RenderSystem(ComponentManager* componentManager, TransformSystem* transformSystem);
    ~RenderSystem() = default;

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

    // Rendering overrides
    void SetForceBackfaceCulling(bool force) { m_forceBackfaceCulling = force; }
    bool GetForceBackfaceCulling() const { return m_forceBackfaceCulling; }

private:
    ComponentManager* m_componentManager;
    TransformSystem* m_transformSystem;

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
    };

    const ShaderUniformCache& GetShaderUniformCache(GLuint shader);

    static constexpr bool VerboseLogging = false;
    bool m_runtimeVerboseLogging = false;
    void BindMaterialTextures(const MeshComponent& mesh, const ShaderUniformCache& uniforms);
    void UploadMaterialUniforms(const MeshComponent& mesh, const ShaderUniformCache& uniforms);
    void UploadTransformUniforms(EntityID entity,
                                 const glm::mat4& worldTransform,
                                 const ShaderUniformCache& uniforms);
    void ApplyCullingState(const MeshComponent& mesh, CullingOverride override);
    void UploadBoneMatrices(EntityID entity, const ShaderUniformCache& uniforms);
    void UpdateGpuTransformBuffer();
    void EnsureTransformBuffer();
    
    // Batch processing helpers
    struct RenderBatch {
        EntityID entity;
        float distanceToCamera;
        bool isTransparent;
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

    GLBufferPtr m_transformBuffer;
    std::vector<GpuTransformRecord> m_gpuTransformRecords;
};
