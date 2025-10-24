#include "Animation.h"
#include "Scene.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <cmath>

// Helper: binary search to find the surrounding keyframe indices.
std::pair<size_t, size_t> Animation::FindKeyframeIndices(const std::vector<float>& keyframes, float time) const {
    if (keyframes.empty())
        return { 0, 0 };
    auto it = std::upper_bound(keyframes.begin(), keyframes.end(), time);
    size_t index0 = (it == keyframes.begin()) ? 0 : (it - keyframes.begin() - 1);
    size_t index1 = std::min(index0 + 1, keyframes.size() - 1);
    return { index0, index1 };
}

float Animation::NormalizeTime(float time) const {
    if (duration <= 0.0f) return 0.0f;
    
    if (isLooping) {
        // Loop animation time
        float t = fmod(time - startTime, duration);
        if (t < 0.0f) t += duration;
        return startTime + t;
    } else {
        // Clamp to animation bounds
        return glm::clamp(time, startTime, endTime);
    }
}

void Animation::UpdateDuration() {
    startTime = std::numeric_limits<float>::max();
    endTime = std::numeric_limits<float>::lowest();
    
    for (const auto& channel : channels) {
        if (!channel.keyframes.empty()) {
            startTime = std::min(startTime, channel.keyframes.front());
            endTime = std::max(endTime, channel.keyframes.back());
        }
    }
    
    if (startTime == std::numeric_limits<float>::max()) {
        startTime = 0.0f;
        endTime = 0.0f;
    }
    
    duration = endTime - startTime;
}

float Animation::CalculateInterpolationFactor(float time, float t0, float t1) const {
    if (fabs(t1 - t0) < 1e-6f) return 0.0f;
    float factor = (time - t0) / (t1 - t0);
    return glm::clamp(factor, 0.0f, 1.0f);
}

size_t Animation::GetOutputStride(InterpolationMode mode) const {
    switch (mode) {
        case InterpolationMode::STEP:
        case InterpolationMode::LINEAR:
            return 1;
        case InterpolationMode::CUBICSPLINE:
            return 3; // in-tangent, value, out-tangent
        default:
            return 1;
    }
}

template<typename T>
T Animation::InterpolateLinear(const T& a, const T& b, float t) const {
    return glm::mix(a, b, t);
}

template<typename T>
T Animation::InterpolateCubicSpline(const T& prevTangent, const T& prevValue, const T& nextValue, const T& nextTangent, float t) const {
    float t2 = t * t;
    float t3 = t2 * t;
    
    // Hermite basis functions
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;  // (2t^3 - 3t^2 + 1)
    float h10 = t3 - 2.0f * t2 + t;             // (t^3 - 2t^2 + t)
    float h01 = -2.0f * t3 + 3.0f * t2;         // (-2t^3 + 3t^2)
    float h11 = t3 - t2;                        // (t^3 - t^2)
    
    return h00 * prevValue + h10 * prevTangent + h01 * nextValue + h11 * nextTangent;
}

