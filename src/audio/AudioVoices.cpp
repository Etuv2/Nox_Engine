#include "AudioVoices.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kSilent = 1e-4f;
constexpr float kPitchEpsilon = 1e-3f;

float ClampUnit(float v) {
    if (!std::isfinite(v)) return 0.0f;
    return std::clamp(v, 0.0f, 1.0f);
}

} // namespace

uint64_t AudioVoices::Add(const VoiceStart& start, VoiceBackend& backend, const VoiceListener& listener,
                          const std::function<float(const std::string&)>& busGain, const AudioRayTest& ray) {
    if (start.channel < 0) return 0;
    Voice v;
    v.id = m_nextId++;
    v.start = start;
    v.start.volume = ClampUnit(start.volume);
    v.volume.Set(v.start.volume);
    v.pitch.Set(v.start.pitch);
    v.appliedPitch = v.start.pitch;   // the caller already applied the starting pitch when it played the sound
    m_voices.push_back(v);
    Apply(m_voices.back(), 0.0f, backend, listener, busGain, ray);
    return v.id;
}

AudioVoices::Voice* AudioVoices::Find(uint64_t id) {
    for (Voice& v : m_voices) {
        if (v.id == id) return &v;
    }
    return nullptr;
}

const AudioVoices::Voice* AudioVoices::Find(uint64_t id) const {
    for (const Voice& v : m_voices) {
        if (v.id == id) return &v;
    }
    return nullptr;
}

bool AudioVoices::IsPlaying(uint64_t id, const VoiceBackend& backend) const {
    const Voice* v = Find(id);
    return v && backend.Playing(v->start.channel, v->start.chunk);
}

bool AudioVoices::Stop(uint64_t id, float fadeSeconds, VoiceBackend& backend) {
    for (auto it = m_voices.begin(); it != m_voices.end(); ++it) {
        if (it->id != id) continue;
        if (fadeSeconds > 0.0f && std::isfinite(fadeSeconds)) {
            it->volume.RampTo(0.0f, fadeSeconds);
            it->haltAtSilence = true;
        }
        else {
            backend.Halt(it->start.channel);
            m_voices.erase(it);
        }
        return true;
    }
    return false;
}

bool AudioVoices::SetVolume(uint64_t id, float volume, float seconds) {
    Voice* v = Find(id);
    if (!v || !std::isfinite(volume)) return false;
    v->haltAtSilence = false;
    v->volume.RampTo(ClampUnit(volume), seconds);
    return true;
}

bool AudioVoices::SetPitch(uint64_t id, float factor, float seconds) {
    Voice* v = Find(id);
    if (!v || !std::isfinite(factor)) return false;
    v->pitch.RampTo(std::clamp(factor, 0.1f, 10.0f), seconds);
    return true;
}

void AudioVoices::Apply(Voice& v, float dt, VoiceBackend& backend, const VoiceListener& listener,
                        const std::function<float(const std::string&)>& busGain, const AudioRayTest& ray) {
    const int channel = v.start.channel;
    float gain = ClampUnit(v.volume.Value()) * (busGain ? ClampUnit(busGain(v.start.bus)) : 1.0f);
    if (v.start.spatial.spatial) {
        const glm::vec3 diff = v.start.spatial.position - listener.position;
        const float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
        const float attenuation = AudioAttenuationGain(distance, v.start.spatial.params);
        const float occlusion = AudioUpdateOcclusion(v.occlusion, v.start.spatial.occlusion,
            listener.position, v.start.spatial.position, ray, dt);
        gain *= attenuation * occlusion;
        backend.SetPan(channel, AudioPanFromDirection(diff, listener.angleDegrees));
    }
    backend.SetVolume(channel, ClampUnit(gain));

    const float pitch = v.pitch.Value();
    if (std::fabs(pitch - v.appliedPitch) > kPitchEpsilon) {
        backend.SetPitch(channel, pitch);
        v.appliedPitch = pitch;
    }
}

void AudioVoices::Tick(float dt, VoiceBackend& backend, const VoiceListener& listener,
                       const std::function<float(const std::string&)>& busGain, const AudioRayTest& ray) {
    for (auto it = m_voices.begin(); it != m_voices.end();) {
        Voice& v = *it;
        if (!backend.Playing(v.start.channel, v.start.chunk)) {
            it = m_voices.erase(it);
            continue;
        }
        v.volume.Tick(dt);
        v.pitch.Tick(dt);
        if (v.haltAtSilence && !v.volume.Active() && v.volume.Value() <= kSilent) {
            backend.Halt(v.start.channel);
            it = m_voices.erase(it);
            continue;
        }
        Apply(v, dt, backend, listener, busGain, ray);
        ++it;
    }
}
