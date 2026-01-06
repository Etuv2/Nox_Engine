#include "AnimationSystem.h"
#include "Animation.h"
#include "Scene.h"
#include "SceneNode.h"
#include <iostream>
#include <algorithm>
#include <queue>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

const std::vector<float> AnimationSystem::s_emptyMorphWeights;

AnimationSystem::AnimationSystem(ComponentManager* componentManager, TransformSystem* transformSystem)
    : m_componentManager(componentManager)
    , m_transformSystem(transformSystem)
{
}

void AnimationSystem::Update(float deltaTime)
{
    if (!m_componentManager || !m_transformSystem) return;

    m_activeAnimations = 0;

    auto& animPool = m_componentManager->GetAnimationPool();
    for (auto& entry : animPool) {
        EntityID entityID = entry.entity;
        auto& animComp = entry.component;

        if (animComp.isPlaying && !animComp.isPaused) {
            UpdateEntityAnimation(entityID, animComp, deltaTime);
            m_activeAnimations++;
        }
    }
}

void AnimationSystem::UpdateEntityAnimation(EntityID entity, AnimationComponent& animComp, float deltaTime)
{
    // Use animation controller if available
    if (animComp.controller) {
        animComp.controller->Update(deltaTime);
        
        // Get renderable to access model
        auto* renderable = m_componentManager->GetRenderable(entity);
        if (renderable && renderable->model) {
            // Apply animation via controller
            // Note: Controller handles complex blending internally
        }
        return;
    }

    // Legacy animation handling
    auto* renderable = m_componentManager->GetRenderable(entity);
    if (!renderable || !renderable->model) return;

    const auto& animations = renderable->model->animations;
    if (animComp.currentAnimationIndex < 0 || 
        animComp.currentAnimationIndex >= static_cast<int>(animations.size())) {
        return;
    }

    const Animation& anim = animations[animComp.currentAnimationIndex];
    animComp.animationTime += deltaTime;

    // Handle looping
    if (anim.isLooping) {
        if (animComp.animationTime > anim.endTime) {
            animComp.animationTime = anim.startTime + 
                std::fmod(animComp.animationTime - anim.startTime, anim.duration);
        }
    } else {
        // Clamp to end for non-looping
        if (animComp.animationTime > anim.endTime) {
            animComp.animationTime = anim.endTime;
            animComp.isPlaying = false;
        }
    }

    // Apply animation to transform(s)
    if (renderable->isSkinned) {
        // For skinned meshes, update all bone node transforms
        UpdateSkeletonAnimations(entity, anim, animComp.animationTime, *renderable);
    } else {
        // For non-skinned, just apply the root transform
        ApplyAnimationToTransform(entity, anim, animComp.animationTime);
    }
}

void AnimationSystem::ApplyAnimationToTransform(EntityID entity, const Animation& anim, float time)
{
    if (time < anim.startTime || time > anim.endTime) return;

    // Get animated transform from animation
    glm::mat4 animatedTransform = anim.GetNodeTransform(time);

    // Apply to transform component
    m_transformSystem->SetAnimatedTransform(entity, animatedTransform);
}

void AnimationSystem::UpdateSkeletonAnimations(EntityID entity, const Animation& anim, float time,
                                               const RenderableComponent& renderable)
{
    if (!renderable.model) return;
    
    const Scene& model = *renderable.model;
    
    // Apply animation to each bone node
    // The boneNodes vector contains SceneNode pointers for each joint in the skeleton
    for (size_t i = 0; i < renderable.boneNodes.size(); ++i) {
        auto& boneNode = renderable.boneNodes[i];
        if (!boneNode) continue;
        
        int nodeIndex = boneNode->nodeIndex;
        if (nodeIndex < 0) continue;
        
        // Get the animated local transform for this node
        glm::mat4 animatedLocal = GetAnimatedLocalTransform(nodeIndex, anim, time, model);
        
        // Apply to the bone node's animated transform
        // This is a LOCAL transform that will be combined with the node's base transform
        boneNode->SetAnimatedTransform(animatedLocal);
    }
}

