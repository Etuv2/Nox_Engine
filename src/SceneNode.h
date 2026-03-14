#pragma once

#include <memory>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "RigidBody.h"
#include "MeshComponent.h"
#include "ComponentTypes.h"

class Scene;
class ComponentManager;
class TransformSystem;
class MDIBatch;

/**
 * SceneNode - Lightweight scene graph node for hierarchical transforms
 * 
 * This class serves as a thin wrapper around ECS entities. Transform, rendering,
 * and animation data are stored in ECS components. SceneNode provides:
 * - Hierarchical parent-child relationships
 * - Transform manipulation (position, rotation, scale)
 * - ECS entity bridging
 * 
 * For rendering, use RenderSystem via SceneGraph.
 * For animation, use AnimationSystem via SceneGraph.
 */
class SceneNode : public std::enable_shared_from_this<SceneNode> {
public:
    // Node type enumeration
    enum NODE_TYPE {
        NODE = 0,
        AUDIO,
        MODEL,
        LIGHT,
        CAMERA,
        GUI,
        LPV_VOLUME,
        SKELETAL
    };

    // Culling mode override
    enum CullingOverride {
        CULLING_INHERIT = 0,
        CULLING_FORCE_ENABLE,
        CULLING_FORCE_DISABLE,
        CULLING_FORCE_FRONT
    };
    
    // LPV Volume configuration
    struct LPVVolumeData {
        glm::vec3 center = glm::vec3(0.0f);
        glm::vec3 extent = glm::vec3(64.0f);
        float voxelSize = 0.5f;
        int gridResolution = 128;
        glm::quat orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        
        glm::vec3 GetMinBounds() const { return center - extent * 0.5f; }
        glm::vec3 GetMaxBounds() const { return center + extent * 0.5f; }
        float GetCoverage() const { return gridResolution * voxelSize; }
        glm::mat4 GetTransformMatrix() const;
        glm::mat4 GetInverseTransformMatrix() const;
    };

    // CONSTRUCTORS
    SceneNode();
    SceneNode(ComponentManager* manager, TransformSystem* transformSystem, EntityID entityID = INVALID_ENTITY);
    virtual ~SceneNode() = default;

    // HIERARCHY (PUBLIC)
    std::vector<std::shared_ptr<SceneNode>> children;
    std::weak_ptr<SceneNode> parentNode;
    
    void AddChild(const std::shared_ptr<SceneNode>& child);
    std::shared_ptr<SceneNode> GetChild(int index);
    size_t GetChildCount() const { return children.size(); }

    // SKINNING DATA (PUBLIC for AnimationController)
    bool isSkinned = false;
    int nodeIndex = -1;
    std::vector<glm::mat4> boneInverseBindMatrices;
    std::vector<std::shared_ptr<SceneNode>> boneNodes;
    float boundingRadius = 1.0f;

    // TRANSFORM
    void SetPosition(glm::vec3 position);
    void SetRotation(glm::vec3 axis, float angle);
    void SetScale(glm::vec3 scale);
    void SetTransform(const glm::mat4& transform);
    void SetLocalTRS(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);
    
    glm::vec3 GetPosition() const;
    glm::vec3 GetRotation() const;
    glm::vec3 GetScale() const;
    glm::mat4 GetTransform() const;
    glm::quat GetOrientation() const;
    
    glm::vec3 GetWorldPosition() const;
    glm::mat4 GetWorldPosition4x4() const;
    glm::mat4 GetGlobalTransform(const glm::mat4& parentTransform) const;

    // MODEL/RENDERING
    void SetModel(const std::shared_ptr<Scene>& model);
    std::shared_ptr<Scene> GetModel() const { return m_model; }
    void SetShader(unsigned int shader) { m_shader = shader; }
    unsigned int GetShader() const { return m_shader; }
    
    void SetCullingOverride(CullingOverride override) { m_cullingOverride = override; }
    CullingOverride GetCullingOverride() const { return m_cullingOverride; }
    
    // BOUNDING BOX
    std::string GetName();
    void SetName(const std::string& name) { m_name = name; }
    std::pair<glm::vec3, glm::vec3> GetBoundingBox();

    // NODE TYPE
    void SetNodeType(NODE_TYPE type) { m_nodeType = type; }
    NODE_TYPE GetNodeType() const { return m_nodeType; }
    