glm::vec3 Animation::InterpolateTranslation(const Channel& channel, float time) const {
    if (channel.keyframes.empty()) return glm::vec3(0.0f);
    if (channel.keyframes.size() == 1) return glm::vec3(channel.values[0]);
    
    auto indices = FindKeyframeIndices(channel.keyframes, time);
    size_t i0 = indices.first;
    size_t i1 = indices.second;
    
    if (i0 == i1) return glm::vec3(channel.values[i0]);
    
    float t0 = channel.keyframes[i0];
    float t1 = channel.keyframes[i1];
    float factor = CalculateInterpolationFactor(time, t0, t1);
    
    switch (channel.interpolation) {
        case InterpolationMode::STEP:
            return glm::vec3(channel.values[i0]);
            
        case InterpolationMode::LINEAR:
            return InterpolateLinear(glm::vec3(channel.values[i0]), glm::vec3(channel.values[i1]), factor);
            
        case InterpolationMode::CUBICSPLINE: {
            // For cubic spline, each keyframe has 3 values: in-tangent, value, out-tangent
            size_t stride = GetOutputStride(InterpolationMode::CUBICSPLINE);
            glm::vec3 prevValue = glm::vec3(channel.values[i0 * stride + 1]);
            glm::vec3 prevOutTangent = glm::vec3(channel.values[i0 * stride + 2]);
            glm::vec3 nextInTangent = glm::vec3(channel.values[i1 * stride + 0]);
            glm::vec3 nextValue = glm::vec3(channel.values[i1 * stride + 1]);
            
            float dt = t1 - t0;
            return InterpolateCubicSpline(prevOutTangent * dt, prevValue, nextValue, nextInTangent * dt, factor);
        }
        
        default:
            return InterpolateLinear(glm::vec3(channel.values[i0]), glm::vec3(channel.values[i1]), factor);
    }
}

glm::quat Animation::InterpolateRotation(const Channel& channel, float time) const {
    if (channel.keyframes.empty()) return glm::quat(1, 0, 0, 0);
    if (channel.keyframes.size() == 1) {
        const glm::vec4& v = channel.values[0];
        return glm::normalize(glm::quat(v.w, v.x, v.y, v.z));
    }
    
    auto indices = FindKeyframeIndices(channel.keyframes, time);
    size_t i0 = indices.first;
    size_t i1 = indices.second;
    
    if (i0 == i1) {
        const glm::vec4& v = channel.values[i0];
        return glm::normalize(glm::quat(v.w, v.x, v.y, v.z));
    }
    
    float t0 = channel.keyframes[i0];
    float t1 = channel.keyframes[i1];
    float factor = CalculateInterpolationFactor(time, t0, t1);
    
    switch (channel.interpolation) {
        case InterpolationMode::STEP: {
            const glm::vec4& v = channel.values[i0];
            return glm::normalize(glm::quat(v.w, v.x, v.y, v.z));
        }
        
        case InterpolationMode::LINEAR: {
            const glm::vec4& v0 = channel.values[i0];
            const glm::vec4& v1 = channel.values[i1];
            glm::quat q0 = glm::normalize(glm::quat(v0.w, v0.x, v0.y, v0.z));
            glm::quat q1 = glm::normalize(glm::quat(v1.w, v1.x, v1.y, v1.z));
            return glm::normalize(glm::slerp(q0, q1, factor));
        }
        
        case InterpolationMode::CUBICSPLINE: {
            // For quaternions, cubic spline interpolation is complex
            // For now, fall back to linear interpolation
            // In a full implementation, you'd use squad (spherical quadrangle interpolation)
            const glm::vec4& v0 = channel.values[i0];
            const glm::vec4& v1 = channel.values[i1];
            glm::quat q0 = glm::normalize(glm::quat(v0.w, v0.x, v0.y, v0.z));
            glm::quat q1 = glm::normalize(glm::quat(v1.w, v1.x, v1.y, v1.z));
            return glm::normalize(glm::slerp(q0, q1, factor));
        }
        
        default: {
            const glm::vec4& v0 = channel.values[i0];
            const glm::vec4& v1 = channel.values[i1];
            glm::quat q0 = glm::normalize(glm::quat(v0.w, v0.x, v0.y, v0.z));
            glm::quat q1 = glm::normalize(glm::quat(v1.w, v1.x, v1.y, v1.z));
            return glm::normalize(glm::slerp(q0, q1, factor));
        }
    }
}

