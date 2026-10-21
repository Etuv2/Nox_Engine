// AudioNode.cpp
#include "AudioNode.h"
#include "audio/AudioMixer.h"
#include "audio/AudioPlayback.h"
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
	m_soundFilePath = soundFile;
	stop();
	if (m_sound) {
		Mix_FreeChunk(m_sound);
		m_sound = nullptr;
	}
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
	m_isLooping = loop;

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
		outputGain(),                      // source volume * bus gain, in [0,1]
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

void AudioNode::pause()
{
	if (m_channel != -1 && m_isPlaying) {
		m_audioPlayer.pauseSound(m_channel);
		m_isPlaying = false;
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

    // Default curve: the original linear falloff over the hearing distance, unchanged.
    if (m_attenuation == AudioAttenuation::Linear && m_minDistance <= 0.0f && !m_occlusion) {
        m_audioPlayer.setChannel3DPosition(
            m_channel,
            worldPos,
            safeListenerPos,
            safeListenerAngle,
            std::max(m_hearingDistance, 1.0f), // Ensure positive hearing distance
            outputGain());                     // source volume * bus gain, in [0,1]
        return;
    }

    // Other curves and occlusion: gain = source * bus * curve * occlusion, panned as before.
    const float distance = glm::length(worldPos - safeListenerPos);
    const float attenuation = AudioAttenuationGain(distance, spatialParams());
    const float occlusion = AudioUpdateOcclusion(m_occlusionState, m_occlusion, safeListenerPos, worldPos,
        AudioGetRayTest(), AudioPlaybackFrameDt());
    m_audioPlayer.setChannel3DGain(m_channel, worldPos, safeListenerPos, safeListenerAngle,
        outputGain() * attenuation * occlusion);
}

void AudioNode::setMinDistance(float d) {
    m_minDistance = std::isfinite(d) ? std::max(d, 0.0f) : 0.0f;
}

void AudioNode::setRolloff(float r) {
    m_rolloff = std::isfinite(r) ? std::clamp(r, 0.01f, 16.0f) : 1.0f;
}

AudioSpatialParams AudioNode::spatialParams() const {
    AudioSpatialParams p;
    p.curve = m_attenuation;
    p.minDistance = m_minDistance;
    p.maxDistance = std::max(m_hearingDistance, 1.0f);
    p.rolloff = m_rolloff;
    return p;
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

void AudioNode::setBus(const std::string& bus) {
	m_bus = bus.empty() ? std::string(AudioMixer::kDefaultSourceBus) : bus;
}

float AudioNode::outputGain() const {
	const float volume = std::isfinite(m_volume) ? std::clamp(m_volume, 0.0f, 1.0f) : 0.0f;
	return std::clamp(volume * AudioMixer::Get().BusGain(m_bus), 0.0f, 1.0f);
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
