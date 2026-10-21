#pragma once
// One-shot voices (engine service): sounds started by script that can be faded, re-pitched and stopped through a
// handle id. The registry owns the per-voice ramps and spatial state; the mixer calls sit behind VoiceBackend.
//
// A voice is identified by a 64-bit id, not by its mixer channel, so a handle never touches a channel that has been
// reused for another sound: the voice is gone once its channel no longer plays its chunk.
#include "AudioRamp.h"
#include "AudioSpatial.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// Listener pose used for spatial voices (see AudioListener.h).
struct VoiceListener {
    glm::vec3 position{ 0.0f };
    float angleDegrees = 0.0f;
};

class VoiceBackend {
public:
    virtual ~VoiceBackend() = default;
    virtual bool Playing(int channel, const void* chunk) const = 0;
    virtual void Halt(int channel) = 0;
    // Channel gain 0..1 (volume * bus * attenuation * occlusion).
    virtual void SetVolume(int channel, float gain01) = 0;
    // Stereo balance from the source direction: 0 = full left, 0.5 = centre, 1 = full right.
    virtual void SetPan(int channel, float pan01) = 0;
    virtual void SetPitch(int channel, float factor) = 0;
};

struct VoiceSpatial {
    bool spatial = false;
    glm::vec3 position{ 0.0f };
    AudioSpatialParams params;
    bool occlusion = false;
};

struct VoiceStart {
    int channel = -1;
    const void* chunk = nullptr;
    std::string bus = "sfx";
    float volume = 1.0f;     // 0..1 source volume
    float pitch = 1.0f;      // factor, clamped to [0.1, 10] by the caller
    VoiceSpatial spatial;
};

class AudioVoices {
public:
    // Registers a started voice and applies its first gain. Returns a non-zero id.
    uint64_t Add(const VoiceStart& start, VoiceBackend& backend, const VoiceListener& listener,
                 const std::function<float(const std::string&)>& busGain, const AudioRayTest& ray);

    bool IsPlaying(uint64_t id, const VoiceBackend& backend) const;
    // Fades to silence over fadeSeconds and halts (<= 0 halts now). False when the voice is gone.
    bool Stop(uint64_t id, float fadeSeconds, VoiceBackend& backend);
    bool SetVolume(uint64_t id, float volume, float seconds);
    bool SetPitch(uint64_t id, float factor, float seconds);

    // Advances ramps and applies gains. Voices whose channel has finished are dropped.
    void Tick(float dt, VoiceBackend& backend, const VoiceListener& listener,
              const std::function<float(const std::string&)>& busGain, const AudioRayTest& ray);

    size_t Count() const { return m_voices.size(); }

private:
    struct Voice {
        uint64_t id = 0;
        VoiceStart start;
        AudioRamp volume{ 1.0f };
        AudioRamp pitch{ 1.0f };
        bool haltAtSilence = false;
        AudioOcclusionState occlusion;
        float appliedPitch = 1.0f;
    };

    void Apply(Voice& v, float dt, VoiceBackend& backend, const VoiceListener& listener,
               const std::function<float(const std::string&)>& busGain, const AudioRayTest& ray);
    Voice* Find(uint64_t id);
    const Voice* Find(uint64_t id) const;

    std::vector<Voice> m_voices;
    uint64_t m_nextId = 1;
};
