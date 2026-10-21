#include "AudioSpatial.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinRange = 1e-3f;
constexpr float kMinRolloff = 0.01f;
constexpr float kMaxRolloff = 16.0f;

std::mutex& RayTestMutex() {
    static std::mutex m;
    return m;
}

AudioRayTest& RayTestSlot() {
    static AudioRayTest test;
    return test;
}

} // namespace

AudioSpatialParams AudioSpatialParams::Hearing(float hearingDistance) {
    AudioSpatialParams p;
    p.curve = AudioAttenuation::Linear;
    p.minDistance = 0.0f;
    p.maxDistance = hearingDistance;
    p.rolloff = 1.0f;
    return p;
}

bool ParseAudioAttenuation(const std::string& name, AudioAttenuation& out) {
    if (name == "linear") { out = AudioAttenuation::Linear; return true; }
    if (name == "inverse") { out = AudioAttenuation::Inverse; return true; }
    if (name == "power") { out = AudioAttenuation::Power; return true; }
    return false;
}

const char* AudioAttenuationName(AudioAttenuation curve) {
    switch (curve) {
    case AudioAttenuation::Linear: return "linear";
    case AudioAttenuation::Inverse: return "inverse";
    case AudioAttenuation::Power: return "power";
    }
    return "linear";
}

float AudioAttenuationGain(float distance, const AudioSpatialParams& params) {
    if (!std::isfinite(distance) || distance < 0.0f) return 0.0f;
    const float minD = std::isfinite(params.minDistance) ? std::max(params.minDistance, 0.0f) : 0.0f;
    float maxD = std::isfinite(params.maxDistance) ? params.maxDistance : minD + kMinRange;
    maxD = std::max(maxD, minD + kMinRange);
    if (distance >= maxD) return 0.0f;
    const float rolloff = std::isfinite(params.rolloff)
        ? std::clamp(params.rolloff, kMinRolloff, kMaxRolloff) : 1.0f;

    switch (params.curve) {
    case AudioAttenuation::Linear: {
        const float t = std::clamp((distance - minD) / (maxD - minD), 0.0f, 1.0f);
        return 1.0f - t;
    }
    case AudioAttenuation::Inverse: {
        const float ref = std::max(minD, kMinRange);
        const float d = std::clamp(distance, ref, maxD);
        return ref / (ref + rolloff * (d - ref));
    }
    case AudioAttenuation::Power: {
        const float t = std::clamp((distance - minD) / (maxD - minD), 0.0f, 1.0f);
        return std::pow(1.0f - t, rolloff);
    }
    }
    return 0.0f;
}

float AudioPanFromDirection(const glm::vec3& diff, float listenerAngleDeg) {
    // Listener frame in the ground plane: forward = (cos a, sin a) on (x, z), the yaw convention of AudioListener and
    // the camera (atan2(z, x)). Right = cross(forward, up) = (-sin a, cos a). The balance is the projection of the
    // horizontal direction to the source on that right vector, so y (height) plays no part.
    const float a = listenerAngleDeg * kPi / 180.0f;
    const float rightX = -std::sin(a);
    const float rightZ = std::cos(a);
    const float horizontal = std::sqrt(diff.x * diff.x + diff.z * diff.z);
    if (!(horizontal > 1e-6f) || !std::isfinite(horizontal)) return 0.5f;   // directly above or on the listener
    const float side = (diff.x * rightX + diff.z * rightZ) / horizontal;   // -1 full left .. +1 full right
    return std::clamp(0.5f + 0.5f * side, 0.0f, 1.0f);
}

void AudioSetRayTest(AudioRayTest test) {
    std::lock_guard<std::mutex> lock(RayTestMutex());
    RayTestSlot() = std::move(test);
}

AudioRayTest AudioGetRayTest() {
    std::lock_guard<std::mutex> lock(RayTestMutex());
    return RayTestSlot();
}

float AudioOcclusionState::Update(bool blocked, float dt, float occludedGain, float fadeSeconds) {
    const float occluded = std::isfinite(occludedGain) ? std::clamp(occludedGain, 0.0f, 1.0f) : 1.0f;
    const float target = blocked ? occluded : 1.0f;
    if (!m_hasState) {
        // First observation: start at the target so a source spawned behind a wall is not faded in from open air.
        m_ramp.Set(target);
        m_blocked = blocked;
        m_hasState = true;
    }
    else if (blocked != m_blocked) {
        m_ramp.RampTo(target, fadeSeconds);
        m_blocked = blocked;
    }
    m_ramp.Tick(dt);
    return m_ramp.Value();
}

float AudioUpdateOcclusion(AudioOcclusionState& state, bool enabled, const glm::vec3& listener, const glm::vec3& source,
                           const AudioRayTest& test, float dt) {
    if (!enabled || !test) {
        return state.Update(false, dt);
    }
    const bool blocked = test(listener, source);
    return state.Update(blocked, dt);
}
