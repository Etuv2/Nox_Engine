#pragma once

#include "InputSystem.h"
#include "InputActions.h"
#include <memory>

// Forward declarations to avoid circular includes
class Core;
class Camera;
class SceneGraph;
class ImGuiInterface;
class DirectionalLight;
class LightManager;

/**
 * @brief Integration helper for the Input System with NOX Engine
 * 
 * This class provides a bridge between the unified input system and the
 * Core engine, making it easy to migrate from hardcoded input handling 
 * to the flexible input system.
 */
class InputIntegration {
public:
    InputIntegration();
    ~InputIntegration();
    
    // Initialization
    bool Initialize(Core* core);
    void Shutdown();
    
    // Main update - should be called every frame
    void Update(float deltaTime);
    
    // Event processing - should be called with SDL events
    bool ProcessEvent(const SDL_Event& event);
    
    // Context management
    void EnableContext(const std::string& contextName, bool enabled = true);
    void DisableContext(const std::string& contextName);
    void SetMouseLocked(bool locked);
    bool IsMouseLocked() const { return m_mouseLocked; }
    
    // Component setters (called by Core during initialization)
    void SetCamera(std::shared_ptr<Camera> camera) { m_camera = camera; }
    void SetSceneGraph(std::shared_ptr<SceneGraph> sceneGraph) { m_sceneGraph = sceneGraph; }
    void SetImGuiInterface(ImGuiInterface* imguiInterface) { m_imguiInterface = imguiInterface; }
    void SetDirectionalLight(std::shared_ptr<DirectionalLight> light) { m_directionalLight = light; }
    void SetLightManager(LightManager* lightManager) { m_lightManager = lightManager; }
    
    // Sensitivity configuration
    void SetGamepadLookSensitivity(float sensitivity) { m_gamepadLookSensitivity = sensitivity; }
    float GetGamepadLookSensitivity() const { return m_gamepadLookSensitivity; }
    
    // Configuration
    bool LoadConfiguration(const std::string& configPath = "config/input_config.json");
    bool SaveConfiguration(const std::string& configPath = "config/input_config.json");
    
    // Get access to the underlying input manager
    Input::InputManager* GetInputManager() { return m_inputManager.get(); }
    
private:
    // Setup all input contexts and their callbacks
    void SetupInputContexts();
    void SetupGlobalContext();
    void SetupCameraContext();
    void SetupEditorContext();
    void SetupLightingContext();
    void SetupAnimationContext();
    void SetupGamepadContext();
    
    // Action callbacks
    void OnCameraMovement(const std::string& direction, float value);
    void OnCameraLook(float deltaX, float deltaY);
    void OnApplicationAction(const std::string& action);
    void OnGizmoAction(const std::string& action);
    void OnLightingAction(const std::string& action);
    void OnAnimationAction(const std::string& action);
    void OnObjectSelect();
    
    // Utility methods
    void UpdateMouseLockState();
    void ApplyLightPreset(int presetNumber);
    
    // Member variables
    std::unique_ptr<Input::InputManager> m_inputManager;
    Core* m_core;
    
    // Component references
    std::shared_ptr<Camera> m_camera;
    std::shared_ptr<SceneGraph> m_sceneGraph;
    ImGuiInterface* m_imguiInterface;
    std::shared_ptr<DirectionalLight> m_directionalLight;
    LightManager* m_lightManager;
    
    // State
    bool m_initialized;
    bool m_mouseLocked;
    std::string m_configPath;
    
    // Settings
    float m_cameraSpeed;
    float m_mouseSensitivity;
    float m_gamepadLookSensitivity; // Separate sensitivity for gamepad looking
};