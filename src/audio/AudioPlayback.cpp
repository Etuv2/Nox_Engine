#include "AudioPlayback.h"

#include "../AudioPlayer.h"
#include "AudioListener.h"
#include "AudioMixer.h"
#include "AudioSpatial.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

namespace {

constexpr size_t kMaxCachedSounds = 128;
constexpr float kOneShotHearingDistance = 1000.0f;   // same default as AudioNode

// One output device for one-shots and music, and a cache of decoded one-shot chunks (kept for the process lifetime).
struct DeviceState {
    AudioPlayer player;
    bool tried = false;
    bool ready = false;
    std::map<std::string, Mix_Chunk*> chunks;
};

DeviceState& Device() {
    static DeviceState state;
    return state;
}

Mix_Chunk* LoadCached(const std::string& path, std::string& error) {
    DeviceState& s = Device();
    const auto it = s.chunks.find(path);
    if (it != s.chunks.end()) return it->second;
    if (s.chunks.size() >= kMaxCachedSounds) {
        error = "too many distinct one-shot sounds (limit " + std::to_string(kMaxCachedSounds) + ")";
        return nullptr;
    }
    Mix_Chunk* chunk = s.player.loadSound(path);
    if (!chunk) {
        error = "could not load sound '" + path + "'";
        return nullptr;
    }
    s.chunks[path] = chunk;
    return chunk;
}

float BusGainOf(const std::string& name) {
    return AudioMixer::Get().BusGain(name);
}

class SdlVoiceBackend : public VoiceBackend {
public:
    bool Playing(int channel, const void* chunk) const override {
        return chunk && Mix_GetChunk(channel) == chunk && Mix_Playing(channel) != 0;
    }
    void Halt(int channel) override {
        if (channel >= 0) Mix_HaltChannel(channel);
    }
    void SetVolume(int channel, float gain01) override {
        Mix_Volume(channel, static_cast<int>(std::lround(std::clamp(gain01, 0.0f, 1.0f) * 128.0f)));
    }
    void SetPan(int channel, float pan01) override {
        const float p = std::clamp(pan01, 0.0f, 1.0f);
        Mix_SetPanning(channel, static_cast<Uint8>(255 * (1.0f - p)), static_cast<Uint8>(255 * p));
    }
    void SetPitch(int channel, float factor) override {
        Device().player.setChannelDopplerEffect(channel, factor);
    }
};

// The stream is released by Halt(). No destructor: SDL may already be shut down when statics are destroyed.
class SdlMusicBackend : public MusicBackend {
public:
    bool Start(const std::string& path, bool loop) override {
        if (!AudioPlaybackEnsureDevice()) return false;
        Halt();
        SDL_RWops* rw = AssetIO::OpenRWops(path);
        if (!rw) {
            std::cerr << "[Audio] could not open music '" << path << "'\n";
            return false;
        }
        Mix_Music* music = Mix_LoadMUS_RW(rw, 1);   // SDL takes the RWops; WAV/OGG/MP3 stream from it
        if (!music) {
            std::cerr << "[Audio] could not decode music '" << path << "': " << Mix_GetError() << "\n";
            return false;
        }
        m_music = music;
        Mix_VolumeMusic(MIX_MAX_VOLUME);
        if (Mix_PlayMusic(m_music, loop ? -1 : 0) != 0) {
            std::cerr << "[Audio] could not play music '" << path << "': " << Mix_GetError() << "\n";
            Halt();
            return false;
        }
        return true;
    }
    void Halt() override {
        if (m_music) {
            Mix_HaltMusic();
            Mix_FreeMusic(m_music);
            m_music = nullptr;
        }
    }
    void SetVolume(float volume01) override {
        Mix_VolumeMusic(static_cast<int>(std::lround(std::clamp(volume01, 0.0f, 1.0f) * MIX_MAX_VOLUME)));
    }
    bool Playing() const override {
        return m_music && Mix_PlayingMusic() != 0;
    }
    double Position() const override {
        return m_music ? Mix_GetMusicPosition(m_music) : -1.0;
    }
    bool Seek(double seconds) override {
        return m_music && Mix_SetMusicPosition(seconds) == 0;
    }

private:
    Mix_Music* m_music = nullptr;
};

SdlMusicBackend& MusicBackendInstance() {
    static SdlMusicBackend backend;
    return backend;
}

AudioVoices& Voices() {
    static AudioVoices voices;
    return voices;
}

VoiceListener ListenerNow() {
    const AudioListenerPose pose = AudioListenerCurrent();
    VoiceListener l;
    l.position = pose.position;
    l.angleDegrees = pose.angleDegrees;
    return l;
}

} // namespace

