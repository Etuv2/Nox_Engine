// Audio runtime logic without a device: fade ramps, attenuation curves, occlusion toggling (mocked ray test), music
// crossfade timing and handle lifetime (fake backends), and mixer ducking. Time advances in fixed steps (NOX_FIXED_DT).
#include "TestFramework.h"

#include "audio/AudioMixer.h"
#include "audio/AudioMusic.h"
#include "audio/AudioRamp.h"
#include "audio/AudioSpatial.h"
#include "audio/AudioVoices.h"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr float kFixedDt = 0.0166666667f;   // NOX_FIXED_DT
constexpr float kEps = 1e-3f;

bool Near(float a, float b, float eps = kEps) { return std::fabs(a - b) <= eps; }

std::function<float(const std::string&)> UnitBus() {
    return [](const std::string&) { return 1.0f; };
}

// ---- Fakes ----------------------------------------------------------------------------------------------------

class FakeMusic : public MusicBackend {
public:
    bool startOk = true;
    bool playing = false;
    int starts = 0;
    std::string last;
    bool lastLoop = false;
    float volume = -1.0f;
    double pos = 0.0;

    bool Start(const std::string& path, bool loop) override {
        if (!startOk) return false;
        ++starts;
        last = path;
        lastLoop = loop;
        playing = true;
        pos = 0.0;
        return true;
    }
    void Halt() override { playing = false; }
    void SetVolume(float v) override { volume = v; }
    bool Playing() const override { return playing; }
    double Position() const override { return pos; }
    bool Seek(double s) override { pos = s; return true; }
};

class FakeVoices : public VoiceBackend {
public:
    struct Channel {
        const void* chunk = nullptr;
        bool playing = false;
        float gain = -1.0f;
        float pan = -1.0f;
        float pitch = 1.0f;
        int pitchCalls = 0;
        int halts = 0;
    };
    std::map<int, Channel> channels;

    bool Playing(int channel, const void* chunk) const override {
        const auto it = channels.find(channel);
        return it != channels.end() && it->second.playing && it->second.chunk == chunk;
    }
    void Halt(int channel) override {
        channels[channel].playing = false;
        ++channels[channel].halts;
    }
    void SetVolume(int channel, float gain01) override { channels[channel].gain = gain01; }
    void SetPan(int channel, float pan01) override { channels[channel].pan = pan01; }
    void SetPitch(int channel, float factor) override {
        channels[channel].pitch = factor;
        ++channels[channel].pitchCalls;
    }
};

// Starts a voice on `channel` playing `chunk`.
VoiceStart Start(FakeVoices& be, int channel, const void* chunk, float volume = 1.0f) {
    be.channels[channel].chunk = chunk;
    be.channels[channel].playing = true;
    VoiceStart s;
    s.channel = channel;
    s.chunk = chunk;
    s.volume = volume;
    return s;
}

} // namespace

// ---- Ramps ------------------------------------------------------------------------------------------------------

NOX_TEST(AudioRuntime, RampInterpolatesLinearly) {
    AudioRamp r(0.0f);
    r.RampTo(1.0f, 2.0f);
    NOX_CHECK(r.Active());
    for (int i = 0; i < 60; ++i) r.Tick(kFixedDt);   // one second
    NOX_CHECK(Near(r.Value(), 0.5f, 0.01f));
    for (int i = 0; i < 70; ++i) r.Tick(kFixedDt);   // past the two-second end (float accumulation may fall just short)
    NOX_CHECK(!r.Active());
    NOX_CHECK(Near(r.Value(), 1.0f));
}

NOX_TEST(AudioRuntime, RampZeroDurationJumpsAndSetCancels) {
    AudioRamp r(0.25f);
    r.RampTo(0.75f, 0.0f);
    NOX_CHECK(!r.Active());
    NOX_CHECK(Near(r.Value(), 0.75f));
    r.RampTo(0.0f, 5.0f);
    r.Set(0.4f);
    NOX_CHECK(!r.Active());
    r.Tick(10.0f);
    NOX_CHECK(Near(r.Value(), 0.4f));
}

NOX_TEST(AudioRuntime, RampIgnoresNonFiniteInput) {
    AudioRamp r(0.5f);
    r.RampTo(std::nanf(""), 1.0f);
    NOX_CHECK(Near(r.Value(), 0.5f));
    r.Set(INFINITY);
    NOX_CHECK(Near(r.Value(), 0.5f));
    r.RampTo(1.0f, 1.0f);
    r.Tick(-1.0f);
    r.Tick(std::nanf(""));
    NOX_CHECK(Near(r.Value(), 0.5f));
}

