// DriveAudio.cpp - procedural driving audio (see DriveAudio.h).
//
// Owns its own SDL audio device (SDL_OpenAudioDevice, float stereo, device-chosen rate) so it does
// not depend on SDL_mixer's Mix_OpenAudio ordering. Game thread (Update/Play) and the audio thread
// (callback) share state through SDL_LockAudioDevice: Update copies the target state and Play queues
// cues under the device lock, the callback is already run under that lock by SDL.

#include "DriveAudio.h"

#include <SDL/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr int kHarmonics = 10;
constexpr int kMaxCueVoices = 8;
constexpr int kMaxPendingCues = 16;
constexpr int kMaxNotes = 4;
constexpr int kBlock = 32;          // harmonic weights are refreshed every kBlock samples
constexpr float kMasterScale = 0.6f;

// Relative harmonic strengths of a 4-cylinder four-stroke: firing order gives a strong 2nd order
// and a weaker 4th, with the usual inline/boxer odd-even mix on top.
constexpr std::array<float, kHarmonics> kHarmonicShape = {
    1.00f, 0.80f, 0.30f, 0.55f, 0.15f, 0.20f, 0.07f, 0.09f, 0.03f, 0.04f};

inline float Clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// One-pole smoothing coefficient for a time constant in seconds.
inline float SmoothCoef(float tauSeconds, float sampleRate) {
    return 1.0f - std::exp(-1.0f / (std::max(tauSeconds, 1e-4f) * sampleRate));
}

// Deterministic xorshift white noise in [-1, 1].
struct Rng {
    uint32_t s = 0x9E3779B9u;
    float White() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) * (2.0f / 4294967295.0f) - 1.0f;
    }
};

// Chamberlin state variable filter. damp = 1/Q. Outputs stored in low/band/high.
struct Svf {
    float low = 0.0f, band = 0.0f, high = 0.0f;
    void Run(float in, float fc, float damp, float sampleRate) {
        const float f = 2.0f * std::sin(kPi * std::min(fc, 0.15f * sampleRate) / sampleRate);
        low += f * band;
        high = in - low - damp * band;
        band += f * high;
    }
};

enum class Wave : uint8_t { Sine, SoftSquare, Saw, Click };

struct Note {
    float f0;      // start frequency (Hz)
    float f1;      // end frequency (Hz), glide target
    float start;   // seconds from cue start
    float dur;     // seconds
    float amp;
    Wave wave;
};

struct CueDef {
    int count;
    std::array<Note, kMaxNotes> notes;
};

// Indexed by UiSound (must match the enum order in DriveAudio.h).
const std::array<CueDef, 9> kCues = {{
    /* Select     */ {1, {{Note{880, 880, 0.00f, 0.05f, 0.40f, Wave::Sine}}}},
    /* Back       */ {1, {{Note{520, 340, 0.00f, 0.10f, 0.35f, Wave::Sine}}}},
    /* Countdown  */ {1, {{Note{660, 660, 0.00f, 0.12f, 0.45f, Wave::SoftSquare}}}},
    /* Go         */ {2, {{Note{990, 990, 0.00f, 0.30f, 0.50f, Wave::SoftSquare},
                         Note{1980, 1980, 0.00f, 0.30f, 0.18f, Wave::Sine}}}},
    /* Checkpoint */ {2, {{Note{1320, 1320, 0.00f, 0.14f, 0.40f, Wave::Sine},
                         Note{1760, 1760, 0.11f, 0.22f, 0.40f, Wave::Sine}}}},
    /* Finish     */ {4, {{Note{523.25f, 523.25f, 0.00f, 0.35f, 0.28f, Wave::Saw},
                         Note{659.25f, 659.25f, 0.10f, 0.35f, 0.28f, Wave::Saw},
                         Note{783.99f, 783.99f, 0.20f, 0.35f, 0.28f, Wave::Saw},
                         Note{1046.5f, 1046.5f, 0.30f, 0.50f, 0.30f, Wave::Saw}}}},
    /* Missed     */ {2, {{Note{140, 110, 0.00f, 0.30f, 0.30f, Wave::SoftSquare},
                         Note{146, 116, 0.00f, 0.30f, 0.15f, Wave::Saw}}}},
    /* Record     */ {4, {{Note{2093.0f, 2093.0f, 0.00f, 0.06f, 0.30f, Wave::Sine},
                         Note{2637.0f, 2637.0f, 0.05f, 0.06f, 0.30f, Wave::Sine},
                         Note{3136.0f, 3136.0f, 0.10f, 0.06f, 0.30f, Wave::Sine},
                         Note{4186.0f, 4186.0f, 0.15f, 0.22f, 0.28f, Wave::Sine}}}},
    /* Shift      */ {2, {{Note{1200, 900, 0.00f, 0.07f, 0.40f, Wave::Sine},
                         Note{0, 0, 0.00f, 0.012f, 0.22f, Wave::Click}}}},
}};

