#pragma once

#include <cstdint>
#include <string>
#include <glm/glm.hpp>

class Camera;

// NOX_CAMERA_PATH: deterministic camera motion for repeatable temporal tests (popping, ghosting,
// light-change lag). The path advances by frame count, not wall time, so NOX_FIXED_DT runs repeat.
//   dolly:<dx>,<dy>,<dz>:<frames>  moves the start position linearly by (dx,dy,dz) in world space over
//                                  <frames> frames, then holds. The start orientation is kept.
//   orbit:<degPerFrame>            turns about world up around a point 5 m ahead of the start pose.
//   turn:<deg>:<frames>            yaws about world up by <deg> over <frames> frames at the start position, then holds
//                                  (plan section 22: a turn that reveals new surfels).
// An empty spec disables the path. An unrecognised spec is reported once and disables the path.
class CameraPath {
public:
    CameraPath() = default;
    explicit CameraPath(const std::string& spec);

    bool IsEnabled() const { return m_kind != Kind::None; }

    // Poses the camera for the current live scene frame (nox::SceneFrameClock): frame 0 is the first live frame of
    // the scene, so loading and loading-overlay frames never advance the path. Does nothing on other frames.
    // Call once per update, after input and game posing.
    void Apply(Camera& camera);

private:
    enum class Kind { None, Dolly, Orbit, Turn };

    Kind m_kind = Kind::None;
    glm::vec3 m_delta{ 0.0f };
    float m_frames = 1.0f;
    float m_degPerFrame = 0.0f;
    float m_turnDegrees = 0.0f;
    bool m_captured = false;
    glm::vec3 m_startPosition{ 0.0f };
    glm::mat4 m_startRotation{ 1.0f };
    glm::vec3 m_orbitCenter{ 0.0f };
};
