#include "CameraWindow.h"
#include "../Camera.h"
#include <glm/gtc/type_ptr.hpp>

CameraWindow::CameraWindow()
    : BaseWindow("Camera Controls", "F2")
{
    m_position = ImVec2(10, 200);
    m_size = ImVec2(300, 180);
}

void CameraWindow::Render() {
    BeginWindow();
    if (!IsVisible()) return;

    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Camera Settings");
    ImGui::Separator();
    
    if (m_camera) {
        // Camera position and orientation info
        glm::vec3 pos = m_camera->GetCameraPosition();
        ImGui::Text("Position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
        
        float fov = m_camera->GetCameraFov();
        ImGui::Text("FOV: %.1f°", fov);
        
        ImGui::Separator();
        
        // Camera speed controls
        float speed = m_camera->GetCameraMovementSpeed();
        if (ImGui::SliderFloat("Movement Speed", &speed, 0.1f, 50.0f, "%.2f")) {
            m_camera->m_movementSpeed = speed;
        }
        
        // Mouse sensitivity
        float sensitivity = m_camera->GetCameraMouseSensitivity();
        if (ImGui::SliderFloat("Mouse Sensitivity", &sensitivity, 0.01f, 2.0f, "%.3f")) {
            m_camera->m_mouseSensitivity = sensitivity;
        }
        
        ImGui::Separator();
        
        // Camera projection settings
        ImGui::Text("Projection Settings:");
        float nearPlane = m_camera->GetCameraNearPlane();
        float farPlane = m_camera->GetCameraFarPlane();
        
        if (ImGui::DragFloat("Near Plane", &nearPlane, 0.01f, 0.01f, 10.0f, "%.3f")) {
            // Update camera near plane if method exists
            ImGui::Text("Near plane: %.3f", nearPlane);
        }
        
        if (ImGui::DragFloat("Far Plane", &farPlane, 1.0f, 10.0f, 10000.0f, "%.1f")) {
            // Update camera far plane if method exists
            ImGui::Text("Far plane: %.1f", farPlane);
        }
        
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Controls:");
        ImGui::BulletText("W/A/S/D: Move camera");
        ImGui::BulletText("Mouse: Look around");
        ImGui::BulletText("Scroll: Zoom in/out");
        ImGui::BulletText("Shift+Click: Toggle mouse lock");
        
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No camera available");
        ImGui::Separator();
        ImGui::Text("Camera controls will appear here when");
        ImGui::Text("a camera is loaded into the scene.");
        ImGui::Separator();
        ImGui::Text("Expected controls:");
        ImGui::BulletText("Movement speed adjustment");
        ImGui::BulletText("Mouse sensitivity tuning");
        ImGui::BulletText("Field of view control");
        ImGui::BulletText("Near/far plane settings");
    }
    
    EndWindow();
}