struct CueVoice {
    bool active = false;
    float t = 0.0f;                       // seconds since trigger
    std::array<float, kMaxNotes> phase{}; // cycles
    const CueDef* def = nullptr;
};

// Stateful synth. Render() is called only from one thread at a time (audio callback, or the
// offline test path), so it holds no locks itself.
class Synth {
public:
    void Trigger(UiSound sound) {
        CueVoice* slot = nullptr;
        for (auto& v : m_voices) {
            if (!v.active) { slot = &v; break; }
        }
        if (!slot) slot = &m_voices[0];  // steal the oldest-looking slot
        *slot = CueVoice{};
        slot->active = true;
        slot->def = &kCues[static_cast<size_t>(sound)];
    }

    void Render(float* out, int frames, int channels, float sr, const DriveAudioState& st) {
        const float invSr = 1.0f / sr;
        const float rpmTarget = std::min(std::max(st.rpm, 600.0f), st.rpmMax * 1.05f);
        const float thrTarget = Clamp01(st.throttle);
        const float loadTarget = Clamp01(0.2f + 0.8f * thrTarget);
        const float speedTarget = Clamp01(st.speed01);
        const float slipTarget = Clamp01(st.slip01);
        const float nitroTarget = st.nitro ? 1.0f : 0.0f;
        const float engineTarget = (st.active && !st.paused) ? 1.0f : 0.0f;
        const float masterTarget = Clamp01(st.masterVolume);

        const float cRpm = SmoothCoef(0.06f, sr);
        const float cThr = SmoothCoef(0.03f, sr);
        const float cGain = SmoothCoef(0.15f, sr);
        const float cSlip = SmoothCoef(0.04f, sr);
        const float cSpeed = SmoothCoef(0.30f, sr);
        const float cNitro = SmoothCoef(0.08f, sr);
        const float cMaster = SmoothCoef(0.05f, sr);
        const float cLimiter = SmoothCoef(0.004f, sr);

        std::array<float, kHarmonics> amps{};
        const float cueScale = 0.5f;

        for (int i = 0; i < frames; ++i) {
            m_rpm += (rpmTarget - m_rpm) * cRpm;
            m_thr += (thrTarget - m_thr) * cThr;
            m_load += (loadTarget - m_load) * cThr;
            m_speed += (speedTarget - m_speed) * cSpeed;
            m_slip += (slipTarget - m_slip) * cSlip;
            m_nitro += (nitroTarget - m_nitro) * cNitro;
            m_engine += (engineTarget - m_engine) * cGain;
            m_master += (masterTarget - m_master) * cMaster;

            if ((i % kBlock) == 0) {
                // Load-dependent timbre: harder load gives a flatter (brighter) spectrum.
                const float rolloff = Lerp(1.6f, 0.7f, m_load);
                float sum = 0.0f;
                for (int k = 0; k < kHarmonics; ++k) {
                    amps[k] = kHarmonicShape[k] * std::pow(static_cast<float>(k + 1), -rolloff);
                    sum += amps[k];
                }
                const float norm = sum > 0.0f ? 1.0f / sum : 0.0f;
                for (auto& a : amps) a *= norm;
            }

            // Four-cylinder firing frequency: two firings per crankshaft revolution.
            const float firingHz = m_rpm / 60.0f * 2.0f;

            // Engine harmonic stack.
            float harmonic = 0.0f;
            for (int k = 0; k < kHarmonics; ++k) {
                m_harmPhase[k] += firingHz * static_cast<float>(k + 1) * invSr;
                if (m_harmPhase[k] >= 1.0f) m_harmPhase[k] -= 1.0f;
                harmonic += amps[k] * std::sin(kTwoPi * m_harmPhase[k]);
            }
            const float firePulse = 0.75f + 0.25f * std::cos(kTwoPi * m_harmPhase[0]);

            // Intake: band-passed noise that opens up with rpm, modulated by firing pulses.
            const float intakeFc = 420.0f + m_rpm * 0.12f;
            m_intake.Run(m_rng.White(), intakeFc, 1.4f, sr);
            const float intake = m_intake.band * 0.10f * (0.2f + 0.8f * m_thr) * firePulse;

            // Exhaust rumble: low-passed noise, darker at low load.
            m_exhaust.Run(m_rng.White(), 150.0f + 450.0f * m_load, 0.9f, sr);
            const float exhaust = m_exhaust.low * 0.22f * (0.3f + 0.7f * m_load);

            // Rev limiter bounce: a ~14 Hz cut-and-recover gate while the limiter is active.
            m_limPhase += 14.0f * invSr;
            if (m_limPhase >= 1.0f) m_limPhase -= 1.0f;
            const float gateTarget = st.limiter
                ? 0.40f + 0.60f * (0.5f + 0.5f * std::cos(kTwoPi * m_limPhase))
                : 1.0f;
            m_limGate += (gateTarget - m_limGate) * cLimiter;

            const float engineLevel = (0.32f * harmonic * (0.35f + 0.65f * m_thr) + intake + exhaust);
            const float engine = engineLevel * m_engine * m_limGate;

            // Tyre squeal: narrow band noise plus a tonal whine, both driven by slip^2.
            const float sq = m_slip * m_slip;
            m_squealBp.Run(m_rng.White(), 2300.0f, 0.08f, sr);
            m_squealPhase += (1150.0f + 60.0f * m_slip) * invSr;
            if (m_squealPhase >= 1.0f) m_squealPhase -= 1.0f;
            const float squeal = sq * (m_squealBp.band * 0.05f + 0.05f * std::sin(kTwoPi * m_squealPhase));

            // Wind: low-passed noise with a slow gust swell, scaled by speed^2.
            m_wind.Run(m_rng.White(), 280.0f + 900.0f * m_speed, 1.0f, sr);
            m_gustPhase += 0.2f * invSr;
            if (m_gustPhase >= 1.0f) m_gustPhase -= 1.0f;
            const float gust = 0.8f + 0.2f * std::sin(kTwoPi * m_gustPhase);
            const float wind = (m_wind.low * 0.35f + m_wind.high * 0.02f) * m_speed * m_speed * gust;

            // Nitrous hiss: high-passed noise.
            m_nitroHp.Run(m_rng.White(), 3500.0f, 1.2f, sr);
            const float nitro = m_nitroHp.high * 0.05f * m_nitro;

            // UI cues (independent of the drive state, so they also play while paused).
            float cue = 0.0f;
            for (auto& v : m_voices) {
                if (v.active) cue += RenderCueSample(v, invSr, cueScale);
            }

            const float mix = engine + squeal + wind + nitro + cue;
            const float sample = std::tanh(mix) * m_master * kMasterScale;
            float* dst = out + static_cast<size_t>(i) * static_cast<size_t>(channels);
            for (int c = 0; c < channels; ++c) dst[c] = sample;
        }
    }

private:
    float RenderCueSample(CueVoice& v, float dt, float scale) {
        float acc = 0.0f;
        bool anyAlive = false;
        for (int n = 0; n < v.def->count; ++n) {
            const Note& nt = v.def->notes[n];
            if (v.t >= nt.start + nt.dur) continue;
            anyAlive = true;
            if (v.t < nt.start) continue;

            const float u = v.t - nt.start;
            const float fr = nt.f0 + (nt.f1 - nt.f0) * (u / nt.dur);
            float& p = v.phase[n];
            p += fr * dt;
            if (p >= 1.0f) p -= std::floor(p);

            float s = 0.0f;
            switch (nt.wave) {
            case Wave::Sine:
                s = std::sin(kTwoPi * p);
                break;
            case Wave::SoftSquare:
                s = 0.7f * std::tanh(2.0f * std::sin(kTwoPi * p));
                break;
            case Wave::Saw:
                s = 0.5f * (std::sin(kTwoPi * p) + 0.5f * std::sin(2.0f * kTwoPi * p) +
                            0.25f * std::sin(3.0f * kTwoPi * p));
                break;
            case Wave::Click:
                s = m_rng.White();
                break;
            }
            // 3 ms attack, exponential decay over the note, short fade-out: no clicks at edges.
            const float env = std::min(1.0f, u / 0.003f) * std::min(1.0f, (nt.dur - u) / 0.006f) *
                              std::exp(-3.0f * u / nt.dur);
            acc += s * env * nt.amp;
        }
        v.t += dt;
        if (!anyAlive) v.active = false;
        return acc * scale;
    }