// ---- Attenuation ------------------------------------------------------------------------------------------------

NOX_TEST(AudioRuntime, LegacyCurveMatchesHearingFalloff) {
    const float hearing = 1000.0f;
    const AudioSpatialParams legacy = AudioSpatialParams::Hearing(hearing);
    for (float d = 0.0f; d < 1500.0f; d += 7.3f) {
        const float expected = d >= hearing ? 0.0f : 1.0f - d / hearing;
        NOX_CHECK(Near(AudioAttenuationGain(d, legacy), expected, 1e-5f));
    }
}

NOX_TEST(AudioRuntime, InverseCurveReferenceAndSilence) {
    AudioSpatialParams p;
    p.curve = AudioAttenuation::Inverse;
    p.minDistance = 2.0f;
    p.maxDistance = 50.0f;
    p.rolloff = 1.0f;
    NOX_CHECK(Near(AudioAttenuationGain(0.0f, p), 1.0f));          // inside the reference distance
    NOX_CHECK(Near(AudioAttenuationGain(2.0f, p), 1.0f));          // at the reference distance
    NOX_CHECK(Near(AudioAttenuationGain(4.0f, p), 0.5f));          // ref / (ref + (d - ref))
    NOX_CHECK(Near(AudioAttenuationGain(50.0f, p), 0.0f));         // at max: silent
    NOX_CHECK(Near(AudioAttenuationGain(80.0f, p), 0.0f));
}

NOX_TEST(AudioRuntime, PowerCurveExponentAndLinearDefault) {
    AudioSpatialParams p;
    p.curve = AudioAttenuation::Power;
    p.minDistance = 0.0f;
    p.maxDistance = 100.0f;
    p.rolloff = 2.0f;
    NOX_CHECK(Near(AudioAttenuationGain(50.0f, p), 0.25f));        // (1 - 0.5) ^ 2
    p.rolloff = 1.0f;
    NOX_CHECK(Near(AudioAttenuationGain(25.0f, p), 0.75f));        // exponent 1 is linear
}

NOX_TEST(AudioRuntime, AttenuationRejectsNonFiniteAndParsesNames) {
    const AudioSpatialParams p = AudioSpatialParams::Hearing(10.0f);
    NOX_CHECK(Near(AudioAttenuationGain(std::nanf(""), p), 0.0f));
    NOX_CHECK(Near(AudioAttenuationGain(-3.0f, p), 0.0f));
    AudioAttenuation curve = AudioAttenuation::Linear;
    NOX_CHECK(ParseAudioAttenuation("inverse", curve) && curve == AudioAttenuation::Inverse);
    NOX_CHECK(ParseAudioAttenuation("power", curve) && curve == AudioAttenuation::Power);
    NOX_CHECK(!ParseAudioAttenuation("logarithmic", curve));
    NOX_CHECK(std::string(AudioAttenuationName(AudioAttenuation::Inverse)) == "inverse");
}

NOX_TEST(AudioRuntime, PanFollowsSourceDirection) {
    // A listener facing +x (yaw 0) has its right side on +z (right = cross(forward, up)).
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(5.0f, 0.0f, 0.0f), 0.0f), 0.5f));    // dead ahead
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(0.0f, 0.0f, 5.0f), 0.0f), 1.0f));    // to the right
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(0.0f, 0.0f, -5.0f), 0.0f), 0.0f));   // to the left
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(5.0f, 0.0f, 5.0f), 0.0f), 0.5f + 0.5f * std::sqrt(0.5f)));
}

NOX_TEST(AudioRuntime, PanIgnoresHeightOfSource) {
    // A source straight above the listener is not panned: the balance comes from the horizontal direction only.
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(0.0f, 5.0f, 0.0f), 0.0f), 0.5f));
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(0.0f, 9.0f, 0.0f), 90.0f), 0.5f));
    // A source to the listener's right and above it still pans right.
    // Horizontal part (4, 3) has length 5; its projection on the right side (+z) is 3/5.
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(4.0f, 4.0f, 3.0f), 0.0f), 0.5f + 0.5f * (3.0f / 5.0f)));
}

