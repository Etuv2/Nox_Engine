#pragma once

#include <memory>
#include <string>
#include <vector>
#include <array>
#include <functional>

#include <SDL/SDL.h>
#include <glm/glm.hpp>

#include "json.hpp"
#include "RayCast.h"
#include "BVH.h"

// Forward declarations
class Camera;
class SceneGraph;
class SceneLoader;
class ModelManager;
class DirectionalLight;
class LightManager;
class ModularRenderer;
class ImGuiInterface;
class PhysicsEngine;
class InputIntegration;
class PerformanceRecorder;
class RuntimeStateManager;
class SceneNode;
struct SimulationConfig;

using json = nlohmann::json;

/**
 * @brief Core engine class that owns and coordinates all engine subsystems.
 * 
 * This class is the primary owner of the engine's functional runtime.
 * It handles:
 * - Subsystem initialization and shutdown
 * - Main loop coordination (update order)
 * - Scene lifecycle management
 * - Resource management
 * - Input routing
 * 
 * MainWindow drives Core through explicit method calls without needing
 * to understand internal engine details.
 */
class Core {
public:
    Core();
    ~Core();


    // Initialization and Shutdown

    
    /**
     * @brief Initialize all engine subsystems.
     * @param window SDL window handle (owned by MainWindow)
     * @param glContext OpenGL context handle (owned by MainWindow)
     * @param windowWidth Initial window width
     * @param windowHeight Initial window height
     * @return true if initialization succeeded
     */
    bool Initialize(SDL_Window* window, SDL_GLContext glContext, int windowWidth, int windowHeight);
    
    /**
     * @brief Shutdown all engine subsystems and release resources.
     */
    void Shutdown();


    // Configuration

    
    /**
     * @brief Load engine configuration from file.
     * @return true if configuration loaded successfully
     */
    bool LoadConfiguration();
    
    /**
     * @brief Get the loaded configuration.
     */
    const json& GetConfig() const { return m_config; }


    // Main Loop Coordination

    
    /**
     * @brief Update all engine subsystems for one frame.
     * @param deltaTime Time elapsed since last frame in seconds
     */
    void Update(float deltaTime);
    
    /**
     * @brief Render the current frame.
     * @param windowWidth Current window width
     * @param windowHeight Current window height
     */
    void Render(int windowWidth, int windowHeight);


    // Event Handling

    
    /**
     * @brief Process an SDL event through the input system.
     * @param event The SDL event to process
     * @return true if the event was handled by the input system
     */
    bool ProcessInputEvent(const SDL_Event& event);
    
    /**
     * @brief Handle mouse click for object picking.
     * @param mouseX Mouse X coordinate
     * @param mouseY Mouse Y coordinate
     * @param windowWidth Current window width
     * @param windowHeight Current window height
     */
    void HandleMouseClick(int mouseX, int mouseY, int windowWidth, int windowHeight);
    
    /**
     * @brief Handle mouse scroll for camera zoom.
     * @param scrollY Scroll delta
     */
    void HandleMouseScroll(float scrollY);


    // Window Events (called by MainWindow)

    
    /**
     * @brief Handle window resize.
     * @param newWidth New window width
     * @param newHeight New window height
     */
    void OnWindowResize(int newWidth, int newHeight);


    // Scene Management

    
    /**
     * @brief Swap to a new scene.
     * @param newSceneFile Path to the new scene file
     */
    void SwapScene(const std::string& newSceneFile);
    
    /**
     * @brief Save the current scene.
     */
    void SaveCurrentScene();
    
    /**
     * @brief Get the current scene name.
     */
    const std::string& GetSceneName() const { return m_sceneName; }


    // Physics Control

    
    /**
     * @brief Request physics toggle (deferred to next update).
     */
    void RequestPhysicsToggle() { m_requestTogglePhysics = true; }
    
    /**
     * @brief Check if physics is enabled for the current scene.
     */
    bool IsPhysicsEnabled() const { return m_physicsEnabledForScene; }


    // Input State Queries

    
    /**
     * @brief Check if mouse is locked for camera control.
     */
    bool IsMouseLocked() const;
    
    /**
     * @brief Set mouse lock state.
     */
    void SetMouseLocked(bool locked);


    // State Export and Performance Recording

    
    bool SaveSceneState(const std::string& filepath);
    bool LoadSceneState(const std::string& filepath);
    bool QuickSave();
    bool ExportPerformanceCSV(const std::string& filepath);
    bool ExportPerformanceJSON(const std::string& filepath);
    void StartPerformanceRecording();
    void StopPerformanceRecording();


    // Accessors for UI Integration

    
    std::shared_ptr<Camera> GetCamera() { return m_camera; }
    std::shared_ptr<SceneGraph> GetSceneGraph() { return m_sceneGraph; }
    std::shared_ptr<DirectionalLight> GetLighting() { return m_lighting; }
    LightManager* GetLightManager();
    ImGuiInterface* GetImGuiInterface() { return m_imguiInterface.get(); }
    InputIntegration* GetInputIntegration() { return m_inputIntegration.get(); }
    float GetFPS() const { return m_fps; }
    
