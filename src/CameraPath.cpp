#include "CameraPath.h"

#include "Camera.h"
#include "SceneFrameClock.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>

namespace {
constexpr float kOrbitCenterDistance = 5.0f;
}

CameraPath::CameraPath(const std::string& spec)
{
    if (spec.empty()) {
        return;
    }

    const size_t colon = spec.find(':');
    const std::string kind = spec.substr(0, colon);
    const std::string args = colon == std::string::npos ? std::string() : spec.substr(colon + 1);

    if (kind == "dolly") {
        float dx = 0.0f;
        float dy = 0.0f;
        float dz = 0.0f;
        float frames = 0.0f;
        if (std::sscanf(args.c_str(), "%f,%f,%f:%f", &dx, &dy, &dz, &frames) == 4 && frames >= 1.0f) {
            m_kind = Kind::Dolly;
            m_delta = glm::vec3(dx, dy, dz);
            m_frames = frames;
            std::cout << "[CameraPath] dolly delta=(" << dx << "," << dy << "," << dz
                      << ") frames=" << frames << std::endl;
            return;
        }
    }
    else if (kind == "orbit") {
        float degPerFrame = 0.0f;
        if (std::sscanf(args.c_str(), "%f", &degPerFrame) == 1) {
            m_kind = Kind::Orbit;
            m_degPerFrame = degPerFrame;
            std::cout << "[CameraPath] orbit degPerFrame=" << degPerFrame << std::endl;
            return;
        }
    }
    else if (kind == "turn") {
        float deg = 0.0f;
        float frames = 0.0f;
        std::istringstream turnArgs(args);
        char separator = 0;
        if ((turnArgs >> deg >> separator >> frames) && separator == ':' && frames >= 1.0f) {
            m_kind = Kind::Turn;
            m_turnDegrees = deg;
            m_frames = frames;
            std::cout << "[CameraPath] turn deg=" << deg << " frames=" << frames << std::endl;
            return;
        }
    }

    std::cerr << "[CameraPath] Unrecognised NOX_CAMERA_PATH '" << spec
              << "' (expected dolly:dx,dy,dz:frames or orbit:degPerFrame)" << std::endl;
}

void CameraPath::Apply(Camera& camera)
{
    if (!IsEnabled() || !nox::SceneFrameClock::live || nox::SceneFrameClock::scene == 0) {
        return;
    }
    const uint64_t frame = nox::SceneFrameClock::scene - 1;  // 0 on the first live scene frame

    if (!m_captured) {
        const glm::mat4 world = glm::inverse(camera.GetViewMatrix());
        m_startPosition = glm::vec3(world[3]);
        m_startRotation = glm::mat4(glm::mat3(world));
        // Camera looks down its local -Z, so the forward direction is -column 2.
        m_orbitCenter = m_startPosition - glm::vec3(world[2]) * kOrbitCenterDistance;
        m_captured = true;
    }

    if (m_kind == Kind::Dolly) {
        const float t = std::min(static_cast<float>(frame) / m_frames, 1.0f);
        glm::mat4 world = m_startRotation;
        world[3] = glm::vec4(m_startPosition + m_delta * t, 1.0f);
        camera.SetWorldTransform(world);
    }
    else if (m_kind == Kind::Turn) {
        const float t = std::min(static_cast<float>(frame) / m_frames, 1.0f);
        const glm::mat4 yaw = glm::rotate(glm::mat4(1.0f), glm::radians(m_turnDegrees * t), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 world = glm::translate(glm::mat4(1.0f), m_startPosition) * yaw * m_startRotation;
        camera.SetWorldTransform(world);
    }
    else if (m_kind == Kind::Orbit) {
        const float angle = glm::radians(m_degPerFrame * static_cast<float>(frame));
        const glm::vec3 offset = m_startPosition - m_orbitCenter;
        const glm::vec3 rotated = glm::vec3(glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::vec4(offset, 0.0f));
        const glm::vec3 position = m_orbitCenter + rotated;
        const glm::mat4 view = glm::lookAt(position, m_orbitCenter, glm::vec3(0.0f, 1.0f, 0.0f));
        camera.SetWorldTransform(glm::inverse(view));
    }

}
