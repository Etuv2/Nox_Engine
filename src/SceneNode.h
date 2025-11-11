#pragma once

#include <memory>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "RigidBody.h"
#include "MeshComponent.h"
#include "MDIBatch.h"

class Scene;

/**
 * SceneNode is the fundamental node in the scene graph, holding transformations, a model, and child nodes.
 */
class SceneNode : public std::enable_shared_from_this<SceneNode> {
public:
    // enum for node type
    enum NODE_TYPE {
        NODE,
        AUDIO,
        MODEL,
        LIGHT,
        CAMERA,
        GUI,
        LPV_VOLUME  // Light Propagation Volume for global illumination
    };

    // Culling mode enumeration for per-node control
    enum CullingOverride {
        CULLING_INHERIT = 0,    // Use mesh/material default
        CULLING_FORCE_ENABLE,   // Force enable backface culling
        CULLING_FORCE_DISABLE,  // Force disable backface culling (double-sided)
        CULLING_FORCE_FRONT     // Force front-face culling
    };
    
    // LPV Volume configuration data
    struct LPVVolumeData {
        glm::vec3 center = glm::vec3(0.0f);      // World-space center position
        glm::vec3 extent = glm::vec3(64.0f);     // World-space extent (total size in each direction)
        float voxelSize = 0.5f;                  // World-space size per voxel
        int gridResolution = 128;                // Grid resolution (128^3 voxels)
        glm::quat orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // World-space orientation (identity quaternion)
        
        // Derived properties
        glm::vec3 GetMinBounds() const { return center - extent * 0.5f; }
        glm::vec3 GetMaxBounds() const { return center + extent * 0.5f; }
        float GetCoverage() const { return gridResolution * voxelSize; } // Total coverage distance
        glm::mat4 GetTransformMatrix() const {
            glm::mat4 translation = glm::translate(glm::mat4(1.0f), center);
            glm::mat4 rotation = glm::mat4_cast(orientation);
            glm::mat4 scale = glm::scale(glm::mat4(1.0f), extent);
            return translation * rotation * scale;
        }
        glm::mat4 GetInverseTransformMatrix() const {
            return glm::inverse(GetTransformMatrix());
        }
    };

    SceneNode();
    virtual ~SceneNode() {}

    // Child references
    std::vector<std::shared_ptr<SceneNode>> children;
    std::weak_ptr<SceneNode> parentNode;

    // Skinning data
    bool isSkinned;
    std::vector<glm::mat4> boneInverseBindMatrices;
    std::vector<std::shared_ptr<SceneNode>> boneNodes; // one node per joint
    int nodeIndex; // The glTF node index this node corresponds to (if any)

    // Basic model accessor
    void SetModel(const std::shared_ptr<Scene>& model);
    std::shared_ptr<Scene> GetModel() const;
    void SetShader(unsigned int shader);

    // Node hierarchy
    void AddChild(const std::shared_ptr<SceneNode>& child);
    std::shared_ptr<SceneNode> GetChild(int index);

    // Node identification
    std::string GetName();
    std::pair<glm::vec3, glm::vec3> GetBoundingBox();

    // Culling control
    void SetCullingOverride(CullingOverride override) { m_cullingOverride = override; }
    CullingOverride GetCullingOverride() const { return m_cullingOverride; }

    // Helper function for culling state management
    static void ApplyCullingState(const MeshComponent& mesh, CullingOverride nodeOverride);

    // Rendering: Forward-style (for direct PBR shading)
    void Draw(
        const glm::mat4& parentTransform,
        const glm::mat4& view,
        const glm::mat4& projection,
        unsigned int defaultShaderProgram
    );

    // Rendering: Shadow pass (cascaded)
    void DrawCascade(
        const glm::mat4& parentTransform,
        const glm::mat4& lightSpace,
        unsigned int shadowShader
    );

    // This method is used in the geometry pass to fill position/normal/albedo, etc.
    void DrawGeometry(
        const glm::mat4& parentTransform,
        unsigned int geometryShader
    );

    // Motion vector pass for TAA
    void DrawVelocity(
        const glm::mat4& parentTransform,
        const glm::mat4& prevParentTransform,
        unsigned int velocityShader
    );

    // MDI collection (use standalone batch)
    void CollectRenderableObjects(
        const glm::mat4& parentTransform,
        MDIBatch& batch
    );

