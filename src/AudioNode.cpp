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

	if (!m_audioPlayer.init()) {
		std::cerr << "AudioNode: Failed to initialize audio system.\n";
	}
}

AudioNode::~AudioNode()
{
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

	// Use the proper hierarchy system to get world position
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

// Use cached world position from UpdateTransformSystems
void AudioNode::updateAudio(const glm::vec3& listenerPos, float listenerAngle)
{
    if (!m_isPlaying || m_channel == -1)
        return;

    // Sanitize input parameters
    glm::vec3 safeListenerPos = SanitizeVector(listenerPos);
    float safeListenerAngle = SanitizeAngle(listenerAngle);

    // Use cached world position from traversal if available
    glm::vec3 worldPos;
    if (m_worldPositionValid) {
        worldPos = m_cachedWorldPosition;
    } else {
        // Fallback: calculate world position
        glm::mat4 parentWorld(1.0f);
        if (auto parent = parentNode.lock()) {
            parentWorld = parent->GetGlobalTransform(glm::mat4(1.0f));
        }
        glm::mat4 worldTransform = GetGlobalTransform(parentWorld);
        worldPos = glm::vec3(worldTransform[3]);
    }
    
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
	UpdateAudioNodesWithTransform(listenerPos, listenerAngle, glm::mat4(1.0f));
}

void AudioNode::UpdateAudioNodesWithTransform(const glm::vec3& listenerPos, float listenerAngle, const glm::mat4& parentWorldTransform)
{
	glm::vec3 safeListenerPos = SanitizeVector(listenerPos);
	float safeListenerAngle = SanitizeAngle(listenerAngle);

	glm::mat4 worldTransform = GetGlobalTransform(parentWorldTransform);
	UpdateTransformSystems(worldTransform);
	updateAudio(safeListenerPos, safeListenerAngle);

	for (auto& child : children) {
		if (child) {
			child->UpdateAudioNodesWithTransform(safeListenerPos, safeListenerAngle, worldTransform);
		}
	}
}

// Updated to use proper hierarchy context propagation
glm::vec3 AudioNode::GetWorldPosition() const
{
	// Get parent's world transform if we have a parent
	glm::mat4 parentWorld(1.0f);
	if (auto parent = parentNode.lock()) {
		// Use the parent's already-calculated world transform during traversal
		parentWorld = parent->GetGlobalTransform(glm::mat4(1.0f));
	}
	
	// Get our global transform using the proper hierarchy method with parent context
	glm::mat4 worldTransform = GetGlobalTransform(parentWorld);
	
	return glm::vec3(worldTransform[3]);
}

// Override UpdateTransformSystems to keep cached world position updated during traversal
void AudioNode::UpdateTransformSystems(const glm::mat4& worldTransform) {
    // Cache world position for use in updateAudio
    // This is called during UpdateAudioNodesWithTransform traversal
    m_cachedWorldPosition = glm::vec3(worldTransform[3]);
    m_worldPositionValid = true;
}

// Additional setters with validation
void AudioNode::setPitch(float pitch) {
	if (std::isfinite(pitch) && pitch > 0.0f) {
		m_pitch = std::clamp(pitch, 0.1f, 10.0f);
	}
	else {
		std::cerr << "[AudioNode] Warning: Invalid pitch value, keeping current: " << m_pitch << std::endl;
	}
}

void AudioNode::setVolume(float volume) {
	if (std::isfinite(volume)) {
		m_volume = std::clamp(volume, 0.0f, 1.0f);
	}
	else {
		std::cerr << "[AudioNode] Warning: Invalid volume value, keeping current: " << m_volume << std::endl;
	}
}

void AudioNode::setHearingDistance(float distance) {
	if (std::isfinite(distance) && distance > 0.0f) {
		m_hearingDistance = std::max(distance, 1.0f);
	}
	else {
		std::cerr << "[AudioNode] Warning: Invalid hearing distance, keeping current: " << m_hearingDistance << std::endl;
	}
}
