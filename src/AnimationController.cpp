#include "Animation.h"
#include "SceneNode.h"
#include "Scene.h"
#include <algorithm>
#include <iostream>
#include <cmath> // Add for fmod function
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp> // Add for glm::decompose
#include <glm/gtx/quaternion.hpp> // Add for glm::toMat4

void AnimationController::PlayAnimation(std::shared_ptr<Animation> animation, float fadeInTime, bool loop) {
    if (!animation) return;
    
    AnimationState& state = m_activeAnimations[animation->name];
    state.animation = animation;
    state.currentTime = 0.0f;
    state.speed = 1.0f;
    state.weight = (fadeInTime > 0.0f) ? 0.0f : 1.0f;
    state.isPlaying = true;
    state.isPaused = false;
    state.isLooping = loop;
    state.priority = 0;
    
    animation->isLooping = loop;
    m_needsUpdate = true;
    
    std::cout << "[AnimationController] Playing animation: " << animation->name 
              << " (duration: " << animation->duration << "s, loop: " << (loop ? "yes" : "no") << ")" 
              << std::endl;
}

void AnimationController::StopAnimation(const std::string& animationName, float fadeOutTime) {
    auto it = m_activeAnimations.find(animationName);
    if (it != m_activeAnimations.end()) {
        if (fadeOutTime > 0.0f) {
            // TODO: Implement fade out
            it->second.isPlaying = false;
        } else {
            m_activeAnimations.erase(it);
        }
        m_needsUpdate = true;
        std::cout << "[AnimationController] Stopped animation: " << animationName << std::endl;
    }
}

void AnimationController::PauseAnimation(const std::string& animationName) {
    auto it = m_activeAnimations.find(animationName);
    if (it != m_activeAnimations.end() && it->second.isPlaying) {
        it->second.isPaused = true;
        std::cout << "[AnimationController] Paused animation: " << animationName << std::endl;
    }
}

void AnimationController::ResumeAnimation(const std::string& animationName) {
    auto it = m_activeAnimations.find(animationName);
    if (it != m_activeAnimations.end() && it->second.isPlaying) {
        it->second.isPaused = false;
        std::cout << "[AnimationController] Resumed animation: " << animationName << std::endl;
    }
}

void AnimationController::StopAllAnimations(float fadeOutTime) {
    for (auto& pair : m_activeAnimations) {
        pair.second.isPlaying = false;
    }
    
    if (fadeOutTime <= 0.0f) {
        m_activeAnimations.clear();
    }
    
    m_needsUpdate = true;
    std::cout << "[AnimationController] Stopped all animations" << std::endl;
}

void AnimationController::BlendToAnimation(std::shared_ptr<Animation> animation, float blendTime, bool loop) {
    // For now, just play the new animation
    // In a full implementation, you'd smoothly blend between the current and new animation
    PlayAnimation(animation, blendTime, loop);
}

void AnimationController::CrossFadeAnimations(const std::string& fromAnim, const std::string& toAnim, float duration) {
    // TODO: Implement proper cross-fading between animations
    auto fromIt = m_activeAnimations.find(fromAnim);
    auto toIt = m_activeAnimations.find(toAnim);
    
    if (fromIt != m_activeAnimations.end()) {
        fromIt->second.weight = 0.5f; // Start fading out
    }
    
    if (toIt != m_activeAnimations.end()) {
        toIt->second.weight = 0.5f; // Start fading in
        toIt->second.isPlaying = true;
        toIt->second.isPaused = false;
    }
    
    std::cout << "[AnimationController] Cross-fading from " << fromAnim << " to " << toAnim 
              << " over " << duration << "s" << std::endl;
}

void AnimationController::Update(float deltaTime) {
    if (m_activeAnimations.empty()) return;
    
    deltaTime *= m_globalSpeed;
    
    // Update all active animations
    for (auto it = m_activeAnimations.begin(); it != m_activeAnimations.end();) {
        AnimationState& state = it->second;
        
        if (!state.isPlaying) {
            it = m_activeAnimations.erase(it);
            continue;
        }
        
        if (!state.isPaused && state.animation) {
            state.currentTime += deltaTime * state.speed;
            
            // Handle looping
            if (state.isLooping && state.animation->duration > 0.0f) {
                if (state.currentTime > state.animation->endTime) {
                    state.currentTime = state.animation->startTime + 
                        std::fmod(state.currentTime - state.animation->startTime, state.animation->duration);
                }
            } else {
                // Clamp to animation bounds for non-looping animations
                if (state.currentTime > state.animation->endTime) {
                    state.currentTime = state.animation->endTime;
                    state.isPlaying = false; // Animation finished
                    std::cout << "[AnimationController] Animation finished: " << state.animation->name << std::endl;
                }
            }
        }
        
        ++it;
    }
    
    CleanupFinishedAnimations();
}

