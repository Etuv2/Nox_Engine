#pragma once
// Global game time scale (engine service). Core feeds game time, animation, physics and the Lua systems with
// deltaTime * scale, so the fixed-step accumulator (physics substeps, fixed Lua systems) advances by the scaled
// time and stays deterministic for a given scale. The editor UI and the real-time overlays (debug draw lifetimes,
// frame timing) keep using unscaled time.
#include <cstdint>

namespace TimeScale {

constexpr float kMin = 0.0f;
constexpr float kMax = 10.0f;

// Clamps to [kMin, kMax]; non-finite values are rejected by Set and map to 1 here.
float Clamp(float scale);
float Get();
// False (and no change) for a non-finite value. Values outside the range are clamped.
bool Set(float scale);
void Reset();

// Scaled simulation time for one frame.
float Apply(float deltaTime);

// Fixed-step accumulator: Advance returns how many whole steps the time adds, and keeps the remainder. Used with
// scaled time, so a scale of 0.5 runs half as many steps per real second.
class FixedStepAccumulator {
public:
    explicit FixedStepAccumulator(float step) : m_step(step > 0.0f ? step : 1.0f / 60.0f) {}
    int Advance(float dt);
    float Remainder() const { return m_remainder; }
    float Step() const { return m_step; }
    void Reset() { m_remainder = 0.0f; }
private:
    float m_step;
    float m_remainder = 0.0f;
};

} // namespace TimeScale
