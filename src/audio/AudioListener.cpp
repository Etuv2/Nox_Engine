#include "AudioListener.h"

#include <cmath>

namespace {
AudioListenerPose g_current;
}

AudioListenerPose AudioListenerPoseFromWorld(const glm::mat4& world) {
    AudioListenerPose pose;
    const glm::vec3 position = glm::vec3(world[3]);
    const glm::vec3 forward = -glm::vec3(world[2]);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return pose;
    if (!std::isfinite(forward.x) || !std::isfinite(forward.z)) return pose;
    pose.position = position;
    if (glm::length(glm::vec2(forward.x, forward.z)) > 1e-6f) {
        pose.angleDegrees = glm::degrees(std::atan2(forward.z, forward.x));
    }
    return pose;
}

int AudioListenerSelect(const std::vector<bool>& current) {
    for (size_t i = 0; i < current.size(); ++i) {
        if (current[i]) return static_cast<int>(i);
    }
    return -1;
}

void AudioListenerPublish(const AudioListenerPose& pose) {
    g_current = pose;
}

AudioListenerPose AudioListenerCurrent() {
    return g_current;
}