    float m_rpm = 900.0f;
    float m_thr = 0.0f;
    float m_load = 0.2f;
    float m_speed = 0.0f;
    float m_slip = 0.0f;
    float m_nitro = 0.0f;
    float m_engine = 0.0f;
    float m_master = 0.8f;
    float m_limGate = 1.0f;
    float m_limPhase = 0.0f;
    float m_squealPhase = 0.0f;
    float m_gustPhase = 0.0f;
    std::array<float, kHarmonics> m_harmPhase{};
    Svf m_intake, m_exhaust, m_squealBp, m_wind, m_nitroHp;
    Rng m_rng;
    std::array<CueVoice, kMaxCueVoices> m_voices{};
};

struct AudioImpl {
    SDL_AudioDeviceID device = 0;
    float sampleRate = 48000.0f;
    int channels = 2;
    DriveAudioState target;                    // written by Update() under the device lock
    UiSound pending[kMaxPendingCues] = {};     // queued by Play() under the device lock
    int pendingCount = 0;
    Synth synth;
};

AudioImpl g_audio;

// Runs on SDL's audio thread. Called with the device lock already held by SDL.
void SDLCALL OnAudio(void* userdata, Uint8* stream, int len) {
    AudioImpl* impl = static_cast<AudioImpl*>(userdata);
    const int frames = len / static_cast<int>(sizeof(float) * static_cast<size_t>(impl->channels));
    for (int i = 0; i < impl->pendingCount; ++i) impl->synth.Trigger(impl->pending[i]);
    impl->pendingCount = 0;
    impl->synth.Render(reinterpret_cast<float*>(stream), frames, impl->channels, impl->sampleRate,
                       impl->target);
}

}  // namespace