    // LPV VOLUME
    void SetLPVVolumeData(const LPVVolumeData& data) { m_lpvData = data; }
    LPVVolumeData& GetLPVVolumeData() { return m_lpvData; }
    const LPVVolumeData& GetLPVVolumeData() const { return m_lpvData; }

    // PHYSICS
    void AttachRigidBody(std::shared_ptr<RigidBody> rb) { m_rigidbody = rb; }
    std::shared_ptr<RigidBody> GetRigidBody() const { return m_rigidbody; }
    void SyncPhysicsFromTransform();
    void SetUpdatingFromPhysics(bool updating) { m_updatingFromPhysics = updating; }
    bool IsUpdatingFromPhysics() const { return m_updatingFromPhysics; }

    // ANIMATION TRANSFORM
    void SetAnimatedTransform(const glm::mat4& transform) { animatedTransform = transform; }
    glm::mat4 GetAnimatedTransform() const { return animatedTransform; }

    // SKINNING HELPERS
    std::vector<glm::mat4> GetBoneTransforms() const;
    std::shared_ptr<SceneNode> FindNodeByIndex(int nodeIdx);
    void BuildSkeleton(const class Scene& model);

    // LIFECYCLE
    virtual void Shutdown();
    virtual void UpdateAudioNodes(const glm::vec3& listenerPos, float listenerAngle);
    virtual void UpdateAudioNodesWithTransform(const glm::vec3& listenerPos, float listenerAngle, const glm::mat4& parentWorldTransform);
    void InvalidateTransformCache();

    // TRANSFORM SYNCHRONIZATION
    virtual void UpdateTransformSystems(const glm::mat4& worldTransform);

    // LEGACY ANIMATION (for backward compatibility)
    void UpdateAnimation(float deltaTime);
    void UpdateAnimationWithTransform(float deltaTime, const glm::mat4& parentWorldTransform);
    
    // ECS INTEGRATION
    void SetECSContext(ComponentManager* manager, TransformSystem* transformSystem);
    EntityID GetEntityID() const { return m_entityID; }
    void SetEntityID(EntityID id) { m_entityID = id; }
    bool HasECSEntity() const { return m_entityID != INVALID_ENTITY && m_componentManager != nullptr; }
    
    void SyncToECS();
    void SyncFromECS();
    
    TransformComponent* GetTransformComponent();
    const TransformComponent* GetTransformComponent() const;
    RenderableComponent* GetRenderableComponent();
    const RenderableComponent* GetRenderableComponent() const;
    AnimationComponent* GetAnimationComponent();
    const AnimationComponent* GetAnimationComponent() const;
    
    EntityID CreateECSEntity(const std::string& name = "");
    void MigrateHierarchyToECS();
    
    static std::shared_ptr<SceneNode> CreateWithECS(
        ComponentManager* manager, 
        TransformSystem* transformSystem,
        const std::string& name = "",
        NodeType type = NodeType::NODE);

    // CULLING HELPER
    static void ApplyCullingState(const MeshComponent& mesh, CullingOverride nodeOverride);

protected:
    // Transform data (protected for derived classes like LightNode, Camera)
    glm::mat4 transform = glm::mat4(1.0f);
    glm::mat4 animatedTransform = glm::mat4(1.0f);
    mutable glm::mat4 m_cachedWorldTransform = glm::mat4(1.0f);
    mutable bool m_worldTransformValid = false;

private:
    // Model and rendering
    std::shared_ptr<Scene> m_model;
    std::string m_name;
    unsigned int m_shader = 0;
    CullingOverride m_cullingOverride = CULLING_INHERIT;
    NODE_TYPE m_nodeType = NODE;
    
    // Physics
    std::shared_ptr<RigidBody> m_rigidbody;
    bool m_updatingFromPhysics = false;
    bool m_transformCacheDirty = false;
    
    // LPV Volume
    LPVVolumeData m_lpvData;
    
    // ECS integration
    EntityID m_entityID = INVALID_ENTITY;
    ComponentManager* m_componentManager = nullptr;
    TransformSystem* m_transformSystem = nullptr;
    
    ComponentManager* RequireComponentManager(const char* caller) const;
};
