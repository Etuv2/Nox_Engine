#pragma once

#include <SDL/SDL.h>
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "json.hpp"
#include "Camera.h"
#include "Scene.h"
#include "SceneGraph.h"
#include "SceneNode.h"
#include "SceneLoader.h"
#include "ModelManager.h"
#include "DirectionalLight.h"
#include "LightManager.h"
#include "ModularRenderer.h"
#include "ImGuiInterface.h"
#include "PhysicsEngine.h"
#include "RayCast.h"
#include "BVH.h"
#include "GizmoRayCast.h"
#include "Input/InputIntegration.h"
using json = nlohmann::json;

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Initialize();
    void Run();
    void Cleanup();

private:
    // Core SDL/OpenGL
    SDL_Window* m_window;
    SDL_GLContext m_glContext;
    int m_windowWidth, m_windowHeight;
    std::string m_windowTitle;
    std::string m_iconPath;
    bool m_vsync;
    bool m_running;
    bool m_wireframe = false;
    bool m_mouselock = true;
    bool m_fullscreen = false;

    // Configuration
    json m_config;
    std::string m_modelPath;
    std::string m_fontPath;
    bool LoadConfiguration();

    // Graphics
    glm::vec3 m_lightPos;
    glm::vec3 m_lightDir;
    glm::vec3 m_environmentColor;
    unsigned int m_shaderProgram;
    unsigned int m_shadowShader;
    std::shared_ptr<ModularRenderer> m_modularRenderer; // Modular renderer architecture (ONLY renderer now)

    // Scene Management
    std::shared_ptr<Camera> m_camera;
    std::shared_ptr<SceneGraph> m_sceneGraph;
    std::shared_ptr<SceneLoader> m_sceneLoader;
    std::shared_ptr<ModelManager> m_modelManager;
    std::string m_scene_to_load;
    std::string m_scene_name;
    std::string m_currentSceneFilePath; // NEW: Track current scene file path for saving

    // Lighting
    std::shared_ptr<DirectionalLight> m_lighting;
    std::unique_ptr<LightManager> m_lightManager;
    bool m_shadows_enabled = true;
    float m_shadow_bias = 0.002f; // Reduced bias for better quality
    float m_shadow_near = 0.1f;
    float m_shadow_far = 1000.0f;
    int m_shadow_size = 1024; // Keep at 1024 as requested
    float m_split_lambda = 0.5f; // Better lambda for close-up detail

    // Studio Lighting
    bool m_studioLightingEnabled = false;
    int m_currentPreset = 0;

    // Physics
    std::shared_ptr<PhysicsEngine> m_physicsEngine;
    bool m_physicsEnabledForScene = false;
    bool m_requestTogglePhysics = false;

    // UI
    std::unique_ptr<ImGuiInterface> m_imguiInterface;

    // Input System
    std::unique_ptr<InputIntegration> m_inputIntegration;

    // Performance
    Uint32 m_lastTime;
    float m_frameCount;
    float m_fpsUpdateTime;
    float m_fps;
    float m_frameTime;
    std::vector<float> m_frameTimeData;
    static constexpr float FPS_UPDATE_INTERVAL = 0.5f;

    // Rendering parameters
    float m_exposure = 1.0f;
    float m_gamma = 2.2f;

    // Scene bounds caching
    glm::vec3 m_cachedMinBounds;
    glm::vec3 m_cachedMaxBounds;
    glm::vec3 m_cachedSceneCenter;
    float m_cachedSceneRadius;
    bool m_boundingBoxCached = false;

    // Spatial Acceleration Structures
    std::unique_ptr<BVH::SceneNodeBVH> m_sceneBVH;
    bool m_bvhDirty = true;

    // Core functions
    void ProcessEvents();
    void Update(float deltaTime);
    void Render(float fps);

    // Scene management
    void ComputeSceneBoundingBox();
    void RebuildSceneBVH();

    // Enhanced Mouse Picking with BVH acceleration
    void HandleMouseClick(int mouseX, int mouseY);
    std::shared_ptr<SceneNode> PerformRayQuery(const RayCast::Ray& ray);
    
    // Legacy compatibility functions (kept for backward compatibility)
    glm::vec3 ScreenToWorldRay(int mouseX, int mouseY);
    std::shared_ptr<SceneNode> RayIntersectScene(const glm::vec3& rayOrigin, const glm::vec3& rayDirection);
    bool RayIntersectNode(const std::shared_ptr<SceneNode>& node, const glm::vec3& rayOrigin, const glm::vec3& rayDirection, const glm::mat4& worldTransform);
    bool RayIntersectAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, const glm::vec3& aabbMin, const glm::vec3& aabbMax);

    // Utility functions
    unsigned int CreateShaderProgram(const std::string& vertexPath, const std::string& fragmentPath);
};