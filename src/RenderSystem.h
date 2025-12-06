#pragma once
#include "ComponentManager.h"
#include "TransformSystem.h"
#include "MDIBatch.h"
#include <glm/glm.hpp>
#include <GL/glew.h>
#include <vector>
#include <memory>
#include <unordered_map>

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

    // Statistics
    size_t GetVisibleEntityCount() const { return m_visibleCount; }
    size_t GetTotalEntityCount() const { return m_totalCount; }
    size_t GetCulledEntityCount() const { return m_totalCount - m_visibleCount; }

private:
    ComponentManager* m_componentManager;
    TransformSystem* m_transformSystem;

    // Frustum planes for culling
    glm::vec4 m_frustumPlanes[6];
    bool m_frustumValid = false;

    // Statistics
    size_t m_visibleCount = 0;
    size_t m_totalCount = 0;

    // Internal helpers
    void BindMaterialTextures(const MeshComponent& mesh, GLuint shader);
    void UploadMaterialUniforms(const MeshComponent& mesh, GLuint shader);
    void ApplyCullingState(const MeshComponent& mesh, CullingOverride override);
    void UploadBoneMatrices(EntityID entity, GLuint shader);
    
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
    
    void BuildInstanceGroups();
    void RenderInstancedGroup(const InstanceGroup& group, GLuint shader);
    uint64_t ComputeMeshKey(GLuint vao, GLuint shaderID) const;
    
    // Instance buffer for GPU
    GLuint m_instanceVBO = 0;
    size_t m_instanceBufferCapacity = 0;
    void EnsureInstanceBuffer(size_t requiredSize);
};
