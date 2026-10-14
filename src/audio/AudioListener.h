#pragma once
// Listener selection for 3D audio (engine service, game-agnostic).
//
// 3D sources are heard from the active listener: the first node (in scene traversal order) that carries an
// enabled AudioListener component with current = true, or the scene camera when there is none. The pose uses the
// camera's convention: angleDegrees is the yaw of the forward direction in the XZ plane, atan2(forward.z, forward.x),
// so a listener on a camera node reproduces the camera's own pose exactly.
//
// Core publishes the active pose once per frame (AudioListenerPublish); code that is not a node (Lua one-shots)
// reads it through AudioListenerCurrent().
#include <glm/glm.hpp>
#include <cstddef>
#include <vector>

struct AudioListenerPose {
    glm::vec3 position{0.0f};
    float angleDegrees = 0.0f;
};

// Pose of a node from its world transform. Forward is the node's local -Z axis (the view convention).
// A non-finite transform yields the default pose.
AudioListenerPose AudioListenerPoseFromWorld(const glm::mat4& world);

// Index of the first candidate flagged current, or -1 when none is.
int AudioListenerSelect(const std::vector<bool>& current);

// Active pose used by one-shot playback; defaults to the origin until Core publishes one.
void AudioListenerPublish(const AudioListenerPose& pose);
AudioListenerPose AudioListenerCurrent();