glm::vec3 Animation::InterpolateScale(const Channel& channel, float time) const {
    if (channel.keyframes.empty()) return glm::vec3(1.0f);
    if (channel.keyframes.size() == 1) return glm::vec3(channel.values[0]);
    
    auto indices = FindKeyframeIndices(channel.keyframes, time);
    size_t i0 = indices.first;
    size_t i1 = indices.second;
    
    if (i0 == i1) return glm::vec3(channel.values[i0]);
    
    float t0 = channel.keyframes[i0];
    float t1 = channel.keyframes[i1];
    float factor = CalculateInterpolationFactor(time, t0, t1);
    
    switch (channel.interpolation) {
        case InterpolationMode::STEP:
            return glm::vec3(channel.values[i0]);
            
        case InterpolationMode::LINEAR:
            return InterpolateLinear(glm::vec3(channel.values[i0]), glm::vec3(channel.values[i1]), factor);
            
        case InterpolationMode::CUBICSPLINE: {
            size_t stride = GetOutputStride(InterpolationMode::CUBICSPLINE);
            glm::vec3 prevValue = glm::vec3(channel.values[i0 * stride + 1]);
            glm::vec3 prevOutTangent = glm::vec3(channel.values[i0 * stride + 2]);
            glm::vec3 nextInTangent = glm::vec3(channel.values[i1 * stride + 0]);
            glm::vec3 nextValue = glm::vec3(channel.values[i1 * stride + 1]);
            
            float dt = t1 - t0;
            return InterpolateCubicSpline(prevOutTangent * dt, prevValue, nextValue, nextInTangent * dt, factor);
        }
        
        default:
            return InterpolateLinear(glm::vec3(channel.values[i0]), glm::vec3(channel.values[i1]), factor);
    }
}

std::vector<float> Animation::InterpolateMorphWeights(const Channel& channel, float time) const {
    if (channel.morphWeights.empty()) return {};
    if (channel.keyframes.empty()) return {};
    
    // Determine the number of morph targets from the first keyframe
    size_t morphTargetCount = channel.morphWeights.size() / channel.keyframes.size();
    if (morphTargetCount == 0) return {};
    
    if (channel.keyframes.size() == 1) {
        return std::vector<float>(channel.morphWeights.begin(), 
                                channel.morphWeights.begin() + morphTargetCount);
    }
    
    auto indices = FindKeyframeIndices(channel.keyframes, time);
    size_t i0 = indices.first;
    size_t i1 = indices.second;
    
    std::vector<float> result(morphTargetCount);
    
    if (i0 == i1) {
        // Copy weights for the single keyframe
        std::copy(channel.morphWeights.begin() + i0 * morphTargetCount,
                 channel.morphWeights.begin() + (i0 + 1) * morphTargetCount,
                 result.begin());
        return result;
    }
    
    float t0 = channel.keyframes[i0];
    float t1 = channel.keyframes[i1];
    float factor = CalculateInterpolationFactor(time, t0, t1);
    
    // Interpolate each morph weight
    for (size_t m = 0; m < morphTargetCount; ++m) {
        float w0 = channel.morphWeights[i0 * morphTargetCount + m];
        float w1 = channel.morphWeights[i1 * morphTargetCount + m];
        
        switch (channel.interpolation) {
            case InterpolationMode::STEP:
                result[m] = w0;
                break;
            case InterpolationMode::LINEAR:
                result[m] = InterpolateLinear(w0, w1, factor);
                break;
            case InterpolationMode::CUBICSPLINE:
                // For now, fall back to linear for morph weights
                result[m] = InterpolateLinear(w0, w1, factor);
                break;
            default:
                result[m] = InterpolateLinear(w0, w1, factor);
                break;
        }
    }
    
    return result;
}

// Non-skinned node animation: interpolate translation, rotation, scale.
glm::mat4 Animation::GetNodeTransform(float time) const {
    float normalizedTime = NormalizeTime(time);
    
    glm::vec3 translation(0.0f);
    glm::quat rotation(1, 0, 0, 0);
    glm::vec3 scale(1.0f);

    for (const auto& channel : channels) {
        if (channel.keyframes.empty()) continue;
        
        switch (channel.type) {
            case ChannelType::TRANSLATION:
                translation = InterpolateTranslation(channel, normalizedTime);
                break;
            case ChannelType::ROTATION:
                rotation = InterpolateRotation(channel, normalizedTime);
                break;
            case ChannelType::SCALE:
                scale = InterpolateScale(channel, normalizedTime);
                break;
            case ChannelType::WEIGHTS:
                // Morph weights are handled separately
                break;
        }
    }

    glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
    glm::mat4 R = glm::toMat4(rotation);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
    return T * R * S;
}

