#pragma once
// Music playback state (engine service): one streamed track at a time, with fades and a dip crossfade.
//
// SDL_mixer plays one music stream at a time, so a crossfade is sequential: the current track fades out over half the
// requested time, then the next one fades in over the other half. Fades are ramps ticked by the audio stage, not
// SDL's own music fades, so the mixer bus gain can be applied on top of them every tick.
//
// The decoder sits behind MusicBackend. The SDL implementation streams through Mix_LoadMUS_RW (WAV, OGG, MP3, FLAC as
// SDL_mixer supports them); tests use a fake backend and drive Tick() with a fixed step.
#include "AudioRamp.h"

#include <functional>
#include <string>

class MusicBackend {
public:
    virtual ~MusicBackend() = default;
    // Starts a stream from the beginning. False when the file cannot be opened or played.
    virtual bool Start(const std::string& path, bool loop) = 0;
    virtual void Halt() = 0;
    // Output volume of the stream, 0..1 (already includes the bus gain).
    virtual void SetVolume(float volume01) = 0;
    virtual bool Playing() const = 0;
    // Playback position in seconds, or a negative value when the format cannot report it.
    virtual double Position() const = 0;
    virtual bool Seek(double seconds) = 0;
};

struct MusicPlayOptions {
    bool loop = false;
    float volume = 1.0f;     // level after the fade in, 0..1
    float fadeIn = 0.0f;     // seconds; 0 starts at full level
    std::string bus = "music";
};

class AudioMusic {
public:
    explicit AudioMusic(MusicBackend& backend);

    // Starts the track now, replacing the current one without a fade (use Crossfade for a smooth change).
    bool Play(const std::string& path, const MusicPlayOptions& options, std::string& error);
    // Fades the current track out over fadeOut seconds and stops it (<= 0 stops at once). Cancels a pending crossfade.
    void Stop(float fadeOut);
    // Fades the current track out over half of `seconds`, then fades the new one in over the other half. With nothing
    // playing, the new track fades in over `seconds`.
    bool Crossfade(const std::string& path, float seconds, const MusicPlayOptions& options, std::string& error);
    // Ramps the level of the current track (0..1) over `seconds`. Cancels a stop that is waiting for its fade.
    void SetVolume(float volume, float seconds);

    bool Seek(double seconds);
    double Position() const;
    // True while a track is audible or fading (a pending crossfade counts as playing).
    bool IsPlaying() const;
    bool HasPending() const { return m_hasPending; }
    const std::string& CurrentPath() const { return m_track.path; }
    float Level() const { return m_track.level.Value(); }

    // Advances fades by dt seconds and applies the output volume. busGain maps a bus name to its gain (mixer).
    void Tick(float dt, const std::function<float(const std::string&)>& busGain);

private:
    struct Track {
        std::string path;
        bool loop = false;
        std::string bus = "music";
        float volume = 1.0f;
        AudioRamp level{ 0.0f };   // full user level including fades
    };

    bool StartTrack(const Track& track, std::string& error);
    void Clear();

    MusicBackend& m_backend;
    bool m_hasTrack = false;
    Track m_track;
    bool m_stopAfterFade = false;
    bool m_hasPending = false;
    Track m_pending;
    float m_pendingFadeIn = 0.0f;   // fade-in length of the pending track, seconds
    float m_appliedVolume = -1.0f;
};
