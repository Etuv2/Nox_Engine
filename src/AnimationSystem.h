#pragma once
#include "ComponentManager.h"
#include "TransformSystem.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>

class Animation;
class AnimationController;
class Scene;

/**
 * Animation System - Handles all animation updates using ECS components
 * 
 * This system processes AnimationComponent data to update skeletal animations,
 * morph targets, and animated transforms. Designed for cache-friendly iteration.
 * 
 * Features:
 * - Skeletal animation with bone matrix computation
 * - Morph target blending
 * - Animation blending and transitions
 * - Legacy animation support for backward compatibility
 */
class AnimationSystem {
public:
    AnimationSystem(ComponentManager* componentManager, TransformSystem* transformSystem);
    ~AnimationSystem() = default;

    // Update all active animations
    void Update(float deltaTime);

    // Play animation on entity
    void PlayAnimation(EntityID entity, int animationIndex, bool loop = true);
    void PlayAnimationByName(EntityID entity, const std::string& name, bool loop = true);
    
    // Animation control
    void PauseAnimation(EntityID entity);
    void StopAnimation(EntityID entity);
    void ResumeAnimation(EntityID entity);
    
    // Stop all playing animations (called on scene change)
    void StopAllAnimations();

    // Animation blending
    void BlendToAnimation(EntityID entity, int animationIndex, float blendTime, bool loop = true);
    void BlendToAnimationByName(EntityID entity, const std::string& name, float blendTime, bool loop = true);

    // Morph targets
    void SetMorphWeights(EntityID entity, const std::vector<float>& weights);
    const std::vector<float>& GetMorphWeights(EntityID entity) const;

    // Get bone matrices for skinned meshes
    std::vector<glm::mat4> GetBoneMatrices(EntityID entity) const;

    // Statistics
    size_t GetActiveAnimationCount() const { return m_activeAnimations; }

private:
    ComponentManager* m_componentManager;
    TransformSystem* m_transformSystem;

    size_t m_activeAnimations = 0;

    // Empty morph weights for returning when entity has none
    static const std::vector<float> s_emptyMorphWeights;

    // Internal helpers
    void UpdateEntityAnimation(EntityID entity, AnimationComponent& animComp, float deltaTime);
    void ApplyAnimationToTransform(EntityID entity, const Animation& anim, float time);
    void ComputeBoneMatrices(EntityID entity, const RenderableComponent& renderable,
                             std::vector<glm::mat4>& outMatrices) const;
    
    // Skeletal animation helpers
    void UpdateSkeletonAnimations(EntityID entity, const Animation& anim, float time,
                                  const RenderableComponent& renderable);
    
    // Compute world transform for a joint, respecting hierarchy
    glm::mat4 ComputeJointWorldTransform(int jointNodeIndex, 
                                         const Scene& model,
                                         const Animation& anim,
                                         float time,
                                         std::unordered_map<int, glm::mat4>& jointWorldCache) const;
    
    // Get animated local transform for a specific node
    glm::mat4 GetAnimatedLocalTransform(int nodeIndex, const Animation& anim, float time,
                                        const Scene& model) const;
};
