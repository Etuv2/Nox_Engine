#include "MainWindow.h"
#include "Core.h"
#include "ImGuiInterface.h"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstdlib>

#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <SDL/SDL_image.h>
#include <GL/glew.h>
#include <IMGUI/imgui.h>
#include <IMGUI/imgui_impl_sdl2.h>
#include <IMGUI/imgui_impl_opengl3.h>

namespace {
int GetEnvInt(const char* name, int fallback)
{
#if defined(_MSC_VER)
    char* rawValue = nullptr;
    size_t valueLength = 0;
    if (_dupenv_s(&rawValue, &valueLength, name) != 0 || rawValue == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(rawValue, &end, 10);
    const bool parsedAny = end != rawValue;
    std::free(rawValue);
    return parsedAny ? static_cast<int>(parsed) : fallback;
#else
    const char* value = std::getenv(name);
    if (!value || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end != value ? static_cast<int>(parsed) : fallback;
#endif
}
}

MainWindow::MainWindow()
    : m_window(nullptr)
    , m_glContext(nullptr)
    , m_windowWidth(1920)
    , m_windowHeight(1080)
    , m_windowTitle("NOX Engine")
    , m_vsync(true)
    , m_fullscreen(false)
    , m_running(false)
    , m_lastTime(0)
    , m_cleanedUp(false)  // Initialize cleanup guard
    , m_isVisible(true)   // Assume visible initially
{
}

MainWindow::~MainWindow() {
    Cleanup();
}

bool MainWindow::LoadWindowConfiguration() {
    try {
		std::ifstream configFile(".\\config\\config.json");
        if (!configFile || !configFile.is_open()) {
            std::cerr << "[MainWindow] Could not open config.json" << std::endl;
            return false;
        }
        configFile >> m_config;

        m_windowWidth = m_config.value("windowWidth", 1920);
        m_windowHeight = m_config.value("windowHeight", 1080);
        m_windowTitle = m_config.value("windowTitle", "NOX Engine");
        m_iconPath = m_config.value("iconPath", "");
        m_vsync = m_config.value("vsync", true);
    }
    catch (const std::exception& e) {
        std::cerr << "[MainWindow] Exception loading config.json: " << e.what() << std::endl;
        return false;
    }
    return true;
}

bool MainWindow::CreateWindow() {
    m_window = SDL_CreateWindow(
        m_windowTitle.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        m_windowWidth,
        m_windowHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    
    if (!m_window) {
        std::cerr << "[MainWindow] Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return false;
    }
    
    return true;
}

bool MainWindow::CreateGLContext() {
    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        std::cerr << "[MainWindow] OpenGL context could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return false;
    }
    
    // Set vsync
    SDL_GL_SetSwapInterval(m_vsync ? 1 : 0);
    
    // Initialize GLEW
    glewExperimental = GL_TRUE;
    GLenum glewError = glewInit();
    glGetError(); // Clear any spurious errors
    if (glewError != GLEW_OK) {
        std::cerr << "[MainWindow] Error initializing GLEW! " << glewGetErrorString(glewError) << std::endl;
        return false;
    }
    
    return true;
}

void MainWindow::SetWindowIcon() {
    if (!m_iconPath.empty()) {
        SDL_Surface* icon = IMG_Load(m_iconPath.c_str());
        if (icon) {
            SDL_SetWindowIcon(m_window, icon);
            SDL_FreeSurface(icon);
        }
        else {
            std::cerr << "[MainWindow] Failed to load window icon: " << IMG_GetError() << std::endl;
        }
    }
}

bool MainWindow::Initialize() {
    std::cout << "[MainWindow] Initializing..." << std::endl;
    
    // Load window configuration
    if (!LoadWindowConfiguration()) {
        return false;
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        std::cerr << "[MainWindow] SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return false;
    }
    
    if (TTF_Init() < 0) {
        std::cerr << "[MainWindow] TTF_Init failed: " << TTF_GetError() << std::endl;
        SDL_Quit();
        return false;
    }
    
    // Set OpenGL attributes before window creation
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // Create window
    if (!CreateWindow()) {
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    // Create OpenGL context
    if (!CreateGLContext()) {
        SDL_DestroyWindow(m_window);
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    // Set window icon
    SetWindowIcon();

    // Set initial mouse mode
    SDL_SetRelativeMouseMode(SDL_TRUE);

    // Create and initialize engine Core
    m_core = std::make_unique<Core>();
    if (!m_core->Initialize(m_window, m_glContext, m_windowWidth, m_windowHeight)) {
        std::cerr << "[MainWindow] Failed to initialize engine Core" << std::endl;
        SDL_GL_DeleteContext(m_glContext);
        SDL_DestroyWindow(m_window);
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    m_running = true;
    m_lastTime = SDL_GetTicks();
    
    std::cout << "[MainWindow] Initialization complete!" << std::endl;
    return true;
}

void MainWindow::ProcessEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Let Core's input system process the event first
        bool handledByInput = false;
        if (m_core) {
            handledByInput = m_core->ProcessInputEvent(event);
        }

        // Allow ImGui to process events when mouse is unlocked
        bool allowImGui = !handledByInput || (m_core && !m_core->IsMouseLocked());
        if (allowImGui) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }

        // Process ImGui keyboard input for window toggles
        if (m_core && m_core->GetImGuiInterface()) {
            m_core->GetImGuiInterface()->ProcessKeyboardInput();
        }

        // Handle system-level events
        switch (event.type) {
        case SDL_QUIT:
            m_running = false;
            break;
            
        case SDL_WINDOWEVENT:
            HandleWindowEvent(event.window);
            break;
            
        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                m_running = false;
            }
            break;
            
        case SDL_MOUSEBUTTONDOWN:
            // Handle mouse picking when mouse is unlocked
            if (!handledByInput && event.button.button == SDL_BUTTON_LEFT &&
                m_core && !m_core->IsMouseLocked()) {
                m_core->HandleMouseClick(event.button.x, event.button.y, m_windowWidth, m_windowHeight);
            }
            break;
            
        case SDL_MOUSEWHEEL:
            if (m_core) {
                m_core->HandleMouseScroll(static_cast<float>(event.wheel.y));
            }
            break;
            
        default:
            break;
        }
    }
}

void MainWindow::HandleWindowEvent(const SDL_WindowEvent& windowEvent) {
    switch (windowEvent.event) {
    case SDL_WINDOWEVENT_SIZE_CHANGED:
    case SDL_WINDOWEVENT_RESIZED:
    {
        int newWidth = windowEvent.data1;
        int newHeight = windowEvent.data2;
        
        if (newWidth > 0 && newHeight > 0 &&
            (newWidth != m_windowWidth || newHeight != m_windowHeight)) {
            m_windowWidth = newWidth;
            m_windowHeight = newHeight;

            // Update OpenGL viewport
            glViewport(0, 0, m_windowWidth, m_windowHeight);

            // Notify Core of the resize
            if (m_core) {
                m_core->OnWindowResize(m_windowWidth, m_windowHeight);
            }

            std::cout << "[MainWindow] Window resized to " << m_windowWidth << "x" << m_windowHeight << std::endl;
        }
    }
    break;
    
    case SDL_WINDOWEVENT_MINIMIZED:
        m_isVisible = false;
        std::cout << "[MainWindow] Window minimized, pausing rendering" << std::endl;
        break;
        
    case SDL_WINDOWEVENT_RESTORED:
    case SDL_WINDOWEVENT_SHOWN:
        m_isVisible = true;
        std::cout << "[MainWindow] Window restored/shown, resuming rendering" << std::endl;
        break;
        
    case SDL_WINDOWEVENT_HIDDEN:
        m_isVisible = false;
        std::cout << "[MainWindow] Window hidden, pausing rendering" << std::endl;
        break;
    }
}

void MainWindow::Run() {
    std::cout << "[MainWindow] Starting main loop..." << std::endl;
    const int exitAfterFrames = std::max(GetEnvInt("NOX_EXIT_AFTER_FRAMES", 0), 0);
    int renderedFrames = 0;
    
    while (m_running) {
        // Calculate delta time
        Uint32 currentTime = SDL_GetTicks();
        float deltaTime = (currentTime - m_lastTime) / 1000.0f;
        m_lastTime = currentTime;

        // Always process window events
        ProcessEvents();
        
        // Skip rendering and update when window is not visible
        if (!m_isVisible) {
            // Still need to sleep briefly to avoid busy-waiting
            SDL_Delay(10); // 10ms sleep when hidden
            continue;
        }
        
        // Update and render through Core only when visible
        if (m_core) {
            m_core->Update(deltaTime);
            m_core->Render(m_windowWidth, m_windowHeight);
        }
        ++renderedFrames;
        if (exitAfterFrames > 0 && renderedFrames >= exitAfterFrames) {
            m_running = false;
        }

        // Swap buffers (already checking visibility, so always swap here)
        SDL_GL_SwapWindow(m_window);
    }
    
    std::cout << "[MainWindow] Main loop ended" << std::endl;
}

void MainWindow::Cleanup() {
    // Prevent double cleanup
    if (m_cleanedUp) {
        std::cout << "[MainWindow] Cleanup already performed, skipping" << std::endl;
        return;
    }
    m_cleanedUp = true;

    std::cout << "[MainWindow] Cleaning up..." << std::endl;

    // Shutdown engine Core first
    if (m_core) {
        m_core->Shutdown();
        m_core.reset();
    }

    // Only cleanup ImGui if context exists and backend data is present
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        
        // Check if OpenGL backend is initialized before shutting it down
        if (io.BackendRendererUserData != nullptr) {
            ImGui_ImplOpenGL3_Shutdown();
        }
        
        // Shutdown SDL backend (safer, less likely to crash)
        ImGui_ImplSDL2_Shutdown();
        
        // Destroy ImGui context
        ImGui::DestroyContext();
    }

    // Cleanup OpenGL context
    if (m_glContext) {
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }

    // Cleanup window
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    // Quit SDL subsystems
    TTF_Quit();
    SDL_Quit();

    std::cout << "[MainWindow] Cleanup complete" << std::endl;
}