DriveAudio& DriveAudio::Get() {
    static DriveAudio instance;
    return instance;
}

bool DriveAudio::Init() {
    if (g_audio.device != 0) return true;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "DriveAudio: SDL audio subsystem unavailable: %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = OnAudio;
    want.userdata = &g_audio;

    SDL_AudioSpec have{};
    // Format and channel count are fixed (SDL converts for us); the rate may change.
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (dev == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "DriveAudio: no audio output device, driving audio disabled: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    if (have.format != AUDIO_F32SYS || have.freq < 8000 || have.channels < 1 || have.channels > 2) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "DriveAudio: unsupported device format, driving audio disabled");
        SDL_CloseAudioDevice(dev);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    g_audio.sampleRate = static_cast<float>(have.freq);
    g_audio.channels = have.channels;
    g_audio.target = DriveAudioState{};
    g_audio.pendingCount = 0;
    g_audio.device = dev;
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void DriveAudio::Shutdown() {
    if (g_audio.device == 0) return;
    SDL_CloseAudioDevice(g_audio.device);  // stops and joins the callback thread
    g_audio.device = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void DriveAudio::Update(const DriveAudioState& state) {
    if (g_audio.device == 0) return;
    SDL_LockAudioDevice(g_audio.device);
    g_audio.target = state;
    SDL_UnlockAudioDevice(g_audio.device);
}

void DriveAudio::Play(UiSound sound) {
    if (g_audio.device == 0) return;
    SDL_LockAudioDevice(g_audio.device);
    if (g_audio.pendingCount < kMaxPendingCues) g_audio.pending[g_audio.pendingCount++] = sound;
    SDL_UnlockAudioDevice(g_audio.device);
}

bool DriveAudio::IsAvailable() const {
    return g_audio.device != 0;
}

// Offline hooks for tooling and tests (declared only in the test, not in DriveAudio.h). They use a
// separate synth instance and never touch the SDL device.
namespace {
Synth& OfflineSynth() {
    static Synth offline;
    return offline;
}
}  // namespace

void DriveAudioOfflineRender(float* out, int frames, int channels, float sampleRate, const DriveAudioState& st) {
    OfflineSynth().Render(out, frames, channels, sampleRate, st);
}

void DriveAudioOfflineTrigger(UiSound sound) {
    OfflineSynth().Trigger(sound);
}