// Helper to decompose a matrix into translation, rotation, scale.
static void DecomposeTRS(const glm::mat4& mat, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale) {
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(mat, outScale, outRotation, outTranslation, skew, perspective);
}

// Skinned (bone) animation: merges the node's base local transform with the animated TRS,
// returning only the *local* matrix. (No parent transform.)
glm::mat4 Animation::GetBoneTransformForNode(int targetNode, float time, const Scene& scene) const {
    float normalizedTime = NormalizeTime(time);
    
    // 1) Get the base local transform for this node from the scene.
    glm::mat4 baseMatrix(1.0f);
    if (targetNode >= 0 && targetNode < (int)scene.nodes.size()) {
        baseMatrix = scene.nodes[targetNode].localTransform;
    }

    glm::vec3 baseTranslation(0.0f), finalTranslation(0.0f);
    glm::quat baseRotation(1, 0, 0, 0), finalRotation(1, 0, 0, 0);
    glm::vec3 baseScale(1.0f), finalScale(1.0f);

    // Decompose the base matrix
    DecomposeTRS(baseMatrix, baseTranslation, baseRotation, baseScale);

    finalTranslation = baseTranslation;
    finalRotation = baseRotation;
    finalScale = baseScale;

    // 2) For each channel that targets this node, override translation/rotation/scale
    for (const auto& channel : channels) {
        if (channel.targetNode != targetNode || channel.keyframes.empty())
            continue;

        switch (channel.type) {
            case ChannelType::TRANSLATION:
                finalTranslation = InterpolateTranslation(channel, normalizedTime);
                break;
            case ChannelType::ROTATION:
                finalRotation = InterpolateRotation(channel, normalizedTime);
                break;
            case ChannelType::SCALE:
                finalScale = InterpolateScale(channel, normalizedTime);
                break;
            case ChannelType::WEIGHTS:
                // Morph weights are handled separately
                break;
        }
    }

    // 3) Combine into a final local transform
    glm::mat4 T = glm::translate(glm::mat4(1.0f), finalTranslation);
    glm::mat4 R = glm::toMat4(finalRotation);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), finalScale);

    return T * R * S;
}

bool Animation::HasMorphTargets() const {
    for (const auto& channel : channels) {
        if (channel.type == ChannelType::WEIGHTS) {
            return true;
        }
    }
    return false;
}

bool Animation::HasSkeletalAnimation() const {
    // Check if we have any rotation or translation channels (typical for skeletal animation)
    for (const auto& channel : channels) {
        if (channel.type == ChannelType::ROTATION || channel.type == ChannelType::TRANSLATION) {
            return true;
        }
    }
    return false;
}

std::vector<int> Animation::GetAnimatedNodes() const {
    std::vector<int> nodes;
    for (const auto& channel : channels) {
        if (channel.targetNode >= 0) {
            if (std::find(nodes.begin(), nodes.end(), channel.targetNode) == nodes.end()) {
                nodes.push_back(channel.targetNode);
            }
        }
    }
    return nodes;
}

// Explicit template instantiations for common types
template float Animation::InterpolateLinear<float>(const float& a, const float& b, float t) const;
template glm::vec3 Animation::InterpolateLinear<glm::vec3>(const glm::vec3& a, const glm::vec3& b, float t) const;
template glm::quat Animation::InterpolateLinear<glm::quat>(const glm::quat& a, const glm::quat& b, float t) const;
