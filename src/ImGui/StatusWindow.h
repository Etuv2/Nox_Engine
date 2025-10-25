#pragma once

#include "BaseWindow.h"
#include <glm/glm.hpp>
#include <functional>
#include <vector>
#include <string>

/**
 * @brief Engine status and information display window
 */
class StatusWindow : public BaseWindow {
public:
    StatusWindow();

    void Render() override;

    // Status data setters
    void SetFPS(float fps) { m_fps = fps; }
    void SetSceneName(const std::string& name) { m_sceneName = name; }
    void SetLightPosition(const glm::vec3& pos) { m_lightPosition = pos; }
    void SetLightDirection(const glm::vec3& dir) { m_lightDirection = dir; }
    void SetWindowDimensions(int width, int height) { m_windowWidth = width; m_windowHeight = height; }
    void SetFrameTimeData(const std::vector<float>& data) { m_frameTimeData = data; }

    // Scene management
    void SetSceneList(const std::vector<std::string>& scenes) { m_sceneList = scenes; }
    void SetSceneSwapCallback(const std::function<void(const std::string&)>& callback) { m_sceneSwapCallback = callback; }
    void SetSceneSaveCallback(const std::function<void()>& callback) { m_sceneSaveCallback = callback; } // NEW: Scene saving

    // Window controls
    void SetWindowToggleCallback(const std::function<void(const std::string&)>& callback) { m_windowToggleCallback = callback; }

private:
    float m_fps = 0.0f;
    std::string m_sceneName = "No Scene";
    glm::vec3 m_lightPosition = glm::vec3(0.0f);
    glm::vec3 m_lightDirection = glm::vec3(0.0f, -1.0f, 0.0f);
    int m_windowWidth = 0;
    int m_windowHeight = 0;
    std::vector<float> m_frameTimeData;

    std::vector<std::string> m_sceneList;
    std::function<void(const std::string&)> m_sceneSwapCallback;
    std::function<void()> m_sceneSaveCallback; // NEW: Save callback
    std::function<void(const std::string&)> m_windowToggleCallback;
};