void AnimationController::ApplyAnimationsToScene(const std::shared_ptr<Scene>& scene) {
    if (!scene || m_activeAnimations.empty()) return;
    
    // This would typically be called by the scene graph update
    // For now, we'll apply animations to scene nodes when they're updated
}

void AnimationController::ApplyAnimationsToNode(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<Scene>& scene) {
    if (!node || !scene || m_activeAnimations.empty()) return;
    
    // Collect all animations that affect this node
    std::vector<std::pair<glm::mat4, float>> transforms;
    std::vector<std::pair<std::vector<float>, float>> morphWeightSets;
    
    int nodeIndex = node->nodeIndex;
    if (nodeIndex < 0) return;
    
    for (const auto& pair : m_activeAnimations) {
        const AnimationState& state = pair.second;
        if (!state.isPlaying || state.isPaused || !state.animation) continue;
        
        // Check if this animation affects this node
        bool affectsNode = false;
        for (const auto& channel : state.animation->channels) {
            if (channel.targetNode == nodeIndex) {
                affectsNode = true;
                break;
            }
        }
        
        if (!affectsNode) continue;
        
        // Get animated transform for this node
        if (node->isSkinned) {
            glm::mat4 transform = state.animation->GetBoneTransformForNode(nodeIndex, state.currentTime, *scene);
            transforms.push_back({transform, state.weight});
        } else {
            glm::mat4 transform = state.animation->GetBoneTransformForNode(nodeIndex, state.currentTime, *scene);
            transforms.push_back({transform, state.weight});
        }
        
        // Get morph weights if this animation has them
        for (const auto& channel : state.animation->channels) {
            if (channel.targetNode == nodeIndex && channel.type == Animation::ChannelType::WEIGHTS) {
                std::vector<float> weights = state.animation->InterpolateMorphWeights(channel, state.currentTime);
                if (!weights.empty()) {
                    morphWeightSets.push_back({weights, state.weight});
                }
            }
        }
    }
    
    // Apply blended transforms
    if (!transforms.empty()) {
        glm::mat4 blendedTransform = BlendTransforms(transforms);
        // Apply the blended transform to the node using the public setter
        node->SetAnimatedTransform(blendedTransform);
    }
    
    // Apply blended morph weights
    if (!morphWeightSets.empty()) {
        std::vector<float> blendedWeights = BlendMorphWeights(morphWeightSets);
        ApplyMorphTargets(node, blendedWeights);
    }
}

void AnimationController::ApplyMorphTargets(const std::shared_ptr<SceneNode>& node, const std::vector<float>& weights) {
    if (!node || weights.empty()) return;
    
    // Apply morph target weights to the node's meshes
    auto model = node->GetModel();
    if (!model) return;
    
    for (auto& mesh : model->meshes) {
        if (mesh.morphTargetCount > 0 && !mesh.morphBuffers.empty()) {
            // Update morph target weights
            // This would typically involve updating vertex buffers or shader uniforms
            // The exact implementation depends on your morph target rendering system
            
            size_t numTargets = std::min(weights.size(), (size_t)mesh.morphTargetCount);
            for (size_t i = 0; i < numTargets; ++i) {
                // Store weights for use in rendering
                // You might store these in a uniform buffer or as vertex attributes
                // For now, we'll just log the weights
                if (i == 0) { // Only log for the first target to avoid spam
                    std::cout << "[AnimationController] Applied morph weight " << i << ": " << weights[i] << std::endl;
                }
            }
        }
    }
}

void AnimationController::UpdateBoneMatrices(const std::shared_ptr<SceneNode>& node, const std::shared_ptr<Scene>& scene) {
    if (!node || !scene || !node->isSkinned) return;
    
    // Update bone transforms for skinned mesh
    std::vector<glm::mat4> boneMatrices = node->GetBoneTransforms();
    
    // The bone matrices are now ready for use in skinning shaders
    // They will be uploaded to the GPU during rendering
}

bool AnimationController::IsAnimationPlaying(const std::string& animationName) const {
    auto it = m_activeAnimations.find(animationName);
    return it != m_activeAnimations.end() && it->second.isPlaying && !it->second.isPaused;
}

