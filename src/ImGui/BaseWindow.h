#pragma once

#include "IMGUI/imgui.h"
#include <memory>
#include <string>

// Forward declarations
class Camera;
class DirectionalLight;
class SceneGraph;
class SceneNode;
class Renderer;
class LightManager;

/**
 * @brief Base class for all ImGui windows
 * 
 * Provides common functionality for window management, positioning, and state handling.
 */
class BaseWindow {
public:
    BaseWindow(const std::string& name, const std::string& keyBinding = "");
    virtual ~BaseWindow() = default;

    // Core interface that all windows must implement
    virtual void Render() = 0;
    
    // Window management
    virtual void SetPosition(const ImVec2& pos) { m_position = pos; }
    virtual void SetSize(const ImVec2& size) { m_size = size; }
    virtual ImVec2 GetPosition() const { return m_position; }
    virtual ImVec2 GetSize() const { return m_size; }
    
    // Visibility control
    bool IsVisible() const { return m_visible; }
    void SetVisible(bool visible) { m_visible = visible; }
    void ToggleVisible() { m_visible = !m_visible; }
    
    // Window properties
    const std::string& GetName() const { return m_name; }
    const std::string& GetKeyBinding() const { return m_keyBinding; }
    ImGuiWindowFlags GetFlags() const { return m_flags; }
    
    // Shared data setters (implemented by base class)
    virtual void SetCamera(const std::shared_ptr<Camera>& camera) { m_camera = camera; }
    virtual void SetLighting(const std::shared_ptr<DirectionalLight>& lighting) { m_lighting = lighting; }
    virtual void SetSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph) { m_sceneGraph = sceneGraph; }
    virtual void SetRenderer(const std::shared_ptr<Renderer>& renderer) { m_renderer = renderer; }
    virtual void SetSelectedNode(const std::shared_ptr<SceneNode>& node) { m_selectedNode = node; }

protected:
    // Helper method for setting up window
    void BeginWindow();
    void EndWindow();
    
    // Window properties
    std::string m_name;
    std::string m_keyBinding;
    bool m_visible = true;
    ImVec2 m_position = ImVec2(0, 0);
    ImVec2 m_size = ImVec2(300, 200);
    ImGuiWindowFlags m_flags = ImGuiWindowFlags_NoCollapse;
    bool m_firstFrame = true;
    
    // Shared data (available to all windows)
    std::shared_ptr<Camera> m_camera;
    std::shared_ptr<DirectionalLight> m_lighting;
    std::shared_ptr<SceneGraph> m_sceneGraph;
    std::shared_ptr<SceneNode> m_selectedNode;
    std::shared_ptr<Renderer> m_renderer;
};