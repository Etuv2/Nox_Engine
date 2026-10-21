#pragma once
// Distance attenuation and occlusion for 3D sources (engine service, pure logic).
//
// Curves map a distance to a linear gain in [0, 1]. Beyond maxDistance every curve is silent. The legacy curve
// (AudioSpatialParams::Hearing) is the original linear falloff over the hearing distance, so sources that do not
// choose a curve sound exactly as before.
//
// Occlusion: a source behind level geometry is attenuated by a gain (default 0.35) that eases in and out over a short
// fade, so toggling line of sight does not click. The ray test is injected (AudioRayTest) so the logic is testable
// without physics; the engine installs a physics-backed test with AudioSetRayTest.
#include "AudioRamp.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <string>

enum class AudioAttenuation : uint8_t {
    Linear,   // gain = 1 - t, t = (d - min) / (max - min)
    Inverse,  // gain = min / (min + rolloff * (d - min)), d clamped to [min, max]
    Power,    // gain = (1 - t) ^ rolloff
};

struct AudioSpatialParams {
    AudioAttenuation curve = AudioAttenuation::Linear;
    float minDistance = 0.0f;     // full volume inside this distance (Inverse: reference distance)
    float maxDistance = 1000.0f;  // silent at and beyond this distance
    float rolloff = 1.0f;         // Inverse: falloff factor; Power: exponent (both clamped to a sane range)

    // The original falloff: linear from 0 to the hearing distance.
    static AudioSpatialParams Hearing(float hearingDistance);
};

// Names used by scripts and the reflection layer: "linear", "inverse", "power".
bool ParseAudioAttenuation(const std::string& name, AudioAttenuation& out);
const char* AudioAttenuationName(AudioAttenuation curve);

// Linear gain in [0, 1] for a listener-to-source distance. Non-finite distances are silent.
float AudioAttenuationGain(float distance, const AudioSpatialParams& params);

// Stereo balance for a source at `diff` (source minus listener) when the listener faces `listenerAngleDeg`: 0 = full
// left, 1 = full right. Same convention as AudioPlayer::setChannel3DPosition (angle from diff.x and diff.y).
float AudioPanFromDirection(const glm::vec3& diff, float listenerAngleDeg);

// Ray test: true when the segment from->to is blocked by level geometry.
using AudioRayTest = std::function<bool(const glm::vec3& from, const glm::vec3& to)>;

// Engine-wide ray test (installed by Core from the physics world). Null means no occlusion.
void AudioSetRayTest(AudioRayTest test);
AudioRayTest AudioGetRayTest();

// Smoothed occlusion for one source. Update() takes this tick's blocked state and returns the gain multiplier.
class AudioOcclusionState {
public:
    static constexpr float kDefaultOccludedGain = 0.35f;
    static constexpr float kDefaultFadeSeconds = 0.15f;

    // Returns the multiplier in [occludedGain, 1] after advancing dt seconds toward the target.
    float Update(bool blocked, float dt, float occludedGain = kDefaultOccludedGain,
                 float fadeSeconds = kDefaultFadeSeconds);
    float Gain() const { return m_ramp.Value(); }
    bool Blocked() const { return m_blocked; }

private:
    AudioRamp m_ramp{ 1.0f };
    bool m_blocked = false;
    bool m_hasState = false;
};

// One tick of occlusion for a source. When disabled the ray test is not called and the gain eases back to 1.
// Returns the occlusion multiplier for this tick.
float AudioUpdateOcclusion(AudioOcclusionState& state, bool enabled, const glm::vec3& listener, const glm::vec3& source,
                           const AudioRayTest& test, float dt);
