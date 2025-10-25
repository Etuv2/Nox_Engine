// AudioNode.h
#pragma once
#include "SceneNode.h"
#include "AudioPlayer.h"
#include <glm/glm.hpp>
#include <string>
#include <SDL/SDL_mixer.h>

class AudioNode : public SceneNode {
public:
    AudioNode();
    virtual ~AudioNode();

    // Initialize the audio node by loading the sound file.
    // Returns true if successful.
    bool initAudio(const std::string& soundFile);

    // Play the audio.
    // 'loop' specifies if the sound should loop, and 'is3d' indicates whether to use 3D positioning.
    void play(bool loop = false, bool is3d = true);

    // Stop the audio playback.
    void stop();

    // Update the audio source position based on this node's global transform.
    // 'listenerPos' and 'listenerAngle' (in degrees) are used to compute relative positioning.
    void updateAudio(const glm::vec3& listenerPos, float listenerAngle);

    // Validated setters for additional audio properties that prevent NaN/Infinity values.
    void setPitch(float pitch);
    void setVolume(float volume);
    void setHearingDistance(float distance);

    // Getters for audio properties
    float getPitch() const { return m_pitch; }
    float getVolume() const { return m_volume; }
    float getHearingDistance() const { return m_hearingDistance; }
    bool isPlaying() const { return m_isPlaying; }
    bool is3D() const { return m_is3d; }
    
    // NEW: Get the sound file path for serialization
    std::string getSoundFilePath() const { return m_soundFilePath; }
    bool isLooping() const { return m_isLooping; }

    //update audio nodes
    virtual void UpdateAudioNodes(const glm::vec3& listenerPos, float listenerAngle) override;

    float GetSelectionRadius() const { return m_selectionRadius; }
    
    // ENHANCED: Unified world position calculation using hierarchy system
    glm::vec3 GetWorldPosition() const;

private:
    AudioPlayer m_audioPlayer;
    Mix_Chunk* m_sound;
    int m_channel;
    bool m_is3d;
    bool m_isPlaying;
    bool m_isLooping = false; // NEW: Track loop state

    // Additional audio properties.
    float m_pitch;           // Pitch factor (1.0 = normal)
    float m_volume;          // Volume multiplier (0.0 to 1.0)
    float m_hearingDistance; // Maximum hearing distance for attenuation

    float m_selectionRadius = 0.6f; // gizmo/picking sphere radius
    
    // NEW: Store the sound file path for serialization
    std::string m_soundFilePath;
};
