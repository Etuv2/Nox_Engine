#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <map>
#include <GL/glew.h>
#include "SceneNode.h"
#include "Skybox.h"
#include "MDIBatch.h"
#include "ComponentManager.h"
#include "TransformSystem.h"
#include "RenderSystem.h"
#include "AnimationSystem.h"
#include "HierarchySystem.h"

// Forward declarations
class LightManager;

/**
 * SceneGraph holds the root node of the entire scene.
 * 
 * ECS Architecture:
 * - ComponentManager: Stores all ECS components
 * - TransformSystem: Handles hierarchical transform computation
 * - RenderSystem: Handles all rendering passes
 * - AnimationSystem: Handles animation updates
 * - HierarchySystem: Manages parent-child relationships
 */
class SceneGraph {
public:
    SceneGraph();
    virtual ~SceneGraph();

    std::shared_ptr<SceneNode> GetRoot();
    // const variant for read-only traversals
    std::shared_ptr<SceneNode> GetRoot() const { return m_root; }

    // Returns the scene's hierarchy as a map
    std::map<std::string, std::shared_ptr<SceneNode>> GetSceneHierarchy();

    // Forward pass draw (legacy - uses SceneNode recursion)
    [[deprecated("Use RenderForward() for ECS-based rendering")]]
    void Draw(const glm::mat4& view, const glm::mat4& projection, GLuint shaderProgram);

    // Shadow pass draw (legacy)
    [[deprecated("Use RenderShadowCascade() for ECS-based rendering")]]
    void DrawCascade(const glm::mat4& lightSpace, GLuint shadowShader);

    // Deferred geometry pass (legacy)
    [[deprecated("Use RenderGeometry() for ECS-based rendering")]]
    void DrawGeometry(GLuint geometryShader);

    // Motion vector pass for TAA (legacy)
    [[deprecated("Use RenderVelocity() for ECS-based rendering")]]
    void DrawVelocity(GLuint velocityShader);

    // MDI collection (legacy)
    [[deprecated("Use CollectRenderables() for ECS-based rendering")]]
    void CollectRenderableObjects(MDIBatch& batch);

    //  NEW ECS-BASED RENDERING API 
    
    // Forward rendering pass (ECS-based)
    void RenderForward(const glm::mat4& view, const glm::mat4& projection, GLuint shaderProgram);
    
    // Shadow cascade pass (ECS-based)
    void RenderShadowCascade(const glm::mat4& lightSpace, GLuint shadowShader);
    
    // Deferred geometry pass (ECS-based)
    void RenderGeometry(GLuint geometryShader);
    
    // Velocity pass for TAA (ECS-based)
    void RenderVelocity(const glm::mat4& view, const glm::mat4& projection,
                        const glm::mat4& prevView, const glm::mat4& prevProjection,
                        GLuint velocityShader);
    
    // Transparent objects pass (ECS-based)
    void RenderTransparent(const glm::mat4& view, const glm::mat4& projection, GLuint shader);
    
    // MDI collection (ECS-based)
    void CollectRenderables(MDIBatch& batch);
    
    // Set frustum for culling
    void SetFrustumPlanes(const glm::mat4& viewProjection);
    
    //  ANIMATION API 
    
    // Update all animations (call once per frame)
    void UpdateAnimations(float deltaTime);
    
    // Animation control for specific entities
    void PlayAnimation(EntityID entity, int animationIndex, bool loop = true);
    void StopAnimation(EntityID entity);
    
    //  END NEW API 

    // Find all nodes by type
    std::vector<std::shared_ptr<SceneNode>> FindNodesByType(SceneNode::NODE_TYPE type);
    
    //Find first LPV volume node in scene
    std::shared_ptr<SceneNode> FindLPVVolumeNode();
    
    //Save scene to file
    bool SaveToFile(const std::string& filePath);

    void SetSkybox(const std::shared_ptr<Skybox>& skybox);

    std::shared_ptr<Skybox> GetSkybox();

    // Multi-light system integration
    void SetLightManager(std::shared_ptr<LightManager> lightManager);
    std::shared_ptr<LightManager> GetLightManager() const;
    void UpdateLightManager(); // Collect lights from scene and update LightManager

