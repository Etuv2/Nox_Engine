#pragma once

#include <vector>
#include <map>
#include <functional>

namespace GuiAnimation {
    enum class PropertyType {
        ALPHA,
        SCALE_X,
        SCALE_Y,
        ROTATION,
        POSITION_X,
        POSITION_Y
    };

    enum class EasingType {
        LINEAR,
        EASE_IN,
        EASE_OUT,
        EASE_IN_OUT,
        BOUNCE,
        ELASTIC
    };

    struct PropertyAnimation {
        PropertyType property;
        float startValue;
        float endValue;
        float duration;
        float currentTime;
        EasingType easing;
        bool loop;
        bool reverse;
        
        PropertyAnimation()
            : property(PropertyType::ALPHA)
            , startValue(0.0f)
            , endValue(1.0f)
            , duration(1.0f)
            , currentTime(0.0f)
            , easing(EasingType::LINEAR)
            , loop(false)
            , reverse(false)
        {}
    };

    struct Keyframe {
        float time;
        float value;
        
        Keyframe(float t = 0.0f, float v = 0.0f) : time(t), value(v) {}
    };

    struct KeyframeAnimation {
        PropertyType property;
        std::vector<Keyframe> keyframes;
        float duration;
        float currentTime;
        EasingType easing;
        bool loop;
        
        KeyframeAnimation()
            : property(PropertyType::ALPHA)
            , duration(1.0f)
            , currentTime(0.0f)
            , easing(EasingType::LINEAR)
            , loop(false)
        {}
    };

    // Easing functions
    float ApplyEasing(float t, EasingType easing);
    float Lerp(float a, float b, float t);
}

class GuiElementAnimator {
public:
    GuiElementAnimator();
    ~GuiElementAnimator();

    void Update(float deltaTime);
    
    void AddPropertyAnimation(const GuiAnimation::PropertyAnimation& anim);
    void AddKeyframeAnimation(const GuiAnimation::KeyframeAnimation& anim);
    void RemoveAnimation(GuiAnimation::PropertyType property);
    void ClearAnimations();
    
    bool HasAnimation(GuiAnimation::PropertyType property) const;
    bool HasActiveAnimations() const;
    
    float GetAnimatedValue(GuiAnimation::PropertyType property, float defaultValue) const;

private:
    std::map<GuiAnimation::PropertyType, GuiAnimation::PropertyAnimation> m_propertyAnimations;
    std::map<GuiAnimation::PropertyType, GuiAnimation::KeyframeAnimation> m_keyframeAnimations;
    std::map<GuiAnimation::PropertyType, float> m_currentValues;
    
    float EvaluatePropertyAnimation(const GuiAnimation::PropertyAnimation& anim) const;
    float EvaluateKeyframeAnimation(const GuiAnimation::KeyframeAnimation& anim) const;
};
