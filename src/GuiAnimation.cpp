#include "GuiAnimation.h"
#include <cmath>
#include <algorithm>

namespace GuiAnimation {
    float ApplyEasing(float t, EasingType easing) {
        t = std::clamp(t, 0.0f, 1.0f);
        
        switch (easing) {
            case EasingType::LINEAR:
                return t;
            
            case EasingType::EASE_IN:
                return t * t;
            
            case EasingType::EASE_OUT:
                return t * (2.0f - t);
            
            case EasingType::EASE_IN_OUT:
                return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
            
            case EasingType::BOUNCE:
                if (t < 0.5f) {
                    return 0.5f * (1.0f - std::cos(t * 2.0f * 3.14159f));
                } else {
                    return 0.5f + 0.5f * std::cos((t - 0.5f) * 2.0f * 3.14159f);
                }
            
            case EasingType::ELASTIC: {
                if (t == 0.0f || t == 1.0f) return t;
                float p = 0.3f;
                return std::pow(2.0f, -10.0f * t) * std::sin((t - p / 4.0f) * (2.0f * 3.14159f) / p) + 1.0f;
            }
            
            default:
                return t;
        }
    }

    float Lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }
}

GuiElementAnimator::GuiElementAnimator() {
}

GuiElementAnimator::~GuiElementAnimator() {
}

void GuiElementAnimator::Update(float deltaTime) {
    // Update property animations
    for (auto& pair : m_propertyAnimations) {
        auto& anim = pair.second;
        anim.currentTime += deltaTime;
        
        if (anim.currentTime >= anim.duration) {
            if (anim.loop) {
                anim.currentTime = std::fmod(anim.currentTime, anim.duration);
                if (anim.reverse) {
                    std::swap(anim.startValue, anim.endValue);
                }
            } else {
                anim.currentTime = anim.duration;
            }
        }
        
        m_currentValues[pair.first] = EvaluatePropertyAnimation(anim);
    }
    
    // Update keyframe animations
    for (auto& pair : m_keyframeAnimations) {
        auto& anim = pair.second;
        anim.currentTime += deltaTime;
        
        if (anim.currentTime >= anim.duration) {
            if (anim.loop) {
                anim.currentTime = std::fmod(anim.currentTime, anim.duration);
            } else {
                anim.currentTime = anim.duration;
            }
        }
        
        m_currentValues[pair.first] = EvaluateKeyframeAnimation(anim);
    }
}

void GuiElementAnimator::AddPropertyAnimation(const GuiAnimation::PropertyAnimation& anim) {
    m_propertyAnimations[anim.property] = anim;
    m_currentValues[anim.property] = anim.startValue;
}

void GuiElementAnimator::AddKeyframeAnimation(const GuiAnimation::KeyframeAnimation& anim) {
    m_keyframeAnimations[anim.property] = anim;
    if (!anim.keyframes.empty()) {
        m_currentValues[anim.property] = anim.keyframes[0].value;
    }
}

void GuiElementAnimator::RemoveAnimation(GuiAnimation::PropertyType property) {
    m_propertyAnimations.erase(property);
    m_keyframeAnimations.erase(property);
    m_currentValues.erase(property);
}

void GuiElementAnimator::ClearAnimations() {
    m_propertyAnimations.clear();
    m_keyframeAnimations.clear();
    m_currentValues.clear();
}

bool GuiElementAnimator::HasAnimation(GuiAnimation::PropertyType property) const {
    return m_propertyAnimations.find(property) != m_propertyAnimations.end() ||
           m_keyframeAnimations.find(property) != m_keyframeAnimations.end();
}

bool GuiElementAnimator::HasActiveAnimations() const {
    return !m_propertyAnimations.empty() || !m_keyframeAnimations.empty();
}

float GuiElementAnimator::GetAnimatedValue(GuiAnimation::PropertyType property, float defaultValue) const {
    auto it = m_currentValues.find(property);
    if (it != m_currentValues.end()) {
        return it->second;
    }
    return defaultValue;
}

float GuiElementAnimator::EvaluatePropertyAnimation(const GuiAnimation::PropertyAnimation& anim) const {
    float t = anim.currentTime / anim.duration;
    t = std::clamp(t, 0.0f, 1.0f);
    
    float easedT = GuiAnimation::ApplyEasing(t, anim.easing);
    return GuiAnimation::Lerp(anim.startValue, anim.endValue, easedT);
}

float GuiElementAnimator::EvaluateKeyframeAnimation(const GuiAnimation::KeyframeAnimation& anim) const {
    if (anim.keyframes.empty()) {
        return 0.0f;
    }
    
    if (anim.keyframes.size() == 1) {
        return anim.keyframes[0].value;
    }
    
    // Find the two keyframes we're between
    size_t i = 0;
    for (; i < anim.keyframes.size() - 1; ++i) {
        if (anim.currentTime >= anim.keyframes[i].time && anim.currentTime < anim.keyframes[i + 1].time) {
            break;
        }
    }
    
    // If we're past the last keyframe
    if (i >= anim.keyframes.size() - 1) {
        return anim.keyframes.back().value;
    }
    
    // Interpolate between keyframes
    const auto& k1 = anim.keyframes[i];
    const auto& k2 = anim.keyframes[i + 1];
    
    float t = (anim.currentTime - k1.time) / (k2.time - k1.time);
    t = std::clamp(t, 0.0f, 1.0f);
    
    float easedT = GuiAnimation::ApplyEasing(t, anim.easing);
    return GuiAnimation::Lerp(k1.value, k2.value, easedT);
}
