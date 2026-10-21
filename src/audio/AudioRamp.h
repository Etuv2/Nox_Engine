#pragma once
// Linear ramp of one audio parameter over time: volume, pitch, bus gain, music fades (engine service, pure logic).
//
// Time is seconds on whatever clock the owner ticks with. The audio stage ticks with unscaled frame time, so a fade
// still finishes while the simulation is paused. A non-positive duration jumps to the target. Non-finite values are
// ignored so a bad script value cannot poison a ramp.
class AudioRamp {
public:
    explicit AudioRamp(float initial = 1.0f);

    float Value() const { return m_value; }
    float Target() const { return m_to; }
    bool Active() const { return m_active; }

    // Jumps to v and cancels any ramp in progress.
    void Set(float v);
    // Ramps from the current value to target over `seconds`. seconds <= 0 jumps.
    void RampTo(float target, float seconds);
    // Advances the ramp by dt seconds (negative or non-finite dt is ignored).
    void Tick(float dt);

private:
    float m_value = 1.0f;
    float m_from = 1.0f;
    float m_to = 1.0f;
    float m_duration = 0.0f;
    float m_elapsed = 0.0f;
    bool m_active = false;
};
