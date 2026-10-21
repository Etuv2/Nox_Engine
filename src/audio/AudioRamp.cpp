#include "AudioRamp.h"

#include <algorithm>
#include <cmath>

AudioRamp::AudioRamp(float initial)
    : m_value(std::isfinite(initial) ? initial : 1.0f), m_from(m_value), m_to(m_value) {
}

void AudioRamp::Set(float v) {
    if (!std::isfinite(v)) return;
    m_value = v;
    m_from = v;
    m_to = v;
    m_duration = 0.0f;
    m_elapsed = 0.0f;
    m_active = false;
}

void AudioRamp::RampTo(float target, float seconds) {
    if (!std::isfinite(target)) return;
    if (!std::isfinite(seconds) || seconds <= 0.0f) {
        Set(target);
        return;
    }
    m_from = m_value;
    m_to = target;
    m_duration = seconds;
    m_elapsed = 0.0f;
    m_active = true;
}

void AudioRamp::Tick(float dt) {
    if (!std::isfinite(dt) || dt < 0.0f || !m_active) return;
    m_elapsed += dt;
    if (m_elapsed >= m_duration) {
        Set(m_to);
        return;
    }
    const float t = m_elapsed / m_duration;
    m_value = m_from + (m_to - m_from) * t;
}