NOX_TEST(AudioRuntime, PanStaysRightAfterListenerTurnsAround) {
    // Source 5 m to the listener's right (+z when facing +x).
    const glm::vec3 right(0.0f, 0.0f, 5.0f);
    NOX_CHECK(Near(AudioPanFromDirection(right, 0.0f), 1.0f));
    // The listener turns 180 degrees (now faces -x): its right side is -z, so the same source is now on its left.
    NOX_CHECK(Near(AudioPanFromDirection(right, 180.0f), 0.0f));
    // The source is also 5 m to the right of the turned listener when it sits on the new right side (-z).
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(0.0f, 0.0f, -5.0f), 180.0f), 1.0f));
    // A quarter turn: facing +z, right is -x.
    NOX_CHECK(Near(AudioPanFromDirection(glm::vec3(-5.0f, 0.0f, 0.0f), 90.0f), 1.0f));
}

// ---- Occlusion --------------------------------------------------------------------------------------------------

NOX_TEST(AudioRuntime, OcclusionDisabledNeverRaycasts) {
    int calls = 0;
    AudioRayTest test = [&](const glm::vec3&, const glm::vec3&) { ++calls; return true; };
    AudioOcclusionState state;
    for (int i = 0; i < 60; ++i) {
        const float g = AudioUpdateOcclusion(state, false, glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f), test, kFixedDt);
        NOX_CHECK(Near(g, 1.0f));
    }
    NOX_CHECK_EQ(calls, 0);
}

NOX_TEST(AudioRuntime, OcclusionEasesInAndOutWithMockedRay) {
    bool blockedNow = false;
    int calls = 0;
    AudioRayTest test = [&](const glm::vec3&, const glm::vec3&) { ++calls; return blockedNow; };
    AudioOcclusionState state;
    const glm::vec3 listener(0.0f), source(10.0f, 0.0f, 0.0f);

    // Open line: no attenuation, one ray per tick.
    for (int i = 0; i < 10; ++i) AudioUpdateOcclusion(state, true, listener, source, test, kFixedDt);
    NOX_CHECK_EQ(calls, 10);
    NOX_CHECK(Near(state.Gain(), 1.0f));

    // A wall appears: the gain eases to 0.35 over the 0.15 s fade, not instantly.
    blockedNow = true;
    const float first = AudioUpdateOcclusion(state, true, listener, source, test, kFixedDt);
    NOX_CHECK(first < 1.0f && first > 0.35f);
    for (int i = 0; i < 20; ++i) AudioUpdateOcclusion(state, true, listener, source, test, kFixedDt);
    NOX_CHECK(Near(state.Gain(), AudioOcclusionState::kDefaultOccludedGain, 1e-4f));
    NOX_CHECK(state.Blocked());

    // The wall goes: back to open over the same fade.
    blockedNow = false;
    for (int i = 0; i < 20; ++i) AudioUpdateOcclusion(state, true, listener, source, test, kFixedDt);
    NOX_CHECK(Near(state.Gain(), 1.0f, 1e-4f));
    NOX_CHECK(!state.Blocked());
}

NOX_TEST(AudioRuntime, OcclusionFirstObservationDoesNotFadeIn) {
    AudioOcclusionState state;
    const float g = state.Update(true, 0.0f);
    NOX_CHECK(Near(g, AudioOcclusionState::kDefaultOccludedGain));
}

NOX_TEST(AudioRuntime, OcclusionNullRayTestIsOpen) {
    AudioOcclusionState state;
    const float g = AudioUpdateOcclusion(state, true, glm::vec3(0.0f), glm::vec3(3.0f, 0.0f, 0.0f), AudioRayTest(), kFixedDt);
    NOX_CHECK(Near(g, 1.0f));
}

// ---- Music ------------------------------------------------------------------------------------------------------

NOX_TEST(AudioRuntime, MusicFadeInReachesTargetOnTime) {
    FakeMusic be;
    AudioMusic music(be);
    std::string err;
    MusicPlayOptions o;
    o.volume = 0.8f;
    o.fadeIn = 1.0f;
    NOX_CHECK(music.Play("music/a.wav", o, err));
    for (int i = 0; i < 30; ++i) music.Tick(kFixedDt, UnitBus());
    NOX_CHECK(Near(be.volume, 0.4f, 0.02f));
    for (int i = 0; i < 40; ++i) music.Tick(kFixedDt, UnitBus());
    NOX_CHECK(Near(be.volume, 0.8f, 1e-3f));
}

NOX_TEST(AudioRuntime, MusicBusGainAppliesOnTop) {
    FakeMusic be;
    AudioMusic music(be);
    std::string err;
    MusicPlayOptions o;
    NOX_CHECK(music.Play("music/a.wav", o, err));
    music.Tick(kFixedDt, [](const std::string&) { return 0.25f; });
    NOX_CHECK(Near(be.volume, 0.25f));
}