    // Transformation/animation related methods
    void SetPosition(glm::vec3 position);
    void SetRotation(glm::vec3 axis, float angle);
    void SetScale(glm::vec3 scale);
    void SetTransform(const glm::mat4& transform);
    // Clean non-accumulative setter for TRS (prevents drift when using gizmos)
    void SetLocalTRS(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);
    glm::vec3 GetPosition() const;
    glm::vec3 GetRotation() const;
    glm::vec3 GetScale() const;
    glm::mat4 GetTransform() const;
    void PlayAnimation(int animationIndex = 0);
    void PauseAnimation();
    void StopAnimation();
    void UpdateAnimation(float deltaTime);

    // Enhanced animation control
    void SetAnimationController(std::shared_ptr<class AnimationController> controller);
    std::shared_ptr<class AnimationController> GetAnimationController() const;
    void PlayAnimationByName(const std::string& animationName, bool loop = true);
    void BlendToAnimation(const std::string& animationName, float blendTime = 0.5f, bool loop = true);
    void SetMorphWeights(const std::vector<float>& weights);
    const std::vector<float>& GetMorphWeights() const;

    // Skeleton creation
    void BuildSkeleton(const Scene& model);

    // Helpers
    glm::mat4 GetGlobalTransform(const glm::mat4& parentTransform) const;
    std::vector<glm::mat4> GetBoneTransforms() const;
    std::shared_ptr<SceneNode> FindOrCreateNode(int nodeIdx, const Scene& model);
    std::shared_ptr<SceneNode> FindNodeByIndex(int nodeIdx);
    std::shared_ptr<SceneNode> GetChildByIndex(int nodeIdx);
    
    // ENHANCED: Unified world position calculation methods for hierarchy system
    glm::vec3 GetWorldPosition() const;
    glm::mat4 GetWorldPosition4x4() const;

    // Audio
    virtual void UpdateAudioNodes(const glm::vec3& listenerPos, float listenerAngle);

    // Physics
    void AttachRigidBody(std::shared_ptr<RigidBody> rb) { m_rigidbody = rb; }
    std::shared_ptr<RigidBody> GetRigidBody() const { return m_rigidbody; }
    glm::quat GetOrientation() const;

    // Physics synchronization
    void SyncPhysicsFromTransform();
    void InvalidateTransformCache();

    // Physics update control (to prevent infinite recursion)
    void SetUpdatingFromPhysics(bool updating) { m_updatingFromPhysics = updating; }
    bool IsUpdatingFromPhysics() const { return m_updatingFromPhysics; }

    // Node type
    void SetNodeType(NODE_TYPE type) { m_nodeType = type; }
    NODE_TYPE GetNodeType() const { return m_nodeType; }
    
    // LPV Volume data access
    void SetLPVVolumeData(const LPVVolumeData& data) { m_lpvData = data; }
    LPVVolumeData& GetLPVVolumeData() { return m_lpvData; }
    const LPVVolumeData& GetLPVVolumeData() const { return m_lpvData; }

    // Shutdown the node
    virtual void Shutdown();

    // Animation transform access for AnimationController
    void SetAnimatedTransform(const glm::mat4& transform) { animatedTransform = transform; }
    glm::mat4 GetAnimatedTransform() const { return animatedTransform; }

protected:
    // Transforms
    glm::mat4 transform;         // User-set local transform
    glm::mat4 animatedTransform; // Animation-driven local transform

private:
    std::shared_ptr<Scene> m_model;

    unsigned int m_shader;  // optional custom shader
    float boundingRadius;
    
    // Animation state
    bool m_isAnimationPlaying;
    bool m_isAnimationPaused;
    float m_animationTime;
    int m_currentAnimationIndex;
    
    // Enhanced animation system
    std::shared_ptr<class AnimationController> m_animationController;
    std::vector<float> m_morphWeights; // For morph target animation
    
    NODE_TYPE m_nodeType;
    
    // Physics integration
    std::shared_ptr<RigidBody> m_rigidbody;
    bool m_updatingFromPhysics = false;
    bool m_transformCacheDirty = true;
    
    // Culling override for this node
    CullingOverride m_cullingOverride = CULLING_INHERIT;
    
    // LPV volume data (only used if m_nodeType == LPV_VOLUME)
    LPVVolumeData m_lpvData;
};