float AnimationController::GetAnimationTime(const std::string& animationName) const {
    auto it = m_activeAnimations.find(animationName);
    return (it != m_activeAnimations.end()) ? it->second.currentTime : 0.0f;
}

void AnimationController::SetAnimationTime(const std::string& animationName, float time) {
    auto it = m_activeAnimations.find(animationName);
    if (it != m_activeAnimations.end()) {
        it->second.currentTime = time;
    }
}

void AnimationController::SetAnimationSpeed(const std::string& animationName, float speed) {
    auto it = m_activeAnimations.find(animationName);
    if (it != m_activeAnimations.end()) {
        it->second.speed = speed;
    }
}

void AnimationController::SetAnimationWeight(const std::string& animationName, float weight) {
    auto it = m_activeAnimations.find(animationName);
    if (it != m_activeAnimations.end()) {
        it->second.weight = glm::clamp(weight, 0.0f, 1.0f);
    }
}

glm::mat4 AnimationController::BlendTransforms(const std::vector<std::pair<glm::mat4, float>>& transforms) const {
    if (transforms.empty()) return glm::mat4(1.0f);
    if (transforms.size() == 1) return transforms[0].first;
    
    // For proper transform blending, we should decompose into TRS, blend each component,
    // then recompose. For now, we'll use a simple weighted average approach.
    
    glm::vec3 blendedTranslation(0.0f);
    glm::quat blendedRotation(1, 0, 0, 0);
    glm::vec3 blendedScale(0.0f);
    float totalWeight = 0.0f;
    
    for (const auto& pair : transforms) {
        const glm::mat4& transform = pair.first;
        float weight = pair.second;
        
        if (weight <= 0.0f) continue;
        
        // Decompose transform
        glm::vec3 scale, translation, skew;
        glm::quat rotation;
        glm::vec4 perspective;
        glm::decompose(transform, scale, rotation, translation, skew, perspective);
        
        // Accumulate weighted components
        blendedTranslation += translation * weight;
        blendedScale += scale * weight;
        
        // For rotation, we need to be more careful with quaternion blending
        if (totalWeight == 0.0f) {
            blendedRotation = rotation;
        } else {
            // Simple linear blend (not ideal for quaternions, but works for small differences)
            blendedRotation = glm::normalize(glm::slerp(blendedRotation, rotation, weight / (totalWeight + weight)));
        }
        
        totalWeight += weight;
    }
    
    if (totalWeight > 0.0f) {
        blendedTranslation /= totalWeight;
        blendedScale /= totalWeight;
    }
    
    // Recompose transform
    glm::mat4 T = glm::translate(glm::mat4(1.0f), blendedTranslation);
    glm::mat4 R = glm::toMat4(blendedRotation);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), blendedScale);
    
    return T * R * S;
}

std::vector<float> AnimationController::BlendMorphWeights(const std::vector<std::pair<std::vector<float>, float>>& weightSets) const {
    if (weightSets.empty()) return {};
    
    // Determine the maximum number of morph targets
    size_t maxTargets = 0;
    for (const auto& pair : weightSets) {
        maxTargets = std::max(maxTargets, pair.first.size());
    }
    
    if (maxTargets == 0) return {};
    
    std::vector<float> blendedWeights(maxTargets, 0.0f);
    float totalWeight = 0.0f;
    
    for (const auto& pair : weightSets) {
        const std::vector<float>& weights = pair.first;
        float blendWeight = pair.second;
        
        if (blendWeight <= 0.0f) continue;
        
        for (size_t i = 0; i < weights.size() && i < maxTargets; ++i) {
            blendedWeights[i] += weights[i] * blendWeight;
        }
        
        totalWeight += blendWeight;
    }
    
    // Normalize by total weight
    if (totalWeight > 0.0f) {
        for (float& weight : blendedWeights) {
            weight /= totalWeight;
        }
    }
    
    return blendedWeights;
}

void AnimationController::CleanupFinishedAnimations() {
    for (auto it = m_activeAnimations.begin(); it != m_activeAnimations.end();) {
        if (!it->second.isPlaying) {
            std::cout << "[AnimationController] Cleaning up finished animation: " << it->first << std::endl;
            it = m_activeAnimations.erase(it);
        } else {
            ++it;
        }
    }
}

void AnimationController::SortBlendStack() {
    // Sort by priority (higher priority animations come later and override lower priority ones)
    std::sort(m_blendStack.begin(), m_blendStack.end(), 
        [](const AnimationState* a, const AnimationState* b) {
            return a->priority < b->priority;
        });
}