glm::mat4 AnimationSystem::GetAnimatedLocalTransform(int nodeIndex, const Animation& anim, 
                                                      float time, const Scene& model) const
{
    // Get the base transform from the model node
    glm::vec3 baseTranslation(0.0f);
    glm::quat baseRotation(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 baseScale(1.0f);
    
    if (nodeIndex >= 0 && nodeIndex < static_cast<int>(model.nodes.size())) {
        const auto& nodeInfo = model.nodes[nodeIndex];
        glm::vec3 baseSkew;
        glm::vec4 basePerspective;
        glm::decompose(nodeInfo.localTransform, baseScale, baseRotation, 
                      baseTranslation, baseSkew, basePerspective);
    }
    
    // Start with base values - these will be overridden by animation channels
    glm::vec3 translation = baseTranslation;
    glm::quat rotation = baseRotation;
    glm::vec3 scale = baseScale;
    
    // Find all channels that target this node and apply them
    for (const auto& channel : anim.channels) {
        if (channel.targetNode != nodeIndex) continue;
        if (channel.keyframes.empty()) continue;
        
        float normalizedTime = anim.NormalizeTime(time);
        
        switch (channel.type) {
            case Animation::ChannelType::TRANSLATION:
                translation = anim.InterpolateTranslation(channel, normalizedTime);
                break;
                
            case Animation::ChannelType::ROTATION:
                rotation = anim.InterpolateRotation(channel, normalizedTime);
                break;
                
            case Animation::ChannelType::SCALE:
                scale = anim.InterpolateScale(channel, normalizedTime);
                break;
                
            case Animation::ChannelType::WEIGHTS:
                // Morph weights are handled separately
                break;
        }
    }
    
    // Build the complete local transform from the TRS values
    // This always returns the local transform - either animated values or base values
    glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
    glm::mat4 R = glm::mat4_cast(rotation);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
    
    return T * R * S;
}

glm::mat4 AnimationSystem::ComputeJointWorldTransform(int jointNodeIndex, 
                                                      const Scene& model,
                                                      const Animation& anim,
                                                      float time,
                                                      std::unordered_map<int, glm::mat4>& jointWorldCache) const
{
    // Check cache first
    auto it = jointWorldCache.find(jointNodeIndex);
    if (it != jointWorldCache.end()) {
        return it->second;
    }
    
    if (jointNodeIndex < 0 || jointNodeIndex >= static_cast<int>(model.nodes.size())) {
        return glm::mat4(1.0f);
    }
    
    const auto& nodeInfo = model.nodes[jointNodeIndex];
    
    // Get the animated local transform for this joint
    glm::mat4 localTransform = GetAnimatedLocalTransform(jointNodeIndex, anim, time, model);
    
    // If node has a parent, recursively compute parent's world transform
    glm::mat4 worldTransform;
    if (nodeInfo.parent >= 0) {
        glm::mat4 parentWorld = ComputeJointWorldTransform(nodeInfo.parent, model, anim, time, jointWorldCache);
        worldTransform = parentWorld * localTransform;
    } else {
        worldTransform = localTransform;
    }
    
    // Cache and return
    jointWorldCache[jointNodeIndex] = worldTransform;
    return worldTransform;
}

void AnimationSystem::PlayAnimation(EntityID entity, int animationIndex, bool loop)
{
    auto* animComp = m_componentManager->GetAnimation(entity);
    if (!animComp) {
        // Create animation component if it doesn't exist
        AnimationComponent newComp;
        newComp.currentAnimationIndex = animationIndex;
        newComp.animationTime = 0.0f;
        newComp.isPlaying = true;
        newComp.isPaused = false;
        m_componentManager->AddAnimation(entity, newComp);
        return;
    }

    animComp->currentAnimationIndex = animationIndex;
    animComp->animationTime = 0.0f;
    animComp->isPlaying = true;
    animComp->isPaused = false;
}

void AnimationSystem::PlayAnimationByName(EntityID entity, const std::string& name, bool loop)
{
    auto* renderable = m_componentManager->GetRenderable(entity);
    if (!renderable || !renderable->model) return;

    const auto& animations = renderable->model->animations;
    for (size_t i = 0; i < animations.size(); ++i) {
        if (animations[i].name == name) {
            PlayAnimation(entity, static_cast<int>(i), loop);
            return;
        }
    }

    std::cerr << "[AnimationSystem] Animation not found: " << name << std::endl;
}

void AnimationSystem::PauseAnimation(EntityID entity)
{
    auto* animComp = m_componentManager->GetAnimation(entity);
    if (animComp && animComp->isPlaying) {
        animComp->isPaused = true;
    }
}

void AnimationSystem::StopAnimation(EntityID entity)
{
    auto* animComp = m_componentManager->GetAnimation(entity);
    if (animComp) {
        animComp->isPlaying = false;
        animComp->isPaused = false;
        animComp->animationTime = 0.0f;
        animComp->currentAnimationIndex = -1;
    }

    // Clear animated transforms for all bone nodes
    auto* renderable = m_componentManager->GetRenderable(entity);
    if (renderable && renderable->isSkinned) {
        for (auto& boneNode : renderable->boneNodes) {
            if (boneNode) {
                boneNode->SetAnimatedTransform(glm::mat4(1.0f));
            }
        }
    }

    // Clear animated transform for the entity itself
    m_transformSystem->ClearAnimatedTransform(entity);
}

void AnimationSystem::ResumeAnimation(EntityID entity)
{
    auto* animComp = m_componentManager->GetAnimation(entity);
    if (animComp && animComp->isPaused) {
        animComp->isPaused = false;
    }
}

void AnimationSystem::StopAllAnimations()
{
    if (!m_componentManager) return;

    auto& animPool = m_componentManager->GetAnimationPool();
    for (auto& entry : animPool) {
        EntityID entityID = entry.entity;
        auto& animComp = entry.component;
        
        // Stop the animation
        animComp.isPlaying = false;
        animComp.isPaused = false;
        animComp.animationTime = 0.0f;
        animComp.currentAnimationIndex = -1;
        
        // Clear animated transforms for bone nodes
        auto* renderable = m_componentManager->GetRenderable(entityID);
        if (renderable && renderable->isSkinned) {
            for (auto& boneNode : renderable->boneNodes) {
                if (boneNode) {
                    boneNode->SetAnimatedTransform(glm::mat4(1.0f));
                }
            }
        }
        
        // Clear animated transform for the entity
        if (m_transformSystem) {
            m_transformSystem->ClearAnimatedTransform(entityID);
        }
    }
    
    m_activeAnimations = 0;
    std::cout << "[AnimationSystem] Stopped all animations" << std::endl;
}

void AnimationSystem::BlendToAnimation(EntityID entity, int animationIndex, float blendTime, bool loop)
{
    auto* animComp = m_componentManager->GetAnimation(entity);
    if (!animComp) {
        PlayAnimation(entity, animationIndex, loop);
        return;
    }

    // If using controller, delegate to it
    if (animComp->controller) {
        auto* renderable = m_componentManager->GetRenderable(entity);
        if (renderable && renderable->model && 
            animationIndex >= 0 && 
            animationIndex < static_cast<int>(renderable->model->animations.size())) {
            auto animPtr = std::make_shared<Animation>(renderable->model->animations[animationIndex]);
            animComp->controller->BlendToAnimation(animPtr, blendTime, loop);
        }
        return;
    }

    // Simple blend - for now just switch (TODO: implement proper blending)
    PlayAnimation(entity, animationIndex, loop);
}

void AnimationSystem::BlendToAnimationByName(EntityID entity, const std::string& name, float blendTime, bool loop)
{
    auto* renderable = m_componentManager->GetRenderable(entity);
    if (!renderable || !renderable->model) return;

    const auto& animations = renderable->model->animations;
    for (size_t i = 0; i < animations.size(); ++i) {
        if (animations[i].name == name) {
            BlendToAnimation(entity, static_cast<int>(i), blendTime, loop);
            return;
        }
    }
}

void AnimationSystem::SetMorphWeights(EntityID entity, const std::vector<float>& weights)
{
    auto* animComp = m_componentManager->GetAnimation(entity);
    if (!animComp) {
        AnimationComponent newComp;
        newComp.morphWeights = weights;
        m_componentManager->AddAnimation(entity, newComp);
        return;
    }

    animComp->morphWeights = weights;
}

const std::vector<float>& AnimationSystem::GetMorphWeights(EntityID entity) const
{
    const auto* animComp = m_componentManager->GetAnimation(entity);
    if (!animComp) {
        return s_emptyMorphWeights;
    }
    return animComp->morphWeights;
}

std::vector<glm::mat4> AnimationSystem::GetBoneMatrices(EntityID entity) const
{
    std::vector<glm::mat4> matrices;

    const auto* renderable = m_componentManager->GetRenderable(entity);
    if (!renderable || !renderable->isSkinned) {
        return matrices;
    }

    ComputeBoneMatrices(entity, *renderable, matrices);
    return matrices;
}

void AnimationSystem::ComputeBoneMatrices(EntityID entity, const RenderableComponent& renderable,
                                          std::vector<glm::mat4>& outMatrices) const
{
    size_t numBones = renderable.boneInverseBindMatrices.size();
    if (numBones == 0) return;

    outMatrices.resize(numBones, glm::mat4(1.0f));

    // Get the skinned mesh's world transform (the model's root transform)
    // This is needed because bone matrices are relative to the mesh's space
    glm::mat4 meshWorldTransform = m_transformSystem->GetWorldTransform(entity);
    glm::mat4 meshWorldInverse = glm::inverse(meshWorldTransform);

    // For each bone, compute the final bone matrix:
    // boneMatrix[i] = inverse(meshWorld) * boneWorld[i] * inverseBindMatrix[i]
    // 
    // This transforms a vertex from bind pose to current animated pose:
    // 1. inverseBindMatrix brings vertex from bind pose to bone local space
    // 2. boneWorld transforms from bone local to world space
    // 3. inverse(meshWorld) brings from world to mesh local space
    for (size_t i = 0; i < numBones && i < renderable.boneNodes.size(); ++i) {
        if (renderable.boneNodes[i]) {
            // Get world transform of bone node (includes animation)
            glm::mat4 boneWorld = renderable.boneNodes[i]->GetWorldPosition4x4();
            
            // Compute final bone matrix
            // The formula is: meshWorldInverse * boneWorld * inverseBindMatrix
            outMatrices[i] = meshWorldInverse * boneWorld * renderable.boneInverseBindMatrices[i];
        }
    }
}
