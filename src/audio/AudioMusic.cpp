#include "AudioMusic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr float kVolumeEpsilon = 1e-4f;
constexpr float kSilent = 1e-4f;

float ClampUnit(float v) {
    if (!std::isfinite(v)) return 0.0f;
    return std::clamp(v, 0.0f, 1.0f);
}

} // namespace

AudioMusic::AudioMusic(MusicBackend& backend) : m_backend(backend) {}

bool AudioMusic::StartTrack(const Track& track, std::string& error) {
    m_backend.Halt();
    m_appliedVolume = -1.0f;
    if (!m_backend.Start(track.path, track.loop)) {
        error = "could not play music '" + track.path + "'";
        m_hasTrack = false;
        return false;
    }
    m_track = track;
    m_hasTrack = true;
    m_stopAfterFade = false;
    return true;
}

void AudioMusic::Clear() {
    if (m_hasTrack) m_backend.Halt();
    m_hasTrack = false;
    m_hasPending = false;
    m_stopAfterFade = false;
    m_appliedVolume = -1.0f;
}

bool AudioMusic::Play(const std::string& path, const MusicPlayOptions& options, std::string& error) {
    if (path.empty()) {
        error = "play_music: empty path";
        return false;
    }
    Track track;
    track.path = path;
    track.loop = options.loop;
    track.bus = options.bus.empty() ? std::string("music") : options.bus;
    track.volume = ClampUnit(options.volume);
    m_hasPending = false;
    if (!StartTrack(track, error)) return false;
    if (options.fadeIn > 0.0f) {
        m_track.level.Set(0.0f);
        m_track.level.RampTo(track.volume, options.fadeIn);
    }
    else {
        m_track.level.Set(track.volume);
    }
    return true;
}

void AudioMusic::Stop(float fadeOut) {
    m_hasPending = false;
    if (!m_hasTrack) return;
    if (fadeOut > 0.0f && std::isfinite(fadeOut)) {
        m_track.level.RampTo(0.0f, fadeOut);
        m_stopAfterFade = true;
        return;
    }
    Clear();
}

bool AudioMusic::Crossfade(const std::string& path, float seconds, const MusicPlayOptions& options, std::string& error) {
    if (!m_hasTrack) {
        MusicPlayOptions fadeOptions = options;
        fadeOptions.fadeIn = seconds;
        return Play(path, fadeOptions, error);
    }
    if (path.empty()) {
        error = "crossfade_music: empty path";
        return false;
    }
    const float half = (std::isfinite(seconds) && seconds > 0.0f) ? seconds * 0.5f : 0.0f;
    Track next;
    next.path = path;
    next.loop = options.loop;
    next.bus = options.bus.empty() ? std::string("music") : options.bus;
    next.volume = ClampUnit(options.volume);
    m_pending = next;
    m_pending.level.Set(0.0f);
    m_hasPending = true;
    // Fade-in length is applied when the pending track starts (see Tick).
    m_pendingFadeIn = half;
    m_track.level.RampTo(0.0f, half);
    m_stopAfterFade = false;
    return true;
}

void AudioMusic::SetVolume(float volume, float seconds) {
    if (!m_hasTrack) return;
    m_stopAfterFade = false;
    m_track.level.RampTo(ClampUnit(volume), seconds);
}

bool AudioMusic::Seek(double seconds) {
    if (!m_hasTrack || !std::isfinite(seconds) || seconds < 0.0) return false;
    return m_backend.Seek(seconds);
}

double AudioMusic::Position() const {
    if (!m_hasTrack) return -1.0;
    return m_backend.Position();
}

bool AudioMusic::IsPlaying() const {
    return m_hasPending || (m_hasTrack && m_backend.Playing());
}

void AudioMusic::Tick(float dt, const std::function<float(const std::string&)>& busGain) {
    if (m_hasTrack) m_track.level.Tick(dt);

    // A pending crossfade starts once the outgoing track is silent (or nothing is playing).
    if (m_hasPending) {
        const bool outgoingSilent = !m_hasTrack || (!m_track.level.Active() && m_track.level.Value() <= kSilent);
        if (outgoingSilent) {
            std::string error;
            Track next = m_pending;
            m_hasPending = false;
            if (StartTrack(next, error)) {
                m_track.level.Set(0.0f);
                if (m_pendingFadeIn > 0.0f) m_track.level.RampTo(next.volume, m_pendingFadeIn);
                else m_track.level.Set(next.volume);
            }
            else {
                std::fprintf(stderr, "[Audio] crossfade failed: %s\n", error.c_str());
            }
        }
    }

    if (m_hasTrack && m_stopAfterFade && !m_track.level.Active() && m_track.level.Value() <= kSilent) {
        Clear();
        return;
    }
    if (m_hasTrack && !m_track.loop && !m_backend.Playing()) {
        // The stream ended on its own.
        m_hasTrack = false;
        m_appliedVolume = -1.0f;
        return;
    }
    if (!m_hasTrack) return;

    const float gain = busGain ? busGain(m_track.bus) : 1.0f;
    const float out = ClampUnit(m_track.level.Value() * ClampUnit(gain));
    if (m_appliedVolume < 0.0f || std::fabs(out - m_appliedVolume) > kVolumeEpsilon) {
        m_backend.SetVolume(out);
        m_appliedVolume = out;
    }
}
