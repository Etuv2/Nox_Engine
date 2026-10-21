#pragma once
// Playback runtime for music and one-shot voices (engine service, SDL_mixer backend). Core ticks it once per frame in
// the audio stage with unscaled frame time, so fades keep running while the simulation is paused.
#include "AudioMusic.h"
#include "AudioVoices.h"

#include <cstdint>
#include <string>

// Opens the output device on first use. False when no device is available.
bool AudioPlaybackEnsureDevice();

// The process-wide music track (streams through Mix_LoadMUS_RW).
AudioMusic& AudioPlaybackMusic();

struct OneShotRequest {
    std::string path;
    std::string bus = "sfx";
    float volume = 1.0f;     // 0..1
    float pitch = 1.0f;      // factor, clamped to [0.1, 10]
    VoiceSpatial spatial;    // spatial.spatial = false plays centred and unattenuated
};

// Starts a one-shot. Returns its voice id (0 on failure, with error set).
uint64_t AudioPlaybackPlayOneShot(const OneShotRequest& request, std::string& error);
// Handle operations on a one-shot by id. A stale id (the sound finished) is simply not playing.
bool AudioPlaybackVoiceIsPlaying(uint64_t id);
bool AudioPlaybackVoiceStop(uint64_t id, float fadeSeconds);
bool AudioPlaybackVoiceSetVolume(uint64_t id, float volume, float seconds);
bool AudioPlaybackVoiceSetPitch(uint64_t id, float factor, float seconds);

// One frame of audio: bus ramps, music fades, and voice fades and spatial gains for the current listener.
void AudioPlaybackTick(float dt);
// Seconds passed to the last AudioPlaybackTick (occlusion ramps on nodes use it).
float AudioPlaybackFrameDt();