    // Node searching
    std::shared_ptr<SceneNode> FindNodeByModelName(const std::string& modelName);

    void SetSceneName(const std::string& sceneName);
    std::string GetSceneName() const { return m_sceneName; }
    void PrintMembers();

    void Shutdown();

    bool IsActive() const { return m_active; }
    bool IsSwappingScenes() const { return m_swapping_scenes; }
    void SetSwappingScenes(bool swapping) { m_swapping_scenes = swapping; }

    // Post-process overrides
    float m_exposure = 1.0f;
    float m_gamma = 2.2f;

    // Per-scene physics toggle
    void SetPhysicsEnabled(bool enabled) { m_physicsEnabled = enabled; }
    bool IsPhysicsEnabled() const { return m_physicsEnabled; }
    
    // Component system access
    ComponentManager* GetComponentManager() { return &m_componentManager; }
    TransformSystem* GetTransformSystem() { return &m_transformSystem; }
    RenderSystem* GetRenderSystem() { return &m_renderSystem; }
    AnimationSystem* GetAnimationSystem() { return &m_animationSystem; }
    HierarchySystem* GetHierarchySystem() { return &m_hierarchySystem; }
    
    const ComponentManager* GetComponentManager() const { return &m_componentManager; }
    const TransformSystem* GetTransformSystem() const { return &m_transformSystem; }
    const RenderSystem* GetRenderSystem() const { return &m_renderSystem; }
    const AnimationSystem* GetAnimationSystem() const { return &m_animationSystem; }
    const HierarchySystem* GetHierarchySystem() const { return &m_hierarchySystem; }
    
    // Flat iteration methods for cache-friendly rendering (legacy)
    [[deprecated("Use RenderForward() instead")]]
    void DrawFlat(const glm::mat4& view, const glm::mat4& projection, GLuint shaderProgram);
    [[deprecated("Use RenderShadowCascade() instead")]]
    void DrawCascadeFlat(const glm::mat4& lightSpace, GLuint shadowShader);
    [[deprecated("Use RenderGeometry() instead")]]
    void DrawGeometryFlat(GLuint geometryShader);
    [[deprecated("Use CollectRenderables() instead")]]
    void CollectRenderableObjectsFlat(MDIBatch& batch);
    
    // Update all transforms in one batch
    void UpdateAllTransforms();
 
    // BVH dirty tracking for ray tracing optimization
    void MarkBVHDirty() { m_bvhDirty = true; }
    bool IsBVHDirty() const { return m_bvhDirty; }
    void ClearBVHDirty() { m_bvhDirty = false; }
    void ForceRebuildBVH() { m_bvhDirty = true; } // Explicit rebuild trigger
    
    // Rendering statistics
    size_t GetVisibleEntityCount() const { return m_renderSystem.GetVisibleEntityCount(); }
    size_t GetTotalEntityCount() const { return m_renderSystem.GetTotalEntityCount(); }
    size_t GetCulledEntityCount() const { return m_renderSystem.GetCulledEntityCount(); }

private:
    std::shared_ptr<SceneNode> FindNodeByModelNameRecursive(
        const std::shared_ptr<SceneNode>& node,
        const std::string& modelName
    );
    
    size_t EstimateRenderableObjectCount() const;
 
    std::shared_ptr<Skybox> m_skybox;
    std::shared_ptr<LightManager> m_lightManager; // Multi-light system

    std::shared_ptr<SceneNode> m_root;
    std::string m_sceneName;
    bool m_active = false;
    bool m_swapping_scenes = false;
    bool m_physicsEnabled = true;
    
    // Component-based architecture (ECS)
    ComponentManager m_componentManager;
    TransformSystem m_transformSystem;
    RenderSystem m_renderSystem;
    AnimationSystem m_animationSystem;
    HierarchySystem m_hierarchySystem;
    
    // BVH dirty flag - set to true when any geometry transforms change
    bool m_bvhDirty = true; // Start dirty to force initial build
    
    // Helper to sync SceneNode hierarchy with component system
    void SyncSceneNodeToComponents(std::shared_ptr<SceneNode> node, EntityID parentID = INVALID_ENTITY);
};
