#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory> // Add memory include for shared_ptr
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/transform.hpp>

// Forward-declare Scene to avoid circular includes.
class Scene;
class SceneNode;

class Animation {
public:
    // glTF interpolation modes
    enum class InterpolationMode {
        LINEAR,
        STEP,
        CUBICSPLINE
    };

    // Animation channel types
    enum class ChannelType {
        TRANSLATION,
        ROTATION,
        SCALE,
        WEIGHTS  // For morph targets
    };

    struct Channel {
        ChannelType type;                    // Channel type
        std::string path;                    // "translation", "rotation", "scale", or "weights"
        std::vector<float> keyframes;        // keyframe times
        std::vector<glm::vec4> values;       // Animation values (may be vec3 for some types)
        std::vector<float> morphWeights;     // For morph target weights
        InterpolationMode interpolation = InterpolationMode::LINEAR;
        int targetNode = -1;                 // Target node index
        int samplerIndex = -1;               // Index into the samplers array
    };

    struct Sampler {
        std::vector<float> input;            // Input keyframe times
        std::vector<glm::vec4> output;       // Output values
        std::vector<float> morphOutput;      // Output for morph weights
        InterpolationMode interpolation = InterpolationMode::LINEAR;
    };

    std::string name;
    float startTime = 0.0f;
    float endTime = 0.0f;
    float duration = 0.0f;
    std::vector<Channel> channels;
    std::vector<Sampler> samplers;
    
    // Animation state tracking
    bool isLooping = true;
    float weight = 1.0f;  // For animation blending

    // Core animation evaluation methods
    glm::mat4 GetNodeTransform(float time) const;
    glm::mat4 GetBoneTransformForNode(int targetNode, float time, const Scene& scene) const;
    
    // Enhanced interpolation methods
    glm::vec3 InterpolateTranslation(const Channel& channel, float time) const;
    glm::quat InterpolateRotation(const Channel& channel, float time) const;
    glm::vec3 InterpolateScale(const Channel& channel, float time) const;
    std::vector<float> InterpolateMorphWeights(const Channel& channel, float time) const;
    
    // Generic interpolation for different data types
    template<typename T>
    T InterpolateLinear(const T& a, const T& b, float t) const;
    
    template<typename T>
    T InterpolateCubicSpline(const T& prevTangent, const T& prevValue, const T& nextValue, const T& nextTangent, float t) const;
    
    // Utility methods
    std::pair<size_t, size_t> FindKeyframeIndices(const std::vector<float>& keyframes, float time) const;
    float NormalizeTime(float time) const;
    void UpdateDuration();
    
    // Animation state queries
    bool HasMorphTargets() const;
    bool HasSkeletalAnimation() const;
    std::vector<int> GetAnimatedNodes() const;
    
private:
    // Internal helper methods
    float CalculateInterpolationFactor(float time, float t0, float t1) const;
    size_t GetOutputStride(InterpolationMode mode) const;
};

// Animation Controller for managing multiple animations
class AnimationController {
public:
    struct AnimationState {
        std::shared_ptr<Animation> animation;
        float currentTime = 0.0f;
        float speed = 1.0f;
        float weight = 1.0f;
        bool isPlaying = false;
        bool isPaused = false;
        bool isLooping = true;
        int priority = 0;  // For animation layering
    };

    // Animation playback control
    void PlayAnimation(std::shared_ptr<Animation> animation, float fadeInTime = 0.0f, bool loop = true);
    void StopAnimation(const std::string& animationName, float fadeOutTime = 0.0f);
    void PauseAnimation(const std::string& animationName);
    void ResumeAnimation(const std::string& animationName);
    void StopAllAnimations(float fadeOutTime = 0.0f);
    
    // Animation blending
    void BlendToAnimation(std::shared_ptr<Animation> animation, float blendTime, bool loop = true);
    void CrossFadeAnimations(const std::string& fromAnim, const std::string& toAnim, float duration);
    
    // Update method - call this each frame
    void Update(float deltaTime);
    
    // Apply animations to scene nodes
    void ApplyAnimationsToScene(const std::shared_ptr<Scene>& scene);
    void ApplyAnimationsToNode(const std::shared_ptr<class SceneNode>& node, const std::shared_ptr<Scene>& scene);
    
    // Animation state queries
    bool IsAnimationPlaying(const std::string& animationName) const;
    bool HasActiveAnimations() const;
    float GetAnimationTime(const std::string& animationName) const;
    void SetAnimationTime(const std::string& animationName, float time);
    void SetAnimationSpeed(const std::string& animationName, float speed);
    void SetAnimationWeight(const std::string& animationName, float weight);
    
    // Morph target support
    void ApplyMorphTargets(const std::shared_ptr<class SceneNode>& node, const std::vector<float>& weights);
    
    // Skinning support
    void UpdateBoneMatrices(const std::shared_ptr<class SceneNode>& node, const std::shared_ptr<Scene>& scene);
    
private:
    std::unordered_map<std::string, AnimationState> m_activeAnimations;
    std::vector<AnimationState*> m_blendStack;  // For layered animation blending
    
    // Animation blending helpers
    glm::mat4 BlendTransforms(const std::vector<std::pair<glm::mat4, float>>& transforms) const;
    std::vector<float> BlendMorphWeights(const std::vector<std::pair<std::vector<float>, float>>& weightSets) const;
    
    // Internal state
    bool m_needsUpdate = true;
    float m_globalSpeed = 1.0f;
    
    // Helper methods
    void CleanupFinishedAnimations();
    void SortBlendStack();
};
