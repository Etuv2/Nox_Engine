// AudioNode.cpp
#include "AudioNode.h"
#include "SDL/SDL_mixer.h"
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <cmath>
#include <algorithm>

AudioNode::AudioNode()
    : m_sound(nullptr), m_channel(-1), m_is3d(true), m_isPlaying(false),
    m_pitch(1.0f), m_volume(1.0f), m_hearingDistance(1000.0f)
{
    // Initialize the audio system only once, or remove entirely if you do global init.
    // If each AudioNode does this, you risk multiple inits. Typically you'd do this in your engine startup.
    if (!m_audioPlayer.init()) {
        std::cerr << "AudioNode: Failed to initialize audio system.\n";
    }
}

AudioNode::~AudioNode()
{
    // Do NOT call m_audioPlayer.shutdown() here, or we double-close Mix audio if multiple nodes exist.
    // Just free our own sound chunk.
    if (m_sound) {
        Mix_FreeChunk(m_sound);
        m_sound = nullptr;
    }
}

bool AudioNode::initAudio(const std::string& soundFile)
{
    m_sound = m_audioPlayer.loadSound(soundFile);
    return (m_sound != nullptr);
}

// Helper function to validate and sanitize vectors
static glm::vec3 SanitizeVector(const glm::vec3& v, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) {
        // Clamp to reasonable bounds to prevent extreme values
        const float MAX_COORD = 100000.0f;
        return glm::clamp(v, glm::vec3(-MAX_COORD), glm::vec3(MAX_COORD));
    }
    std::cerr << "[AudioNode] Warning: Invalid vector detected, using fallback: (" 
              << fallback.x << ", " << fallback.y << ", " << fallback.z << ")" << std::endl;
    return fallback;
}

// Helper function to validate angles
static float SanitizeAngle(float angle) {
    if (!std::isfinite(angle)) {
        std::cerr << "[AudioNode] Warning: Invalid angle detected, using 0.0" << std::endl;
        return 0.0f;
    }
    // Normalize to [-180, 180] range
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

void AudioNode::play(bool loop, bool is3d)
{
    m_is3d = is3d;
    
    // FIXED: Use the proper hierarchy system to get world position
    // The world position will be calculated during the next UpdateAudioNodes call
    // For now, just store the parameters and mark as playing
    glm::vec3 sourcePos = GetWorldPosition();
    sourcePos = SanitizeVector(sourcePos);
    
    glm::vec3 listenerPos = SanitizeVector(glm::vec3(0.0f)); // Default for initial
    float listenerAngle = SanitizeAngle(0.0f);  // Default listener facing

    m_channel = m_audioPlayer.playSound(
        m_sound,
        loop,
        m_is3d,
        sourcePos,
        listenerPos,
        listenerAngle,
        std::clamp(m_pitch, 0.1f, 10.0f),  // Clamp pitch to reasonable range
        std::clamp(m_volume, 0.0f, 1.0f),  // Clamp volume to [0,1]
        std::max(m_hearingDistance, 1.0f)); // Ensure positive hearing distance

    if (m_channel != -1) {
        m_isPlaying = true;
    }
    else {
        std::cerr << "AudioNode: Failed to play sound.\n";
    }
}

void AudioNode::stop()
{
    if (m_channel != -1) {
        Mix_HaltChannel(m_channel);
        m_isPlaying = false;
        m_channel = -1;
    }
}

// FIXED: Use proper hierarchy system instead of manual transform reconstruction
void AudioNode::updateAudio(const glm::vec3& listenerPos, float listenerAngle)
{
    if (!m_isPlaying || m_channel == -1)
        return;

    // Sanitize input parameters
    glm::vec3 safeListenerPos = SanitizeVector(listenerPos);
    float safeListenerAngle = SanitizeAngle(listenerAngle);

    // FIXED: Get world position using the unified hierarchy system
    glm::vec3 worldPos = GetWorldPosition();
    worldPos = SanitizeVector(worldPos, glm::vec3(0.0f, 0.0f, 0.0f));

    // Additional validation: check for reasonable distance from listener
    glm::vec3 toListener = worldPos - safeListenerPos;
    float distanceToListener = glm::length(toListener);
    
    if (!std::isfinite(distanceToListener) || distanceToListener > 1000000.0f) {
        std::cerr << "[AudioNode] Warning: Audio source too far from listener or invalid distance, skipping update" << std::endl;
        return;
    }

    m_audioPlayer.setChannel3DPosition(
        m_channel,
        worldPos,
        safeListenerPos,
        safeListenerAngle,
        std::max(m_hearingDistance, 1.0f), // Ensure positive hearing distance
        std::clamp(m_volume, 0.0f, 1.0f)); // Ensure valid volume range
}

void AudioNode::UpdateAudioNodes(const glm::vec3& listenerPos, float listenerAngle)
{
    // Validate input parameters at the top level
    glm::vec3 safeListenerPos = SanitizeVector(listenerPos);
    float safeListenerAngle = SanitizeAngle(listenerAngle);

    // Always update this node's audio based on current world position from hierarchy
    updateAudio(safeListenerPos, safeListenerAngle);
    
    // Recursively update children with sanitized parameters
    for (auto& child : children) {
        if (child) {
            child->UpdateAudioNodes(safeListenerPos, safeListenerAngle);
        }
    }
}

// FIXED: Add unified method to get world position using hierarchy system
glm::vec3 AudioNode::GetWorldPosition() const
{
    // Use the parent transform that will be passed down during traversal
    // This will be set correctly during Draw() or UpdateAudioNodes() calls
    glm::mat4 parentWorld(1.0f);
    
    // Get parent's world transform if we have a parent
    if (auto parent = parentNode.lock()) {
        // The parent should already have its world transform calculated
        // during the traversal - we just need our local transform applied to it
        parentWorld = parent->GetGlobalTransform(glm::mat4(1.0f));
    }
    
    // Get our global transform using the proper hierarchy method
    glm::mat4 worldTransform = GetGlobalTransform(parentWorld);
    
    return glm::vec3(worldTransform[3]);
}

// Additional setters with validation
void AudioNode::setPitch(float pitch) { 
    if (std::isfinite(pitch) && pitch > 0.0f) {
        m_pitch = std::clamp(pitch, 0.1f, 10.0f); 
    } else {
        std::cerr << "[AudioNode] Warning: Invalid pitch value, keeping current: " << m_pitch << std::endl;
    }
}

void AudioNode::setVolume(float volume) { 
    if (std::isfinite(volume)) {
        m_volume = std::clamp(volume, 0.0f, 1.0f);
    } else {
        std::cerr << "[AudioNode] Warning: Invalid volume value, keeping current: " << m_volume << std::endl;
    }
}

void AudioNode::setHearingDistance(float distance) { 
    if (std::isfinite(distance) && distance > 0.0f) {
        m_hearingDistance = std::max(distance, 1.0f);
    } else {
        std::cerr << "[AudioNode] Warning: Invalid hearing distance, keeping current: " << m_hearingDistance << std::endl;
    }
}
