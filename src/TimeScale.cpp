#include "TimeScale.h"

#include <algorithm>
#include <cmath>

namespace TimeScale {
namespace {
float g_scale = 1.0f;
}

float Clamp(float scale) {
    if (!std::isfinite(scale)) return 1.0f;
    return std::clamp(scale, kMin, kMax);
}

float Get() {
    return g_scale;
}

bool Set(float scale) {
    if (!std::isfinite(scale)) return false;
    g_scale = Clamp(scale);
    return true;
}

void Reset() {
    g_scale = 1.0f;
}

float Apply(float deltaTime) {
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0f) return 0.0f;
    return deltaTime * g_scale;
}

int FixedStepAccumulator::Advance(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return 0;
    m_remainder += dt;
    int steps = 0;
    while (m_remainder >= m_step && steps < 1000) {
        m_remainder -= m_step;
        ++steps;
    }
    return steps;
}

} // namespace TimeScale
