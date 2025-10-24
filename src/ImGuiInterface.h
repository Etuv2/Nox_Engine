#pragma once
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <glm/glm.hpp>
#include "json.hpp"
#include "IMGUI/imgui.h"

// Forward declarations
class Camera;
class DirectionalLight;
class SceneGraph;
class SceneNode;
class Renderer;
class ModularRenderer;
class ImGuiWindowManager;

/**
 * @brief Main ImGui interface class for the NOX Engine
 * 
 * This class now acts as a facade over the modular window system,
 * providing backward compatibility while delegating to ImGuiWindowManager.
 */
class ImGuiInterface {
public:
    ImGuiInterface();
    ~ImGuiInterface();

    // Set smart pointers to objects so that ImGui can modify them.
    void SetCamera(const std::shared_ptr<Camera>& camera);
    void SetLighting(const std::shared_ptr<DirectionalLight>& lighting);
    void SetSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph);
    void SetRenderer(const std::shared_ptr<Renderer>& renderer);
    void SetModularRenderer(const std::shared_ptr<ModularRenderer>& renderer);

    void SetFrameData(const std::vector<float>& frameData);

    // Render the ImGui interface.
    // windowWidth and windowHeight are used to position the windows.
    // fps is displayed in the status panel.
    void Render(int windowWidth, int windowHeight, float fps, std::string sceneName, glm::vec3 lightPos, glm::vec3 lightDir);

    // Render the 3D gizmo overlay (called from main application)
    void RenderGizmoOverlay(int windowWidth, int windowHeight);

    // Process keyboard input for window toggles and gizmo controls
    void ProcessKeyboardInput();

    // Gizmo functionality (delegates to window manager)
    void SetSelectedNode(const std::shared_ptr<SceneNode>& node);
    std::shared_ptr<SceneNode> GetSelectedNode() const;
    void SetGizmoVisible(bool visible);
    bool IsGizmoVisible() const;
    void SetGizmoOperation(int operation);
    int GetGizmoOperation() const;
    void SetGizmoMode(int mode);
    int GetGizmoMode() const;

    // Snap settings access (for MainWindow gizmo rendering)
    bool IsSnapEnabled() const;
    float GetTranslateSnap() const;
    float GetRotateSnap() const;
    float GetScaleSnap() const;

    // Set the scene swap callback.
    // When the user clicks "Load Scene", the callback will be invoked with the selected scene file path.
    void SetSceneSwapCallback(const std::function<void(const std::string&)>& callback);

    // Set the list of available scene file paths.
    void SetSceneList(const std::vector<std::string>& scenes);

    // Window visibility controls (for backward compatibility)
    void ToggleWindow(const std::string& windowName);
    void SetWindowVisible(const std::string& windowName, bool visible);
    bool IsWindowVisible(const std::string& windowName) const;

private:
    std::unique_ptr<ImGuiWindowManager> m_windowManager;
};