    // Get frame time data for UI (returns pointer to circular buffer and count)
    const float* GetFrameTimeData(size_t& outCount) const { 
        outCount = m_frameTimeCount; 
        return m_frameTimeBuffer.data(); 
    }
    
    // Lighting configuration
    glm::vec3 GetLightPosition() const { return m_lightPos; }
    glm::vec3 GetLightDirection() const { return m_lightDir; }

private:

    // Internal Initialization

    
    bool InitializeGraphics();
    bool InitializeLighting();
    bool InitializeCamera();
    bool InitializeScene();
    bool InitializePhysics();
    bool InitializeUI();
    bool InitializeInput();
    

    // Internal Scene Management

    
    void CleanupCurrentScene();
    bool ApplySceneCameraOverride(const std::string& sceneFilePath);
    void ComputeSceneBoundingBox();
    void RebuildSceneBVH();
    

    // Ray Casting (for mouse picking)

    
    std::shared_ptr<SceneNode> PerformRayQuery(const RayCast::Ray& ray);
    glm::vec3 ScreenToWorldRay(int mouseX, int mouseY, int windowWidth, int windowHeight);
    std::shared_ptr<SceneNode> RayIntersectScene(const glm::vec3& rayOrigin, const glm::vec3& rayDirection);
    bool RayIntersectNode(const std::shared_ptr<SceneNode>& node, const glm::vec3& rayOrigin, 
                          const glm::vec3& rayDirection, const glm::mat4& worldTransform);
    bool RayIntersectAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                          const glm::vec3& aabbMin, const glm::vec3& aabbMax);


    // Window References (not owned)

    
    SDL_Window* m_window;
    SDL_GLContext m_glContext;
    int m_windowWidth;
    int m_windowHeight;


    // Configuration

    
    json m_config;
    std::string m_modelPath;
    std::string m_fontPath;
    std::string m_iconPath;
    bool m_vsync;


    // Graphics Subsystems (owned)

    
    std::shared_ptr<ModularRenderer> m_modularRenderer;
    unsigned int m_shaderProgram;
    unsigned int m_shadowShader;
    glm::vec3 m_environmentColor;


    // Lighting Subsystems (owned)

    
    std::shared_ptr<DirectionalLight> m_lighting;
    glm::vec3 m_lightPos;
    glm::vec3 m_lightDir;
    bool m_shadowsEnabled;
    float m_shadowBias;
    float m_shadowNear;
    float m_shadowFar;
    int m_shadowSize;
    float m_splitLambda;


    // Camera (owned)

    
    std::shared_ptr<Camera> m_camera;


    // Scene Management (owned)

    
    std::shared_ptr<SceneGraph> m_sceneGraph;
    std::shared_ptr<SceneLoader> m_sceneLoader;
    std::shared_ptr<ModelManager> m_modelManager;
    std::string m_sceneToLoad;
    std::string m_sceneName;
    std::string m_currentSceneFilePath;
    float m_exposure;
    float m_gamma;
    bool m_sceneHasAudioNodes;


    // Physics (owned)

    
    std::shared_ptr<PhysicsEngine> m_physicsEngine;
    bool m_physicsEnabledForScene;
    bool m_requestTogglePhysics;


    // UI (owned)

    
    std::unique_ptr<ImGuiInterface> m_imguiInterface;


    // Input (owned)

    
    std::unique_ptr<InputIntegration> m_inputIntegration;


    // Performance Tracking

    
    float m_frameCount;
    float m_fpsUpdateTime;
    float m_fps;
    float m_frameTime;
    
    // Circular buffer for frame time data (avoids vector erase overhead)
    static constexpr size_t FRAME_TIME_BUFFER_SIZE = 100;
    std::array<float, FRAME_TIME_BUFFER_SIZE> m_frameTimeBuffer;
    size_t m_frameTimeIndex;
    size_t m_frameTimeCount;
    
    static constexpr float FPS_UPDATE_INTERVAL = 0.5f;
    std::unique_ptr<PerformanceRecorder> m_performanceRecorder;
    std::unique_ptr<RuntimeStateManager> m_stateManager;


    // Scene Bounds Caching

    
    glm::vec3 m_cachedMinBounds;
    glm::vec3 m_cachedMaxBounds;
    glm::vec3 m_cachedSceneCenter;
    float m_cachedSceneRadius;
    bool m_boundingBoxCached;


    // Spatial Acceleration

    
    std::unique_ptr<BVH::SceneNodeBVH> m_sceneBVH;
    bool m_bvhDirty;


    // UI Data Caching

    
    std::shared_ptr<Camera> m_cachedImGuiCamera;
    std::shared_ptr<SceneGraph> m_cachedImGuiSceneGraph;
    std::shared_ptr<DirectionalLight> m_cachedImGuiLighting;
};
