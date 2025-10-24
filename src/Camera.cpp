// Camera.cpp
#include "Camera.h"
#include <glm/gtc/matrix_inverse.hpp>

// 8-arg ctor
Camera::Camera(glm::vec3 position, glm::vec3 worldUp,
    float yaw, float pitch,
    float fov, float aspect,
    float nearPlane, float farPlane)
    : m_worldUp(worldUp),
    m_yaw(yaw), m_pitch(pitch),
    m_fov(fov), m_aspect(aspect),
    m_nearPlane(nearPlane), m_farPlane(farPlane)
{
    // default ortho box
    m_left = -1; m_right = 1;
    m_bottom = -1; m_top = 1;
    SetPosition(position);
    updateCameraVectors();
}

// 9-arg legacy ctor
Camera::Camera(glm::vec3 position, glm::vec3 worldUp,
    float yaw, float pitch,
    float fov, float nearPlane, float farPlane,
    float movementSpeed, float mouseSensitivity)
    : Camera(position, worldUp, yaw, pitch, fov, /*aspect=*/1.0f, nearPlane, farPlane)
{
    m_movementSpeed = movementSpeed;
    m_mouseSensitivity = mouseSensitivity;
}

void Camera::SetPerspective(float fov, float aspect, float nearPlane, float farPlane) {
    m_projType = ProjectionType::Perspective;
    m_fov = fov;
    m_aspect = aspect;
    m_nearPlane = nearPlane;
    m_farPlane = farPlane;
}

void Camera::SetOrthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane) {
    m_projType = ProjectionType::Orthographic;
    m_left = left;
    m_right = right;
    m_bottom = bottom;
    m_top = top;
    m_nearPlane = nearPlane;
    m_farPlane = farPlane;
}

void Camera::SetProjectionType(ProjectionType type) {
    m_projType = type;
}

void Camera::ProcessKeyboard(const Uint8* ks, float dt) {
    float v = m_movementSpeed * dt;
    glm::vec3 pos = glm::vec3(transform[3]);
    if (ks[SDL_SCANCODE_W]) pos += m_front * v;
    if (ks[SDL_SCANCODE_S]) pos -= m_front * v;
    if (ks[SDL_SCANCODE_A]) pos -= m_rightVec * v;
    if (ks[SDL_SCANCODE_D]) pos += m_rightVec * v;
    SetPosition(pos);
    updateCameraVectors();
}

void Camera::ProcessMouseMovement(float xoff, float yoff, bool constrainPitch) {
    xoff *= m_mouseSensitivity;
    yoff *= m_mouseSensitivity;
    m_yaw += xoff;
    m_pitch += yoff;
    if (constrainPitch) {
        m_pitch = glm::clamp(m_pitch, -89.0f, 89.0f);
    }
    updateCameraVectors();
}

// Legacy two-arg overload
void Camera::ProcessMouseScroll(float yoff, float /*maxFov*/) {
    ProcessMouseScroll(yoff);
}
void Camera::ProcessMouseScroll(float yoff) {
    if (m_projType == ProjectionType::Perspective) {
        m_fov -= yoff;
        m_fov = glm::clamp(m_fov, 1.0f, 120.0f);
    }
}

glm::mat4 Camera::GetViewMatrix() const {
    return glm::inverse(transform);
}

glm::mat4 Camera::GetProjectionMatrix() const {
    if (m_projType == ProjectionType::Perspective) {
        return glm::perspective(glm::radians(m_fov), m_aspect, m_nearPlane, m_farPlane);
    }
    else {
        return glm::ortho(m_left, m_right, m_bottom, m_top, m_nearPlane, m_farPlane);
    }
}

std::pair<glm::mat4, glm::mat4> Camera::GetViewProjectionMatrix() const {
    return { GetViewMatrix(), GetProjectionMatrix() };
}

std::pair<glm::mat4, glm::mat4> Camera::GetUpdatedViewProjectionMatrix(float aspect, float nearPlane, float farPlane) const {
    glm::mat4 view = GetViewMatrix();
    glm::mat4 proj = glm::perspective(glm::radians(m_fov), aspect, nearPlane, farPlane);
    return { view, proj };
}

glm::vec3 Camera::GetCameraPosition()      const { return glm::vec3(transform[3]); }
float     Camera::GetCameraFacingAngle()   const { return m_yaw; }
glm::vec3 Camera::GetCameraUpVector()      const { return m_upVec; }
glm::vec3 Camera::GetCameraFrontVector()   const { return m_front; }
glm::vec3 Camera::GetCameraRightVector()   const { return m_rightVec; }
float     Camera::GetCameraFov()           const { return m_fov; }
float     Camera::GetCameraNearPlane()     const { return m_nearPlane; }
float     Camera::GetCameraFarPlane()      const { return m_farPlane; }
float     Camera::GetCameraMovementSpeed() const { return m_movementSpeed; }
float     Camera::GetCameraMouseSensitivity() const { return m_mouseSensitivity; }

void Camera::updateCameraVectors() {
    glm::vec3 f;
    f.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    f.y = sin(glm::radians(m_pitch));
    f.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    m_front = glm::normalize(f);
    m_rightVec = glm::normalize(glm::cross(m_front, m_worldUp));
    m_upVec = glm::normalize(glm::cross(m_rightVec, m_front));

    // write into SceneNode::transform
    transform[0] = glm::vec4(m_rightVec, 0.0f);
    transform[1] = glm::vec4(m_upVec, 0.0f);
    transform[2] = glm::vec4(-m_front, 0.0f);
    // transform[3] already holds position
}
