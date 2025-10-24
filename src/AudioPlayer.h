// AudioPlayer.h
#pragma once
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#include <glm/glm.hpp>
#include <iostream>
#include <string>
#include <cmath>
#include <cstring>

// Structure for doppler effect parameters.
struct DopplerData {
    float pitchFactor; // >1.0 raises the pitch (approaching), <1.0 lowers it (receding)
};

class AudioPlayer {
public:
    AudioPlayer() {}

    // Initializes SDL audio and SDL_mixer.
    bool init() {
        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
            std::cerr << "SDL Init Error: " << SDL_GetError() << std::endl;
            return false;
        }
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
            std::cerr << "SDL_mixer Init Error: " << Mix_GetError() << std::endl;
            return false;
        }
        return true;
    }


    // Loads a WAV file. (Other formats supported by SDL_mixer might work.)
    Mix_Chunk* loadSound(const std::string& filename) {
        Mix_Chunk* sound = Mix_LoadWAV(filename.c_str());
        if (!sound) {
            std::cerr << "Failed to load sound (" << filename << "): " << Mix_GetError() << std::endl;
        }
        return sound;
    }

    // Plays the given sound.
    // Parameters:
    // - loop: whether to loop the sound.
    // - is3d: if true, source and listener positions are used.
    // - pitchFactor: factor to adjust pitch (1.0 = normal).
    // - volume: a multiplier (0.0–1.0) for the sound volume.
    // - hearingDistance: maximum distance for hearing (affects attenuation).
    // Returns the channel number (or -1 on error).
    int playSound(Mix_Chunk* sound, bool loop = false, bool is3d = false,
        const glm::vec3& sourcePos = glm::vec3(0.0f),
        const glm::vec3& listenerPos = glm::vec3(0.0f),
        float listenerAngle = 0.0f,
        float pitchFactor = 1.0f,
        float volume = 1.0f,
        float hearingDistance = 1000.0f)
    {
        int loops = loop ? -1 : 0;
        int channel = Mix_PlayChannel(-1, sound, loops);
        if (channel == -1) {
            std::cerr << "Failed to play sound: " << Mix_GetError() << std::endl;
            return -1;
        }
        if (is3d) {
            setChannel3DPosition(channel, sourcePos, listenerPos, listenerAngle, hearingDistance, volume);
        }
        else {
            // For 2D playback, apply the volume multiplier.
            Uint8 vol = static_cast<Uint8>(128 * volume);
            Mix_Volume(channel, vol);
            Mix_SetPanning(channel, 255, 255);
        }
        if (std::fabs(pitchFactor - 1.0f) >= 0.001f) {
            setChannelDopplerEffect(channel, pitchFactor);
        }
        return channel;
    }

	//Pauses the sound
	void pauseSound(int channel) 
    {
		Mix_Pause(channel);
    }

    // Sets the 3D position for a channel using glm::vec3.
    // 'hearingDistance' controls the maximum distance (for attenuation),
    // and 'volume' scales the final computed volume.
    void setChannel3DPosition(int channel, const glm::vec3& sourcePos,
        const glm::vec3& listenerPos, float listenerAngle,
        float hearingDistance, float volume)
    {
        // Check if the channel is valid and not deleted.
        if (channel == -1 || Mix_GetChunk(channel) == nullptr) {
            std::cerr << "Invalid channel." << std::endl;
            return;
        }

        // Compute difference vector.
        glm::vec3 diff = sourcePos - listenerPos;

        // Compute distance and apply linear attenuation based on hearingDistance.
        float distance = glm::length(diff);
        if (distance > hearingDistance) {
            distance = hearingDistance;
        }
        Uint8 vol = static_cast<Uint8>(128 * volume * (1.0f - distance / hearingDistance));
        Mix_Volume(channel, vol);

        // Compute the angle (in degrees) from listener to source.
        float angle = std::atan2f(diff.y, diff.x) * 180.0f / M_PI;
        float relativeAngle = angle - listenerAngle;
        while (relativeAngle < 0) {
            relativeAngle += 360.0f;
        }
        while (relativeAngle >= 360.0f) {
            relativeAngle -= 360.0f;
        }

        // Calculate panning based on the sine of the relative angle.
        float panFactor = static_cast<float>((std::sin(relativeAngle * M_PI / 180.0f) + 1.0f) / 2.0f);
        Uint8 right = static_cast<Uint8>(255 * panFactor);
        Uint8 left = static_cast<Uint8>(255 * (1.0f - panFactor));
        Mix_SetPanning(channel, left, right);
    }


    // Registers a doppler effect on the specified channel using a pitch factor.
    // A pitchFactor of 1.0 means no pitch change.
    void setChannelDopplerEffect(int channel, float pitchFactor) {
        Mix_UnregisterAllEffects(channel);
        if (std::fabs(pitchFactor - 1.0f) < 0.001f)
            return;
        DopplerData* data = new DopplerData();
        data->pitchFactor = pitchFactor;
        if (Mix_RegisterEffect(channel, AudioPlayer::DopplerEffectCallback, nullptr, data) == 0) {
            std::cerr << "Failed to register doppler effect: " << Mix_GetError() << std::endl;
            delete data;
        }
    }

private:
    // Custom effect callback that applies a rudimentary pitch shift (doppler effect)
    // by resampling the audio chunk using linear interpolation.
    static void DopplerEffectCallback(int channel, void* stream, int len, void* udata) {
        DopplerData* data = static_cast<DopplerData*>(udata);
        const int channels = 2; // assuming stereo, AUDIO_S16LSB
        const int sampleCount = len / (sizeof(Sint16) * channels);
        Sint16* orig = new Sint16[sampleCount * channels];
        std::memcpy(orig, stream, len);

        for (int i = 0; i < sampleCount; i++) {
            float srcPos = i * data->pitchFactor;
            int index = static_cast<int>(srcPos);
            float frac = srcPos - index;
            for (int ch = 0; ch < channels; ch++) {
                Sint16 s1 = (index < sampleCount) ? orig[index * channels + ch] : 0;
                Sint16 s2 = ((index + 1) < sampleCount) ? orig[(index + 1) * channels + ch] : 0;
                float sample = s1 + frac * (s2 - s1);
                ((Sint16*)stream)[i * channels + ch] = static_cast<Sint16>(sample);
            }
        }
        delete[] orig;
    }
};