NOX_TEST(AudioRuntime, MusicCrossfadeSwitchesAtHalfwayAndFadesIn) {
    FakeMusic be;
    AudioMusic music(be);
    std::string err;
    MusicPlayOptions o;
    NOX_CHECK(music.Play("music/a.wav", o, err));
    for (int i = 0; i < 60; ++i) music.Tick(kFixedDt, UnitBus());   // one second of track A

    NOX_CHECK(music.Crossfade("music/b.wav", 2.0f, o, err));
    NOX_CHECK(music.HasPending());
    int switchedAt = -1;
    float volumeAtHalf = -1.0f;
    for (int i = 1; i <= 180; ++i) {
        music.Tick(kFixedDt, UnitBus());
        if (i == 30) volumeAtHalf = be.volume;
        if (switchedAt < 0 && be.last == "music/b.wav") switchedAt = i;
    }
    // Outgoing track: 1 s of fade-out (half of 2 s). Its level is about half way at 0.5 s into the crossfade.
    NOX_CHECK(Near(volumeAtHalf, 0.5f, 0.03f));
    // The new track starts when the outgoing fade finishes (about 60 ticks, allowing for float accumulation).
    NOX_CHECK(switchedAt >= 59 && switchedAt <= 62);
    NOX_CHECK_EQ(be.starts, 2);
    NOX_CHECK(!music.HasPending());
    NOX_CHECK(Near(be.volume, 1.0f, 1e-3f));   // new track faded in over the second half
    NOX_CHECK(be.playing);
}

NOX_TEST(AudioRuntime, MusicCrossfadeWithNothingPlayingFadesIn) {
    FakeMusic be;
    AudioMusic music(be);
    std::string err;
    MusicPlayOptions o;
    NOX_CHECK(music.Crossfade("music/b.wav", 1.0f, o, err));
    NOX_CHECK(be.last == "music/b.wav");
    for (int i = 0; i < 30; ++i) music.Tick(kFixedDt, UnitBus());
    NOX_CHECK(Near(be.volume, 0.5f, 0.02f));
}

NOX_TEST(AudioRuntime, MusicStopFadesThenHalts) {
    FakeMusic be;
    AudioMusic music(be);
    std::string err;
    MusicPlayOptions o;
    NOX_CHECK(music.Play("music/a.wav", o, err));
    music.Tick(kFixedDt, UnitBus());
    music.Stop(0.5f);
    for (int i = 0; i < 15; ++i) music.Tick(kFixedDt, UnitBus());   // 0.25 s into a 0.5 s fade
    NOX_CHECK(Near(be.volume, 0.5f, 0.03f));
    NOX_CHECK(be.playing);
    for (int i = 0; i < 20; ++i) music.Tick(kFixedDt, UnitBus());   // past the end of the fade
    NOX_CHECK(!be.playing);
    NOX_CHECK(!music.IsPlaying());
}

NOX_TEST(AudioRuntime, MusicStreamEndClearsTrackUnlessLooping) {
    FakeMusic be;
    AudioMusic music(be);
    std::string err;
    MusicPlayOptions o;
    NOX_CHECK(music.Play("music/a.wav", o, err));
    be.playing = false;   // the decoder reached the end
    music.Tick(kFixedDt, UnitBus());
    NOX_CHECK(!music.IsPlaying());
    NOX_CHECK(music.Position() < 0.0);                    // no track any more

    MusicPlayOptions looping;
    looping.loop = true;
    NOX_CHECK(music.Play("music/a.wav", looping, err));
    NOX_CHECK(be.lastLoop);
    be.playing = false;                                   // a looping track is kept (the backend owns the restart)
    music.Tick(kFixedDt, UnitBus());
    NOX_CHECK(music.Position() >= 0.0);
}

NOX_TEST(AudioRuntime, MusicFailedStartReportsError) {
    FakeMusic be;
    be.startOk = false;
    AudioMusic music(be);
    std::string err;
    MusicPlayOptions o;
    NOX_CHECK(!music.Play("music/missing.wav", o, err));
    NOX_CHECK(!err.empty());
    NOX_CHECK(!music.IsPlaying());
}

// ---- One-shot voices --------------------------------------------------------------------------------------------

