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
// An empty spec disables the path. An unrecognised spec is reported once and disables the path.
class CameraPath {
public:
    CameraPath() = default;
    explicit CameraPath(const std::string& spec);

    bool IsEnabled() const { return m_kind != Kind::None; }

    // Poses the camera for the next frame. Call once per frame, after input and game posing.
    void Apply(Camera& camera);

private:
    enum class Kind { None, Dolly, Orbit };

    Kind m_kind = Kind::None;
    glm::vec3 m_delta{ 0.0f };
    float m_frames = 1.0f;
    float m_degPerFrame = 0.0f;
    uint64_t m_frame = 0;
    bool m_captured = false;
    glm::vec3 m_startPosition{ 0.0f };
    glm::mat4 m_startRotation{ 1.0f };
    glm::vec3 m_orbitCenter{ 0.0f };
};
