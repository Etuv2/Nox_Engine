// AudioNode.h
#pragma once
#include "SceneNode.h"
#include "AudioPlayer.h"
#include "audio/AudioSpatial.h"
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

    // Pause the current channel. play() starts it again from the beginning.
    void pause();

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
    // Mixer bus the source plays through (AudioMixer). Defaults to "sfx"; an empty name resets to the default.
    const std::string& getBus() const { return m_bus; }
    void setBus(const std::string& bus);

    // Distance attenuation. hearing_distance is the maximum distance (silent at and beyond it). The default curve is
    // the original linear falloff, which keeps existing sources sounding as they always did. minDistance is the
    // distance inside which the source is at full volume (Inverse: the reference distance).
    AudioAttenuation getAttenuation() const { return m_attenuation; }
    void setAttenuation(AudioAttenuation curve) { m_attenuation = curve; }
    float getMinDistance() const { return m_minDistance; }
    void setMinDistance(float d);
    float getRolloff() const { return m_rolloff; }
    void setRolloff(float r);
    // Occlusion: when a wall blocks the line from listener to source, the source is attenuated (AudioSpatial.h).
    bool getOcclusion() const { return m_occlusion; }
    void setOcclusion(bool on) { m_occlusion = on; }
    // Curve parameters for this source (maximum distance = hearing distance).
    AudioSpatialParams spatialParams() const;
    // Volume the channel is mixed at: source volume * bus gain (AudioMixer::BusGain).
    float outputGain() const;
    bool isPlaying() const { return m_isPlaying; }
    bool is3D() const { return m_is3d; }
    
    // Get the sound file path for serialization
    std::string getSoundFilePath() const { return m_soundFilePath; }
    bool hasSound() const { return m_sound != nullptr; }   // false when the file failed to load
    bool isLooping() const { return m_isLooping; }
    // Settings applied by the next play(): loop, 3D positioning, and whether a scene load starts the sound.
    void setLooping(bool loop) { m_isLooping = loop; }
    void setIs3D(bool is3d) { m_is3d = is3d; }
    bool getAutoplay() const { return m_autoplay; }
    void setAutoplay(bool autoplay) { m_autoplay = autoplay; }

    //update audio nodes
    virtual void UpdateAudioNodes(const glm::vec3& listenerPos, float listenerAngle) override;
    virtual void UpdateAudioNodesWithTransform(const glm::vec3& listenerPos, float listenerAngle, const glm::mat4& parentWorldTransform) override;

    // Override UpdateTransformSystems to sync audio position from world transform
    virtual void UpdateTransformSystems(const glm::mat4& worldTransform) override;

    float GetSelectionRadius() const { return m_selectionRadius; }
    
    // ENHANCED: Unified world position calculation using hierarchy system
    glm::vec3 GetWorldPosition() const;

private:
    AudioPlayer m_audioPlayer;
    Mix_Chunk* m_sound;
    int m_channel;
    bool m_is3d;
    bool m_isPlaying;
    bool m_isLooping = false; // Track loop state
    bool m_autoplay = true;   // Scene loading plays the sound when true

    // Additional audio properties.
    float m_pitch;           // Pitch factor (1.0 = normal)
    float m_volume;          // Volume multiplier (0.0 to 1.0)
    float m_hearingDistance; // Maximum hearing distance for attenuation
    std::string m_bus = "sfx"; // AudioMixer bus name

    AudioAttenuation m_attenuation = AudioAttenuation::Linear;
    float m_minDistance = 0.0f;
    float m_rolloff = 1.0f;
    bool m_occlusion = false;
    AudioOcclusionState m_occlusionState;

    float m_selectionRadius = 0.6f; // gizmo/picking sphere radius
    
    // Store the sound file path for serialization (the authored path, kept even when loading fails)
    std::string m_soundFilePath;
    
    // Cache world position from traversal to avoid recalculation
    mutable glm::vec3 m_cachedWorldPosition = glm::vec3(0.0f);
    mutable bool m_worldPositionValid = false;
};
