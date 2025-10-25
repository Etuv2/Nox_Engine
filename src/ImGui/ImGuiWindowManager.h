#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <glm/glm.hpp>
#include <IMGUI/imgui.h>
#include <IMGUI/ImGuizmo.h>

// Forward declarations for engine types
class Camera;
class DirectionalLight;
class SceneGraph;
class SceneNode;
class Renderer;
class ModularRenderer;

// ImGui window includes
#include "BaseWindow.h"
#include "StatusWindow.h"
#include "CameraWindow.h"
#include "LightingWindow.h"
#include "SceneHierarchyWindow.h"
#include "RenderingSettingsWindow.h"
#include "PerformanceWindow.h"
#include "HelpWindow.h"

/**
* @brief Manages all ImGui windows in the NOX Engine interface
*        (Gizmo now rendered as true 3D overlay; only a lightweight panel here)
*/
class ImGuiWindowManager {
public:
    ImGuiWindowManager();
    ~ImGuiWindowManager();

    bool Initialize();
    void Shutdown();

    void Render(int windowWidth, int windowHeight, float fps,
                const std::string& sceneName, glm::vec3 lightPos, glm::vec3 lightDir);

    // New: render 3D gizmo overlay (called from ImGuiInterface after windows)
    void RenderGizmoOverlay(int windowWidth, int windowHeight);

    void ProcessKeyboardInput();

    // Data setters for windows
    void SetCamera(const std::shared_ptr<Camera>& camera);
    void SetLighting(const std::shared_ptr<DirectionalLight>& lighting);
    void SetSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph);
    void SetRenderer(const std::shared_ptr<Renderer>& renderer);
    void SetModularRenderer(const std::shared_ptr<ModularRenderer>& renderer);
    void SetSelectedNode(const std::shared_ptr<SceneNode>& node);
    void SetFrameTimeData(const std::vector<float>& data);

    // Scene management
    void SetSceneSwapCallback(const std::function<void(const std::string&)>& callback);
    void SetSceneList(const std::vector<std::string>& scenes);

    // NEW: Scene saving
    void SetSceneSaveCallback(const std::function<void()>& callback);

    // Window visibility controls
    void ToggleWindow(const std::string& windowName);
    void SetWindowVisible(const std::string& windowName, bool visible);
    bool IsWindowVisible(const std::string& windowName) const;

    // Window state persistence
    void SaveWindowStates(const std::string& filename = "imgui_layout.json");
    void LoadWindowStates(const std::string& filename = "imgui_layout.json");

    // Gizmo state access (for ImGuiInterface / MainWindow)
    std::shared_ptr<SceneNode> GetSelectedNode() const { return m_selectedNode; }
    void SetGizmoVisible(bool visible) { m_gizmoVisible = visible; }
    bool IsGizmoVisible() const { return m_gizmoVisible; }
    void SetGizmoOperation(int operation) { m_gizmoOperation = operation; }
    int GetGizmoOperation() const { return m_gizmoOperation; }
    void SetGizmoMode(int mode) { m_gizmoMode = mode; }
    int GetGizmoMode() const { return m_gizmoMode; }

    // Snap settings access
    bool IsSnapEnabled() const { return m_snapEnabled; }
    float GetTranslateSnap() const { return m_translateSnap; }
    float GetRotateSnap() const { return m_rotateSnap; }
    float GetScaleSnap() const { return m_scaleSnap; }

private:
    // Internal lightweight gizmo control panel
    void RenderGizmoPanel();

    // Window instances
    std::unique_ptr<StatusWindow> m_statusWindow;
    std::unique_ptr<CameraWindow> m_cameraWindow;
    std::unique_ptr<LightingWindow> m_lightingWindow;
    std::unique_ptr<SceneHierarchyWindow> m_sceneHierarchyWindow;
    std::unique_ptr<RenderingSettingsWindow> m_renderingSettingsWindow;
    std::unique_ptr<PerformanceWindow> m_performanceWindow;
    std::unique_ptr<HelpWindow> m_helpWindow;

    // Window lookup map (gizmo removed)
    std::unordered_map<std::string, BaseWindow*> m_windowMap{
        {"Status", nullptr},
        {"Camera", nullptr},
        {"Lighting", nullptr},
        {"Hierarchy", nullptr},
        {"Rendering", nullptr},
        {"Performance", nullptr},
        {"Help", nullptr}
    };

    // Data references for windows
    std::shared_ptr<Camera> m_camera;
    std::shared_ptr<DirectionalLight> m_lighting;
    std::shared_ptr<SceneGraph> m_sceneGraph;
    std::shared_ptr<Renderer> m_renderer;
    std::shared_ptr<SceneNode> m_selectedNode;

    // Gizmo state (logic only – rendering done in MainWindow)
    bool m_gizmoVisible = true;
    bool m_showGizmoPanel = true; // toggled with F5 now
    int  m_gizmoOperation = ImGuizmo::TRANSLATE;
    int  m_gizmoMode = ImGuizmo::LOCAL;
    bool m_snapEnabled = false;
    float m_translateSnap = 0.5f;
    float m_rotateSnap = 15.0f;
    float m_scaleSnap = 0.1f;

    // Scene management
    std::function<void(const std::string&)> m_sceneSwapCallback;
    std::function<void()> m_sceneSaveCallback; // NEW: Scene save callback

    // Cached dimensions
    int m_windowWidth = 1920;
    int m_windowHeight = 1080;
};