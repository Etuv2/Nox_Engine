#include "AnimationSystem.h"
#include "Animation.h"
#include "Scene.h"
#include "SceneNode.h"  // Include for SceneNode methods
#include <iostream>
#include <algorithm>

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

    // Apply animation to transform
    ApplyAnimationToTransform(entity, anim, animComp.animationTime);
}

void AnimationSystem::ApplyAnimationToTransform(EntityID entity, const Animation& anim, float time)
{
    if (time < anim.startTime || time > anim.endTime) return;

    // Get animated transform from animation
    glm::mat4 animatedTransform = anim.GetNodeTransform(time);

    // Apply to transform component
    m_transformSystem->SetAnimatedTransform(entity, animatedTransform);
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

    // Clear animated transform
    m_transformSystem->ClearAnimatedTransform(entity);
}

void AnimationSystem::ResumeAnimation(EntityID entity)
{
    auto* animComp = m_componentManager->GetAnimation(entity);
    if (animComp && animComp->isPaused) {
        animComp->isPaused = false;
    }
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

    // Get bone transforms from bone nodes
    for (size_t i = 0; i < numBones && i < renderable.boneNodes.size(); ++i) {
        if (renderable.boneNodes[i]) {
            // Get world transform of bone node
            glm::mat4 boneWorld = renderable.boneNodes[i]->GetWorldPosition4x4();
            
            // Compute final bone matrix
            outMatrices[i] = boneWorld * renderable.boneInverseBindMatrices[i];
        }
    }
}