bool AudioPlaybackEnsureDevice() {
    DeviceState& s = Device();
    if (!s.tried) {
        s.tried = true;
        s.ready = s.player.init();
    }
    return s.ready;
}

AudioMusic& AudioPlaybackMusic() {
    static AudioMusic music(MusicBackendInstance());
    return music;
}

AudioVoices& AudioPlaybackVoices() {
    return Voices();
}

bool AudioPlaybackVoiceIsPlaying(uint64_t id) {
    SdlVoiceBackend backend;
    return Voices().IsPlaying(id, backend);
}

bool AudioPlaybackVoiceStop(uint64_t id, float fadeSeconds) {
    SdlVoiceBackend backend;
    return Voices().Stop(id, fadeSeconds, backend);
}

bool AudioPlaybackVoiceSetVolume(uint64_t id, float volume, float seconds) {
    return Voices().SetVolume(id, volume, seconds);
}

bool AudioPlaybackVoiceSetPitch(uint64_t id, float factor, float seconds) {
    return Voices().SetPitch(id, factor, seconds);
}

uint64_t AudioPlaybackPlayOneShot(const OneShotRequest& request, std::string& error) {
    if (!AudioPlaybackEnsureDevice()) {
        error = "audio device unavailable";
        return 0;
    }
    Mix_Chunk* chunk = LoadCached(request.path, error);
    if (!chunk) return 0;

    const float pitch = std::clamp(request.pitch, 0.1f, 10.0f);
    const VoiceListener listener = ListenerNow();
    AudioPlayer& player = Device().player;
    int channel = -1;
    if (request.spatial.spatial) {
        channel = player.playSound(chunk, false, true, request.spatial.position, listener.position,
            listener.angleDegrees, pitch, 1.0f, kOneShotHearingDistance);
    }
    else {
        channel = player.playSound(chunk, false, false, glm::vec3(0.0f), glm::vec3(0.0f), 0.0f,
            pitch, 1.0f, kOneShotHearingDistance);
    }
    if (channel < 0) {
        error = "no free mixer channel";
        return 0;
    }

    VoiceStart start;
    start.channel = channel;
    start.chunk = chunk;
    start.bus = request.bus.empty() ? std::string(AudioMixer::kDefaultSourceBus) : request.bus;
    start.volume = request.volume;
    start.pitch = pitch;
    start.spatial = request.spatial;
    SdlVoiceBackend backend;
    return AudioPlaybackVoices().Add(start, backend, listener, BusGainOf, AudioGetRayTest());
}

namespace {
float g_frameDt = 0.0f;
} // namespace

float AudioPlaybackFrameDt() {
    return g_frameDt;
}

void AudioPlaybackTick(float dt) {
    g_frameDt = std::isfinite(dt) && dt > 0.0f ? dt : 0.0f;
    AudioMixer::Get().Tick(dt);
    AudioPlaybackMusic().Tick(dt, BusGainOf);
    AudioVoices& voices = AudioPlaybackVoices();
    if (voices.Count() == 0) return;
    SdlVoiceBackend backend;
    voices.Tick(dt, backend, ListenerNow(), BusGainOf, AudioGetRayTest());
}
