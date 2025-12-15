#pragma once

#include <SDL/SDL.h>
#include <GL/glew.h>
#include <memory>
#include <string>

#include "json.hpp"
using json = nlohmann::json;

// Forward declarations
class Core;

/**
 * @brief MainWindow handles the OS window lifecycle and surface management.
 * 
 * This class is responsible for:
 * - SDL window creation and destruction
 * - OpenGL context creation and management
 * - Event polling and routing to Core
 * - Window resize/fullscreen handling
 * - Swap chain management
 * 
 * All engine functionality is delegated to Core through explicit method calls.
 */
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    /**
     * @brief Initialize the window and create the engine Core.
     * @return true if initialization succeeded
     */
    bool Initialize();
    
    /**
     * @brief Run the main application loop.
     */
    void Run();
    
    /**
     * @brief Cleanup window resources.
     */
    void Cleanup();

    // ========================================================================
    // Window State Accessors (for Core to query if needed)
    // ========================================================================
    
    SDL_Window* GetWindow() { return m_window; }
    SDL_GLContext GetGLContext() { return m_glContext; }
    int GetWindowWidth() const { return m_windowWidth; }
    int GetWindowHeight() const { return m_windowHeight; }
    bool IsRunning() const { return m_running; }
    
    /**
     * @brief Request the application to stop running.
     */
    void RequestQuit() { m_running = false; }

private:
    // ========================================================================
    // Window Lifecycle
    // ========================================================================
    
    bool LoadWindowConfiguration();
    bool CreateWindow();
    bool CreateGLContext();
    void SetWindowIcon();
    
    // ========================================================================
    // Event Handling
    // ========================================================================
    
    void ProcessEvents();
    void HandleWindowEvent(const SDL_WindowEvent& windowEvent);

    // ========================================================================
    // SDL/OpenGL Resources (owned by MainWindow)
    // ========================================================================
    
    SDL_Window* m_window;
    SDL_GLContext m_glContext;
    
    // ========================================================================
    // Window Configuration
    // ========================================================================
    
    json m_config;
    int m_windowWidth;
    int m_windowHeight;
    std::string m_windowTitle;
    std::string m_iconPath;
    bool m_vsync;
    bool m_fullscreen;
    
    // ========================================================================
    // Application State
    // ========================================================================
    
    bool m_running;
    Uint32 m_lastTime;
    bool m_cleanedUp;  // Guard to prevent double cleanup
    bool m_isVisible;  // Cached visibility state to avoid SDL queries every frame
    
    // ========================================================================
    // Engine Core (owns all engine subsystems)
    // ========================================================================
    
    std::unique_ptr<Core> m_core;
};