NOX_TEST(AudioRuntime, VoiceGainIsVolumeTimesBus) {
    FakeVoices be;
    AudioVoices voices;
    int chunk = 0;
    VoiceStart s = Start(be, 3, &chunk, 0.8f);
    s.bus = "sfx";
    const uint64_t id = voices.Add(s, be, VoiceListener{}, [](const std::string&) { return 0.5f; }, AudioRayTest());
    NOX_CHECK(id != 0);
    NOX_CHECK(Near(be.channels[3].gain, 0.4f));
    NOX_CHECK(voices.IsPlaying(id, be));
}

NOX_TEST(AudioRuntime, VoiceStopWithFadeHaltsAfterRamp) {
    FakeVoices be;
    AudioVoices voices;
    int chunk = 0;
    const uint64_t id = voices.Add(Start(be, 1, &chunk), be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK(voices.Stop(id, 0.25f, be));
    for (int i = 0; i < 10; ++i) voices.Tick(kFixedDt, be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK(be.channels[1].playing);                    // still fading
    NOX_CHECK(be.channels[1].gain < 0.7f && be.channels[1].gain > 0.0f);
    for (int i = 0; i < 10; ++i) voices.Tick(kFixedDt, be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK(!be.channels[1].playing);                   // halted at silence
    NOX_CHECK_EQ(be.channels[1].halts, 1);
    NOX_CHECK_EQ(voices.Count(), static_cast<size_t>(0));
}

NOX_TEST(AudioRuntime, VoiceStopImmediateHaltsNow) {
    FakeVoices be;
    AudioVoices voices;
    int chunk = 0;
    const uint64_t id = voices.Add(Start(be, 2, &chunk), be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK(voices.Stop(id, 0.0f, be));
    NOX_CHECK(!be.channels[2].playing);
    NOX_CHECK(!voices.IsPlaying(id, be));
    NOX_CHECK(!voices.Stop(id, 0.0f, be));                // second stop: the handle is gone
}

NOX_TEST(AudioRuntime, VoiceHandleIsStaleWhenChannelIsReused) {
    FakeVoices be;
    AudioVoices voices;
    int first = 0, second = 0;
    const uint64_t oldId = voices.Add(Start(be, 4, &first), be, VoiceListener{}, UnitBus(), AudioRayTest());
    // The first sound ends and channel 4 starts another sound before the registry ticked.
    be.channels[4].playing = false;
    const uint64_t newId = voices.Add(Start(be, 4, &second), be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK(newId != oldId);
    NOX_CHECK(!voices.IsPlaying(oldId, be));              // the channel plays someone else now
    NOX_CHECK(voices.IsPlaying(newId, be));
    voices.Tick(kFixedDt, be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK_EQ(voices.Count(), static_cast<size_t>(1)); // the finished voice was dropped, the new one kept
    NOX_CHECK(!voices.Stop(oldId, 0.0f, be));             // stale handles do nothing
    NOX_CHECK(be.channels[4].playing);
}

NOX_TEST(AudioRuntime, VoiceVolumeAndPitchRamp) {
    FakeVoices be;
    AudioVoices voices;
    int chunk = 0;
    const uint64_t id = voices.Add(Start(be, 5, &chunk, 1.0f), be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK(voices.SetVolume(id, 0.2f, 0.5f));
    NOX_CHECK(voices.SetPitch(id, 2.0f, 0.5f));
    for (int i = 0; i < 15; ++i) voices.Tick(kFixedDt, be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK(Near(be.channels[5].gain, 0.6f, 0.02f));     // 1.0 -> 0.2 over half a second, halfway
    NOX_CHECK(Near(be.channels[5].pitch, 1.5f, 0.05f));    // 1.0 -> 2.0 over half a second, halfway
    const int callsMidway = be.channels[5].pitchCalls;
    for (int i = 0; i < 60; ++i) voices.Tick(kFixedDt, be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK(Near(be.channels[5].gain, 0.2f, 1e-3f));
    NOX_CHECK(Near(be.channels[5].pitch, 2.0f));
    NOX_CHECK(be.channels[5].pitchCalls > callsMidway);    // re-applied only while it changes
    const int callsSettled = be.channels[5].pitchCalls;
    for (int i = 0; i < 10; ++i) voices.Tick(kFixedDt, be, VoiceListener{}, UnitBus(), AudioRayTest());
    NOX_CHECK_EQ(be.channels[5].pitchCalls, callsSettled);
}

NOX_TEST(AudioRuntime, VoiceSpatialAttenuationAndOcclusion) {
    FakeVoices be;
    AudioVoices voices;
    int chunk = 0;
    VoiceStart s = Start(be, 6, &chunk, 1.0f);
    s.spatial.spatial = true;
    s.spatial.position = glm::vec3(10.0f, 0.0f, 0.0f);
    s.spatial.params = AudioSpatialParams::Hearing(20.0f);   // linear: half volume at 10 m
    s.spatial.occlusion = true;
    VoiceListener listener;
    bool blocked = false;
    int rays = 0;
    AudioRayTest ray = [&](const glm::vec3&, const glm::vec3&) { ++rays; return blocked; };
    const uint64_t id = voices.Add(s, be, listener, UnitBus(), ray);
    NOX_CHECK(id != 0);
    NOX_CHECK(Near(be.channels[6].gain, 0.5f, 1e-3f));
    NOX_CHECK(Near(be.channels[6].pan, 0.5f, 1e-3f));       // straight ahead

    blocked = true;
    for (int i = 0; i < 20; ++i) voices.Tick(kFixedDt, be, listener, UnitBus(), ray);
    NOX_CHECK(Near(be.channels[6].gain, 0.5f * AudioOcclusionState::kDefaultOccludedGain, 1e-3f));
    NOX_CHECK(rays > 0);
}

NOX_TEST(AudioRuntime, VoiceSpatialOcclusionOffNeverRaycasts) {
    FakeVoices be;
    AudioVoices voices;
    int chunk = 0;
    VoiceStart s = Start(be, 7, &chunk, 1.0f);
    s.spatial.spatial = true;
    s.spatial.position = glm::vec3(3.0f, 0.0f, 0.0f);
    s.spatial.params = AudioSpatialParams::Hearing(100.0f);
    s.spatial.occlusion = false;
    int rays = 0;
    AudioRayTest ray = [&](const glm::vec3&, const glm::vec3&) { ++rays; return true; };
    voices.Add(s, be, VoiceListener{}, UnitBus(), ray);
    for (int i = 0; i < 30; ++i) voices.Tick(kFixedDt, be, VoiceListener{}, UnitBus(), ray);
    NOX_CHECK_EQ(rays, 0);
}

// ---- Mixer ------------------------------------------------------------------------------------------------------

NOX_TEST(AudioRuntime, MixerFadeBusRampsAndTargets) {
    AudioMixer& mixer = AudioMixer::Get();
    mixer.Reset();
    NOX_CHECK(mixer.FadeBus("music", 0.2f, 1.0f));
    NOX_CHECK(Near(mixer.BusTargetVolume("music"), 0.2f));
    for (int i = 0; i < 30; ++i) mixer.Tick(kFixedDt);
    NOX_CHECK(Near(mixer.BusVolume("music"), 0.6f, 0.02f));
    for (int i = 0; i < 40; ++i) mixer.Tick(kFixedDt);
    NOX_CHECK(Near(mixer.BusVolume("music"), 0.2f));
    NOX_CHECK(!mixer.FadeBus("nope", 0.5f, 1.0f));
    mixer.Reset();
}

NOX_TEST(AudioRuntime, MixerDuckRestoresBaseVolume) {
    AudioMixer& mixer = AudioMixer::Get();
    mixer.Reset();
    NOX_CHECK(mixer.SetBusVolume("music", 0.8f));
    NOX_CHECK(mixer.Duck("music", 0.3f, 0.2f));
    NOX_CHECK(mixer.IsDucked("music"));
    NOX_CHECK(mixer.Duck("music", 0.1f, 0.0f));           // a second duck keeps the first base (0.8)
    NOX_CHECK(Near(mixer.BusVolume("music"), 0.1f));
    NOX_CHECK(mixer.Unduck("music", 0.2f));
    NOX_CHECK(!mixer.IsDucked("music"));
    for (int i = 0; i < 20; ++i) mixer.Tick(kFixedDt);
    NOX_CHECK(Near(mixer.BusVolume("music"), 0.8f, 1e-3f));
    NOX_CHECK(!mixer.Unduck("music", 0.2f));              // not ducked any more
    mixer.Reset();
}

NOX_TEST(AudioRuntime, MixerSetVolumeCancelsDuck) {
    AudioMixer& mixer = AudioMixer::Get();
    mixer.Reset();
    NOX_CHECK(mixer.Duck("voice", 0.2f, 0.0f));
    NOX_CHECK(mixer.SetBusVolume("voice", 0.9f));
    NOX_CHECK(!mixer.IsDucked("voice"));
    NOX_CHECK(Near(mixer.BusVolume("voice"), 0.9f));
    mixer.Reset();
}
