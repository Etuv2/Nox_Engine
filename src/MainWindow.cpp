#include "FrameProfiler.h"
#include "MainWindow.h"
#include "ui/Pointer.h"
#include "Core.h"
#include "ImGuiInterface.h"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstdlib>

#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <SDL/SDL_image.h>
#include <IMGUI/imgui.h>
#include <IMGUI/imgui_impl_sdl2.h>
#include "stb_image_write.h"
#include <filesystem>
#include <vector>

namespace {
std::string GetEnvString(const char* name)
{
#if defined(_MSC_VER)
    char* rawValue = nullptr;
    size_t valueLength = 0;
    if (_dupenv_s(&rawValue, &valueLength, name) != 0 || rawValue == nullptr) {
        return {};
    }
    std::string result(rawValue);
    std::free(rawValue);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

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

namespace { SDL_Window* g_noxMainWindow = nullptr; }
SDL_Window* NoxMainWindow() { return g_noxMainWindow; }

MainWindow::MainWindow()
    : m_window(nullptr)
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
        m_windowMode = m_config.value("windowMode", std::string("windowed"));
        // Launch preset / command line.
        if (!m_launch.window.empty()) m_windowMode = m_launch.window;
        if (m_launch.width > 0) m_windowWidth = m_launch.width;
        if (m_launch.height > 0) m_windowHeight = m_launch.height;
        m_windowTitle = m_launch.mode == "game" ? "NOX Racing - Night Sprint" : "NOX Editor";
        // Overrides for scripted runs (e.g. native-resolution benchmarks).
        m_windowWidth = GetEnvInt("NOX_WINDOW_WIDTH", m_windowWidth);
        m_windowHeight = GetEnvInt("NOX_WINDOW_HEIGHT", m_windowHeight);
        if (const std::string mode = GetEnvString("NOX_WINDOW_MODE"); !mode.empty()) {
            m_windowMode = mode;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[MainWindow] Exception loading config.json: " << e.what() << std::endl;
        return false;
    }
    return true;
}

bool MainWindow::CreateWindow() {
    Uint32 flags = SDL_WINDOW_SHOWN | rhi::PrepareWindow(rhi::Backend::OpenGL, false);
    if (m_windowMode == "borderless") {
        // Desktop-resolution borderless fullscreen: the window takes the native display size.
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    else if (m_windowMode == "fullscreen") {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    else {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    m_window = SDL_CreateWindow(
        m_windowTitle.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        m_windowWidth,
        m_windowHeight,
        flags
    );
    
    g_noxMainWindow = m_window;
    if (!m_window) {
        std::cerr << "[MainWindow] Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return false;
    }
    if (m_windowMode != "windowed") {
        SDL_GetWindowSize(m_window, &m_windowWidth, &m_windowHeight);
        std::cout << "[MainWindow] " << m_windowMode << " window " << m_windowWidth << "x" << m_windowHeight << std::endl;
    }
    // Pointer layer: window coordinates are the UI/draw-list space; log the pixel size so a DPI
    // mismatch (drawable != window size) is visible in validation logs.
    ui::Pointer::Get().SetWindow(m_window);
    {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
        std::cout << "[MainWindow] Window " << m_windowWidth << "x" << m_windowHeight << " (pixels " << pw << "x" << ph
                  << ", display scale " << ui::Pointer::Get().DpiScale() << ")" << std::endl;
    }
    
    return true;
}

bool MainWindow::CreateGLContext() {
    // Creates the backend context, vsync and loader state, and makes the device the engine's device.
    m_device = rhi::CreateDevice({ rhi::Backend::OpenGL, m_window, false, m_vsync });
    if (!m_device) {
        std::cerr << "[MainWindow] OpenGL context could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return false;
    }
    rhi::SetGpu(m_device.get());

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
    // Fullscreen modes render at true pixel resolution on scaled (e.g. 125%) Windows desktops.
    // Windowed mode keeps the system-scaled editor unless "dpiAware" is set in config.json.
    if (m_windowMode != "windowed" || m_config.value("dpiAware", false)) {
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        std::cerr << "[MainWindow] SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return false;
    }
    
    if (TTF_Init() < 0) {
        std::cerr << "[MainWindow] TTF_Init failed: " << TTF_GetError() << std::endl;
        SDL_Quit();
        return false;
    }
    
    // Create window (rhi::PrepareWindow supplies the backend window flags and attributes)
    if (!CreateWindow()) {
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    // Create render device (OpenGL context)
    if (!CreateGLContext()) {
        SDL_DestroyWindow(m_window);
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    // Set window icon
    SetWindowIcon();

    // Mouse mode is chosen by Core (free cursor in the editor, hidden cursor in the game).

    // Create and initialize engine Core
    m_core = std::make_unique<Core>();
    {
        Core::LaunchOptions options;
        options.mode = m_launch.mode == "game" ? Core::AppMode::Game : Core::AppMode::Editor;
        options.scene = m_launch.scene;
        options.quality = m_launch.quality;
        m_core->SetLaunchOptions(options);
    }
    if (!m_core->Initialize(m_window, m_windowWidth, m_windowHeight)) {
        std::cerr << "[MainWindow] Failed to initialize engine Core" << std::endl;
        // Partially initialized engine objects still own GPU resources: the ordered cleanup shuts
        // them down (and ImGui) while the device exists, then releases the device and SDL.
        Cleanup();
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
        // Engine pointer layer sees every mouse/window event (UI hit regions, focus, capture).
        ui::Pointer::Get().ProcessEvent(event);

        // Let Core's input system process the event first
        bool handledByInput = false;
        if (m_core) {
            handledByInput = m_core->ProcessInputEvent(event);
        }

        // The editor UI always sees events (it routes focus itself) unless the fly camera consumed them.
        if (!handledByInput || (m_core && m_core->GetAppMode() == Core::AppMode::Game)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }

        // Handle system-level events
        switch (event.type) {
        case SDL_QUIT:
            m_running = false;
            break;
            
        case SDL_WINDOWEVENT:
            HandleWindowEvent(event.window);
            break;
            
        // Escape opens the pause menu in game mode; quitting uses menus or the close button.
        // Picking and wheel dolly are handled per viewport by the editor workspace.
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
            rhi::Gpu().SetViewport({ 0, 0, m_windowWidth, m_windowHeight });

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

    // Focus: the pointer layer releases capture and drops held buttons; Core resets held keys.
    case SDL_WINDOWEVENT_FOCUS_LOST:
        std::cout << "[MainWindow] Focus lost" << std::endl;
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        std::cout << "[MainWindow] Focus gained" << std::endl;
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

        FrameProfiler::BeginFrame();
        // Always process window events
        ProcessEvents();
        FrameProfiler::Mark("events");
        
        // Skip rendering and update when window is not visible
        if (!m_isVisible) {
            // Still need to sleep briefly to avoid busy-waiting
            SDL_Delay(10); // 10ms sleep when hidden
            continue;
        }
        
        // Update and render through Core only when visible. Pointer frame: input latched before the
        // update, UI hit regions drawn this frame become the hit set of the next one.
        ui::Pointer::Get().BeginFrame(deltaTime);
        if (m_core) {
            m_core->Update(deltaTime);
            m_core->Render(m_windowWidth, m_windowHeight);
        }
        ui::Pointer::Get().EndFrame();
        ++renderedFrames;
        if (exitAfterFrames > 0 && renderedFrames >= exitAfterFrames) {
            m_running = false;
        }
        if (m_core && m_core->IsQuitRequested()) {
            m_running = false;
        }

        // NOX_FINAL_DUMP_DIR + NOX_FINAL_DUMP_FRAMES (comma list): save the final backbuffer,
        // including UI/HUD, for validation captures.
        static const std::string finalDumpDir = GetEnvString("NOX_FINAL_DUMP_DIR");
        if (!finalDumpDir.empty()) {
            static const std::string frames = "," + GetEnvString("NOX_FINAL_DUMP_FRAMES") + ",";
            // NOX_FINAL_DUMP_EVERY=N (+ optional NOX_FINAL_DUMP_START): sequence dumps for recordings.
            static const int every = GetEnvInt("NOX_FINAL_DUMP_EVERY", 0);
            static const int start = GetEnvInt("NOX_FINAL_DUMP_START", 1);
            const bool sequence = every > 0 && renderedFrames >= start && (renderedFrames - start) % every == 0;
            if (sequence || frames.find("," + std::to_string(renderedFrames) + ",") != std::string::npos) {
                std::vector<unsigned char> pixels(static_cast<size_t>(m_windowWidth) * m_windowHeight * 3);
                rhi::Gpu().BindReadFramebuffer(rhi::kBackBuffer);
                rhi::Gpu().ReadPixels(rhi::kBackBuffer, 0, { 0, 0, m_windowWidth, m_windowHeight }, rhi::PixelFormat::RGB, rhi::PixelType::UByte, pixels.data(), 1);
                std::error_code ec;
                std::filesystem::create_directories(finalDumpDir, ec);
                char name[32];
                std::snprintf(name, sizeof(name), "/final_%05d.png", renderedFrames);
                const std::string path = finalDumpDir + name;
                stbi_flip_vertically_on_write(1);
                stbi_write_png(path.c_str(), m_windowWidth, m_windowHeight, 3, pixels.data(), m_windowWidth * 3);
                stbi_flip_vertically_on_write(0);
            }
        }

        FrameProfiler::Mark("capture");
        // Swap buffers (already checking visibility, so always swap here)
        rhi::Gpu().Present();
        FrameProfiler::Mark("present");
        FrameProfiler::EndFrame();
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
        
        // Check if the renderer backend is initialized before shutting it down (also shuts down SDL)
        if (io.BackendRendererUserData != nullptr) {
            rhi::Gpu().ImGuiShutdown();
        }
        else {
            // Shutdown SDL backend (safer, less likely to crash)
            ImGui_ImplSDL2_Shutdown();
        }

        // Destroy ImGui context
        ImGui::DestroyContext();
    }

    // Release the render device (after all GPU resources are gone)
    if (m_device) {
        rhi::SetGpu(nullptr);
        m_device.reset();
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
