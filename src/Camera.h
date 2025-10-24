// Camera.h
#pragma once

#include "SceneNode.h"
#include <SDL/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

class Camera : public SceneNode {
public:
    enum class ProjectionType { Perspective, Orthographic };

    // —— Constructors ——  
    // 8-arg: (pos, up, yaw, pitch, fov, aspect, near, far)
    Camera(glm::vec3 position,
        glm::vec3 worldUp,
        float     yaw,
        float     pitch,
        float     fov = 45.0f,
        float     aspect = 16.0f / 9.0f,
        float     nearPlane = 0.1f,
        float     farPlane = 100.0f);

    // 9-arg legacy overload so you don't have to rewrite your MainWindow call:
    // (pos, up, yaw, pitch, fov, near, far, moveSpeed, mouseSens)
    Camera(glm::vec3 position,
        glm::vec3 worldUp,
        float     yaw,
        float     pitch,
        float     fov,
        float     nearPlane,
        float     farPlane,
        float     movementSpeed,
        float     mouseSensitivity);

    // —— Projection Setup ——  
    void SetPerspective(float fov, float aspect, float nearPlane, float farPlane);
    void SetOrthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane);
    void SetProjectionType(ProjectionType type);

    // —— Input Controls ——  
    void ProcessKeyboard(const Uint8* keystate, float deltaTime);
    void ProcessMouseMovement(float xoffset, float yoffset, bool constrainPitch = true);
    // keep your old two‐arg scroll call
    void ProcessMouseScroll(float yoffset, float maxFov);
    // this one actually does the work
    void ProcessMouseScroll(float yoffset);

    // —— Matrix Getters ——  
    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix() const;
    std::pair<glm::mat4, glm::mat4> GetViewProjectionMatrix() const;
    // legacy signature
    std::pair<glm::mat4, glm::mat4> GetUpdatedViewProjectionMatrix(float aspect, float nearPlane, float farPlane) const;

    // —— Legacy getters ——  
    glm::vec3 GetCameraPosition()      const;
    float     GetCameraFacingAngle()   const;
    glm::vec3 GetCameraUpVector()      const;
    glm::vec3 GetCameraFrontVector()   const;
    glm::vec3 GetCameraRightVector()   const;
    float     GetCameraFov()           const;
    float     GetCameraNearPlane()     const;
    float     GetCameraFarPlane()      const;
    float     GetCameraMovementSpeed() const;
    float     GetCameraMouseSensitivity() const;
    float m_movementSpeed = 2.5f;
    float m_mouseSensitivity = 0.1f;
private:
    void updateCameraVectors();

    ProjectionType m_projType = ProjectionType::Perspective;

    // perspective params
    float m_fov, m_aspect;
    // orthographic params
    float m_left, m_right, m_bottom, m_top;
    // shared near/far
    float m_nearPlane, m_farPlane;

    // orientation
    float     m_yaw, m_pitch;
    glm::vec3 m_front, m_upVec, m_rightVec, m_worldUp;


};
