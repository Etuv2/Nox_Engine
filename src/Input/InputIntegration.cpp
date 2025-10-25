#include "InputIntegration.h"
#include "../MainWindow.h"
#include "../Camera.h"
#include "../SceneGraph.h"
#include "../ImGuiInterface.h"
#include "../DirectionalLight.h"
#include "../LightManager.h"
#include <iostream>

InputIntegration::InputIntegration()
    : m_inputManager(std::make_unique<Input::InputManager>())
    , m_mainWindow(nullptr)
    , m_imguiInterface(nullptr)
    , m_lightManager(nullptr)
    , m_initialized(false)
    , m_mouseLocked(true)
    , m_cameraSpeed(2.5f)
    , m_mouseSensitivity(0.1f)
    , m_gamepadLookSensitivity(100) { // NEW: Default gamepad look sensitivity
}

InputIntegration::~InputIntegration() {
    Shutdown();
}

bool InputIntegration::Initialize(MainWindow* mainWindow) {
    if (m_initialized) {
        return true;
    }
    
    if (!mainWindow) {
        std::cerr << "[InputIntegration] MainWindow pointer is null" << std::endl;
        return false;
    }
    
    m_mainWindow = mainWindow;
    
    // Initialize the input manager
    if (!m_inputManager->Initialize()) {
        std::cerr << "[InputIntegration] Failed to initialize InputManager" << std::endl;
        return false;
    }
    
    // Set up input contexts and callbacks
    SetupInputContexts();
    
    // Load configuration if available
    LoadConfiguration();
    
    m_initialized = true;
    
    std::cout << "[InputIntegration] Initialized successfully" << std::endl;
    return true;
}

void InputIntegration::Shutdown() {
    if (!m_initialized) {
        return;
    }
    
    if (m_inputManager) {
        m_inputManager->Shutdown();
    }
    
    m_initialized = false;
    
    std::cout << "[InputIntegration] Shutdown complete" << std::endl;
}

void InputIntegration::Update(float deltaTime) {
    if (!m_initialized || !m_inputManager) {
        return;
    }
    
    m_inputManager->Update(deltaTime);
    UpdateMouseLockState();
}

bool InputIntegration::ProcessEvent(const SDL_Event& event) {
    if (!m_initialized || !m_inputManager) {
        return false;
    }
    
    return m_inputManager->ProcessEvent(event);
}

void InputIntegration::EnableContext(const std::string& contextName, bool enabled) {
    if (m_inputManager) {
        m_inputManager->SetContextEnabled(contextName, enabled);
        
        if (enabled) {
            m_inputManager->PushContext(contextName);
        }
    }
}

void InputIntegration::DisableContext(const std::string& contextName) {
    if (m_inputManager) {
        m_inputManager->SetContextEnabled(contextName, false);
    }
}

void InputIntegration::SetMouseLocked(bool locked) {
    m_mouseLocked = locked;
    SDL_SetRelativeMouseMode(locked ? SDL_TRUE : SDL_FALSE);
    
    std::cout << "[InputIntegration] Mouse " << (locked ? "locked" : "unlocked") 
              << " - " << (locked ? "Camera control" : "Gizmo interaction") << std::endl;
}

bool InputIntegration::LoadConfiguration(const std::string& configPath) {
    m_configPath = configPath;
    
    if (m_inputManager) {
        bool result = m_inputManager->LoadConfiguration(configPath);
        if (result) {
            std::cout << "[InputIntegration] Loaded input configuration" << std::endl;
        } else {
            std::cout << "[InputIntegration] Using default input configuration" << std::endl;
        }
        return result;
    }
    
    return false;
}

bool InputIntegration::SaveConfiguration(const std::string& configPath) {
    if (m_inputManager) {
        return m_inputManager->SaveConfiguration(configPath);
    }
    return false;
}

void InputIntegration::SetupInputContexts() {
    SetupGlobalContext();
    SetupCameraContext();
    SetupEditorContext();
    SetupLightingContext();
    SetupAnimationContext();
    SetupGamepadContext();
    
    std::cout << "[InputIntegration] Activating default contexts..." << std::endl;
    
    // Activate default contexts
    m_inputManager->PushContext(Input::Contexts::GLOBAL);
    m_inputManager->PushContext(Input::Contexts::CAMERA);
    m_inputManager->PushContext("editor");
    m_inputManager->PushContext("lighting");
    m_inputManager->PushContext("animation");
    m_inputManager->PushContext("gamepad"); // CRITICAL: Add gamepad context to active stack!
    
    std::cout << "[InputIntegration] All contexts activated" << std::endl;
}

void InputIntegration::SetupGlobalContext() {
    auto context = m_inputManager->CreateContext(Input::Contexts::GLOBAL);
    
    // Application controls
    context->BindKeyAction(Input::Actions::APP_EXIT, SDLK_ESCAPE);
    context->RegisterActionCallback(Input::Actions::APP_EXIT, 
        [this](float) { OnApplicationAction("exit"); });
    
    context->BindKeyAction(Input::Actions::APP_FULLSCREEN, SDLK_F11);
    context->RegisterActionCallback(Input::Actions::APP_FULLSCREEN, 
        [this](float) { OnApplicationAction("fullscreen"); });
    
    context->BindKeyAction(Input::Actions::APP_TOGGLE_WIREFRAME, SDLK_SPACE);
    context->RegisterActionCallback(Input::Actions::APP_TOGGLE_WIREFRAME, 
        [this](float) { OnApplicationAction("wireframe"); });
    
    context->BindKeyAction(Input::Actions::APP_TOGGLE_MOUSE_LOCK, SDLK_LSHIFT);
    context->RegisterActionCallback(Input::Actions::APP_TOGGLE_MOUSE_LOCK, 
        [this](float) { OnApplicationAction("mouse_lock"); });
    
    context->BindKeyAction(Input::Actions::SCENE_TOGGLE_PHYSICS, SDLK_F10);
    context->RegisterActionCallback(Input::Actions::SCENE_TOGGLE_PHYSICS, 
        [this](float) { OnApplicationAction("toggle_physics"); });
    
    context->BindKeyAction(Input::Actions::SHADOWS_TOGGLE, SDLK_LCTRL);
    context->RegisterActionCallback(Input::Actions::SHADOWS_TOGGLE, 
        [this](float) { OnApplicationAction("toggle_shadows"); });
    
    // Debug controls
    context->BindKeyAction(Input::Actions::DEBUG_LIGHT_INFO, SDLK_i);
    context->RegisterActionCallback(Input::Actions::DEBUG_LIGHT_INFO, 
        [this](float) { OnApplicationAction("debug_light_info"); });
    
    context->BindKeyAction(Input::Actions::DEBUG_LIGHT_VALIDATION, SDLK_F9);
    context->RegisterActionCallback(Input::Actions::DEBUG_LIGHT_VALIDATION, 
        [this](float) { OnApplicationAction("debug_light_validation"); });
}

void InputIntegration::SetupCameraContext() {
    auto context = m_inputManager->CreateContext(Input::Contexts::CAMERA);
    
    // Movement controls (HOLD type for continuous movement)
    context->BindKeyAction(Input::Actions::CAMERA_MOVE_FORWARD, SDLK_w, Input::ActionType::HOLD);
    context->RegisterActionCallback(Input::Actions::CAMERA_MOVE_FORWARD, 
        [this](float deltaTime) { OnCameraMovement("forward", deltaTime); });
    
    context->BindKeyAction(Input::Actions::CAMERA_MOVE_BACKWARD, SDLK_s, Input::ActionType::HOLD);
    context->RegisterActionCallback(Input::Actions::CAMERA_MOVE_BACKWARD, 
        [this](float deltaTime) { OnCameraMovement("backward", deltaTime); });
    
    context->BindKeyAction(Input::Actions::CAMERA_MOVE_LEFT, SDLK_a, Input::ActionType::HOLD);
    context->RegisterActionCallback(Input::Actions::CAMERA_MOVE_LEFT, 
        [this](float deltaTime) { OnCameraMovement("left", deltaTime); });
    
    context->BindKeyAction(Input::Actions::CAMERA_MOVE_RIGHT, SDLK_d, Input::ActionType::HOLD);
    context->RegisterActionCallback(Input::Actions::CAMERA_MOVE_RIGHT, 
        [this](float deltaTime) { OnCameraMovement("right", deltaTime); });
    
    context->BindKeyAction(Input::Actions::CAMERA_MOVE_UP, SDLK_e, Input::ActionType::HOLD);
    context->RegisterActionCallback(Input::Actions::CAMERA_MOVE_UP, 
        [this](float deltaTime) { OnCameraMovement("up", deltaTime); });
    
    context->BindKeyAction(Input::Actions::CAMERA_MOVE_DOWN, SDLK_q, Input::ActionType::HOLD);
    context->RegisterActionCallback(Input::Actions::CAMERA_MOVE_DOWN, 
        [this](float deltaTime) { OnCameraMovement("down", deltaTime); });
    
    // Mouse look
    context->BindMouseAxisAction(Input::Actions::CAMERA_LOOK);
    context->RegisterAxisCallback(Input::Actions::CAMERA_LOOK, 
        [this](float deltaX, float deltaY) { OnCameraLook(deltaX, deltaY); });
}

void InputIntegration::SetupEditorContext() {
    auto context = m_inputManager->CreateContext("editor");
    
    std::cout << "[InputIntegration] Setting up editor context..." << std::endl;
    
    // Object selection
    context->BindMouseAction(Input::Actions::OBJECT_SELECT, Input::MouseButton::LEFT);
    context->RegisterActionCallback(Input::Actions::OBJECT_SELECT, 
        [this](float) { 
            std::cout << "[InputIntegration] Mouse click for object selection" << std::endl;
            OnObjectSelect(); 
        });
    
    // CRITICAL FIX: Gizmo controls - ensure they use PRESS type for immediate response
    context->BindKeyAction(Input::Actions::GIZMO_TOGGLE, SDLK_g, Input::ActionType::PRESS);
    context->RegisterActionCallback(Input::Actions::GIZMO_TOGGLE, 
        [this](float) { 
            std::cout << "[InputIntegration] G key pressed - Toggle gizmo" << std::endl;
            OnGizmoAction("toggle"); 
        });
    
    context->BindKeyAction(Input::Actions::GIZMO_TRANSLATE, SDLK_q, Input::ActionType::PRESS);
    context->RegisterActionCallback(Input::Actions::GIZMO_TRANSLATE, 
        [this](float) { 
            std::cout << "[InputIntegration] Q key pressed - Translate mode" << std::endl;
            OnGizmoAction("translate"); 
        });
    
    context->BindKeyAction(Input::Actions::GIZMO_ROTATE, SDLK_e, Input::ActionType::PRESS);
    context->RegisterActionCallback(Input::Actions::GIZMO_ROTATE, 
        [this](float) { 
            std::cout << "[InputIntegration] E key pressed - Rotate mode" << std::endl;
            OnGizmoAction("rotate"); 
        });
    
    context->BindKeyAction(Input::Actions::GIZMO_SCALE, SDLK_r, Input::ActionType::PRESS);
    context->RegisterActionCallback(Input::Actions::GIZMO_SCALE, 
        [this](float) { 
            std::cout << "[InputIntegration] R key pressed - Scale mode" << std::endl;
            OnGizmoAction("scale"); 
        });
    
    std::cout << "[InputIntegration] Editor context setup complete with gizmo bindings" << std::endl;
}

void InputIntegration::SetupLightingContext() {
    auto context = m_inputManager->CreateContext("lighting");
    
    // Light controls
    context->BindKeyAction(Input::Actions::LIGHT_TOGGLE_ALL, SDLK_t);
    context->RegisterActionCallback(Input::Actions::LIGHT_TOGGLE_ALL, 
        [this](float) { OnLightingAction("toggle_all"); });
    
    context->BindKeyAction(Input::Actions::LIGHT_INCREASE_INTENSITY, SDLK_KP_PLUS);
    context->RegisterActionCallback(Input::Actions::LIGHT_INCREASE_INTENSITY, 
        [this](float) { OnLightingAction("increase_intensity"); });
    
    context->BindKeyAction(Input::Actions::LIGHT_DECREASE_INTENSITY, SDLK_KP_MINUS);
    context->RegisterActionCallback(Input::Actions::LIGHT_DECREASE_INTENSITY, 
        [this](float) { OnLightingAction("decrease_intensity"); });
    
    // Alternative intensity controls using = and - keys
    context->BindKeyAction("light_increase_alt", SDLK_EQUALS);
    context->RegisterActionCallback("light_increase_alt", 
        [this](float) { 
            // Check if Shift is held for + key
            const Uint8* keystate = SDL_GetKeyboardState(NULL);
            if (keystate[SDL_SCANCODE_LSHIFT] || keystate[SDL_SCANCODE_RSHIFT]) {
                OnLightingAction("increase_intensity");
            }
        });
    
    context->BindKeyAction("light_decrease_alt", SDLK_MINUS);
    context->RegisterActionCallback("light_decrease_alt", 
        [this](float) { OnLightingAction("decrease_intensity"); });
    
    // Light presets
    context->BindKeyAction(Input::Actions::LIGHT_PRESET_1, SDLK_1);
    context->RegisterActionCallback(Input::Actions::LIGHT_PRESET_1, 
        [this](float) { ApplyLightPreset(1); });
    
    context->BindKeyAction(Input::Actions::LIGHT_PRESET_2, SDLK_2);
    context->RegisterActionCallback(Input::Actions::LIGHT_PRESET_2, 
        [this](float) { ApplyLightPreset(2); });
    
    context->BindKeyAction(Input::Actions::LIGHT_PRESET_3, SDLK_3);
    context->RegisterActionCallback(Input::Actions::LIGHT_PRESET_3, 
        [this](float) { ApplyLightPreset(3); });
    
    context->BindKeyAction(Input::Actions::LIGHT_PRESET_4, SDLK_4);
    context->RegisterActionCallback(Input::Actions::LIGHT_PRESET_4, 
        [this](float) { ApplyLightPreset(4); });
    
    context->BindKeyAction(Input::Actions::LIGHT_PRESET_5, SDLK_5);
    context->RegisterActionCallback(Input::Actions::LIGHT_PRESET_5, 
        [this](float) { ApplyLightPreset(5); });
}

void InputIntegration::SetupAnimationContext() {
    auto context = m_inputManager->CreateContext("animation");
    
    // Animation controls
    context->BindKeyAction(Input::Actions::ANIMATION_PLAY, SDLK_p);
    context->RegisterActionCallback(Input::Actions::ANIMATION_PLAY, 
        [this](float) { OnAnimationAction("play"); });
    
    context->BindKeyAction(Input::Actions::ANIMATION_PAUSE, SDLK_o);
    context->RegisterActionCallback(Input::Actions::ANIMATION_PAUSE, 
        [this](float) { OnAnimationAction("pause"); });
    
    context->BindKeyAction(Input::Actions::ANIMATION_STOP, SDLK_s);
    context->RegisterActionCallback(Input::Actions::ANIMATION_STOP, 
        [this](float) { 
            // Only trigger if Shift is NOT held (to avoid conflict with camera movement)
            const Uint8* keystate = SDL_GetKeyboardState(NULL);
            if (!(keystate[SDL_SCANCODE_LSHIFT] || keystate[SDL_SCANCODE_RSHIFT])) {
                OnAnimationAction("stop");
            }
        });
}

void InputIntegration::SetupGamepadContext() {
    auto context = m_inputManager->CreateContext("gamepad");
    
    std::cout << "[InputIntegration] Setting up gamepad context..." << std::endl;
    
    // Gamepad camera movement
    context->BindAxisAction("gamepad_move_x", Input::GamepadAxis::LEFT_X);
    context->RegisterAxisCallback("gamepad_move_x", 
        [this](float value, float) { 
            if (std::abs(value) > 0.1f) { // Apply deadzone
                OnCameraMovement(value > 0 ? "right" : "left", std::abs(value) * 0.016f); // ~16ms frame time
            }
        });
    
    context->BindAxisAction("gamepad_move_y", Input::GamepadAxis::LEFT_Y);
    context->RegisterAxisCallback("gamepad_move_y", 
        [this](float value, float) { 
            if (std::abs(value) > 0.1f) { // Apply deadzone
                OnCameraMovement(value > 0 ? "backward" : "forward", std::abs(value) * 0.016f);
            }
        });
    
    // Gamepad camera look - Uses configurable sensitivity
    context->BindAxisAction("gamepad_look_x", Input::GamepadAxis::RIGHT_X);
    context->RegisterAxisCallback("gamepad_look_x", 
        [this](float value, float) { 
            if (std::abs(value) > 0.1f) {
                // Use configurable sensitivity for horizontal look
                OnCameraLook(value * m_gamepadLookSensitivity, 0.0f); 
            }
        });
    
    context->BindAxisAction("gamepad_look_y", Input::GamepadAxis::RIGHT_Y);
    context->RegisterAxisCallback("gamepad_look_y", 
        [this](float value, float) { 
            if (std::abs(value) > 0.1f) {
                // Use configurable sensitivity for vertical look (inverted Y)
                OnCameraLook(0.0f, -value * m_gamepadLookSensitivity);
            }
        });
    
    // Gamepad buttons
    context->BindGamepadAction("gamepad_select", Input::GamepadButton::A);
    context->RegisterActionCallback("gamepad_select", 
        [this](float) { 
            std::cout << "[InputIntegration] Gamepad A button pressed" << std::endl;
            OnObjectSelect(); 
        });
    
    context->BindGamepadAction("gamepad_exit", Input::GamepadButton::START);
    context->RegisterActionCallback("gamepad_exit", 
        [this](float) { 
            std::cout << "[InputIntegration] Gamepad START button pressed" << std::endl;
            OnApplicationAction("exit"); 
        });
    
    std::cout << "[InputIntegration] Gamepad context setup complete (look sensitivity: " 
              << m_gamepadLookSensitivity << ")" << std::endl;
}

void InputIntegration::OnCameraMovement(const std::string& direction, float value) {
    if (!m_camera || !m_mouseLocked) {
        return;
    }
    
    // Simulate the old keyboard state processing
    const Uint8* keystate = SDL_GetKeyboardState(NULL);
    
    // Create a mock keystate for the camera processing
    Uint8 mockKeystate[SDL_NUM_SCANCODES] = {0};
    
    // Map direction to SDL scancodes
    if (direction == "forward") {
        mockKeystate[SDL_SCANCODE_W] = 1;
    } else if (direction == "backward") {
        mockKeystate[SDL_SCANCODE_S] = 1;
    } else if (direction == "left") {
        mockKeystate[SDL_SCANCODE_A] = 1;
    } else if (direction == "right") {
        mockKeystate[SDL_SCANCODE_D] = 1;
    } else if (direction == "up") {
        mockKeystate[SDL_SCANCODE_E] = 1;
    } else if (direction == "down") {
        mockKeystate[SDL_SCANCODE_Q] = 1;
    }
    
    // Use the camera's existing ProcessKeyboard method
    m_camera->ProcessKeyboard(mockKeystate, value);
}

void InputIntegration::OnCameraLook(float deltaX, float deltaY) {
    if (!m_camera || !m_mouseLocked) {
        return;
    }
    
    // Apply sensitivity scaling
    deltaX *= m_mouseSensitivity;
    deltaY *= m_mouseSensitivity;
    
    // Use the camera's existing ProcessMouseMovement method
    m_camera->ProcessMouseMovement(deltaX, -deltaY); // Invert Y for standard FPS controls
}

void InputIntegration::OnApplicationAction(const std::string& action) {
    if (!m_mainWindow) {
        return;
    }
    
    if (action == "exit") {
        // Set the running flag to false (we'll need to add a method to MainWindow for this)
        // For now, we can trigger the SDL_QUIT event
        SDL_Event quitEvent;
        quitEvent.type = SDL_QUIT;
        SDL_PushEvent(&quitEvent);
    } else if (action == "fullscreen") {
        // Toggle fullscreen - we'll need access to MainWindow's fullscreen state
        std::cout << "[InputIntegration] Fullscreen toggle requested" << std::endl;
        // m_mainWindow->ToggleFullscreen(); // Would need to add this method
    } else if (action == "wireframe") {
        // Toggle wireframe mode
        std::cout << "[InputIntegration] Wireframe toggle requested" << std::endl;
        // m_mainWindow->ToggleWireframe(); // Would need to add this method
    } else if (action == "mouse_lock") {
        SetMouseLocked(!m_mouseLocked);
    } else if (action == "toggle_physics") {
        std::cout << "[InputIntegration] Physics toggle requested" << std::endl;
        // m_mainWindow->TogglePhysics(); // Would need to add this method
    } else if (action == "toggle_shadows") {
        std::cout << "[InputIntegration] Shadows toggle requested" << std::endl;
        // m_mainWindow->ToggleShadows(); // Would need to add this method
    } else if (action == "debug_light_info") {
        if (m_lightManager) {
            m_lightManager->PrintLightInfo();
        }
    } else if (action == "debug_light_validation") {
        if (m_lightManager) {
            std::cout << "\n[DEBUG] === LIGHT SYSTEM VALIDATION ===" << std::endl;
            m_lightManager->PrintLightInfo();
            m_lightManager->UpdateLightProxies();
            m_lightManager->UpdateGPUBuffers();
            std::cout << "[DEBUG] ================================\n" << std::endl;
        }
    }
}

void InputIntegration::OnGizmoAction(const std::string& action) {
    // CRITICAL FIX: Gizmo actions should work regardless of mouse lock state
    // The user needs to be able to toggle gizmo visibility and change modes
    if (!m_imguiInterface) {
        std::cout << "[InputIntegration] No ImGui interface available for gizmo action" << std::endl;
        return;
    }
    
    if (action == "toggle") {
        bool newState = !m_imguiInterface->IsGizmoVisible();
        m_imguiInterface->SetGizmoVisible(newState);
        std::cout << "[InputIntegration] Gizmo " << (newState ? "enabled" : "disabled") << std::endl;
    } 
    else if (action == "translate") {
        // ImGuizmo::TRANSLATE = 7 (binary 0111)
        m_imguiInterface->SetGizmoOperation(7);
        std::cout << "[InputIntegration] Switched to TRANSLATE mode (operation=7)" << std::endl;
    } 
    else if (action == "rotate") {
        // ImGuizmo::ROTATE = 120 (binary 01111000)  
        m_imguiInterface->SetGizmoOperation(120);
        std::cout << "[InputIntegration] Switched to ROTATE mode (operation=120)" << std::endl;
    } 
    else if (action == "scale") {
        // ImGuizmo::SCALE = 896 (binary 1110000000)
        m_imguiInterface->SetGizmoOperation(896);
        std::cout << "[InputIntegration] Switched to SCALE mode (operation=896)" << std::endl;
    }
}

void InputIntegration::OnLightingAction(const std::string& action) {
    if (action == "toggle_all") {
        if (m_lightManager) {
            if (m_lightManager->GetEnabledLightCount() > 0) {
                m_lightManager->DisableAllLights();
                std::cout << "[InputIntegration] Disabled all lights" << std::endl;
            } else {
                m_lightManager->EnableAllLights();
                std::cout << "[InputIntegration] Enabled all lights" << std::endl;
            }
        }
    } else if (action == "increase_intensity") {
        if (m_directionalLight) {
            float newIntensity = glm::min(10.0f, m_directionalLight->GetIntensity() + 0.1f);
            m_directionalLight->SetIntensity(newIntensity);
            std::cout << "[InputIntegration] Light intensity: " << newIntensity << std::endl;
        }
    } else if (action == "decrease_intensity") {
        if (m_directionalLight) {
            float newIntensity = glm::max(0.0f, m_directionalLight->GetIntensity() - 0.1f);
            m_directionalLight->SetIntensity(newIntensity);
            std::cout << "[InputIntegration] Light intensity: " << newIntensity << std::endl;
        }
    }
}

void InputIntegration::OnAnimationAction(const std::string& action) {
    if (!m_sceneGraph) {
        return;
    }

}

void InputIntegration::OnObjectSelect() {
    if (m_mouseLocked) {
        return; // Don't handle object selection when mouse is locked
    }
    
    std::cout << "[InputIntegration] Object selection requested" << std::endl;
    // The actual mouse picking logic should be handled by MainWindow
    // We could add a callback mechanism here if needed
}

void InputIntegration::UpdateMouseLockState() {
    // This could be used to handle automatic context switching based on mouse lock state
    if (m_mouseLocked) {
        // When mouse is locked, prioritize camera controls
        m_inputManager->SetContextEnabled(Input::Contexts::CAMERA, true);
        m_inputManager->SetContextEnabled("editor", false);
    } else {
        // When mouse is unlocked, enable editor controls
        m_inputManager->SetContextEnabled(Input::Contexts::CAMERA, false);
        m_inputManager->SetContextEnabled("editor", true);
    }
}

void InputIntegration::ApplyLightPreset(int presetNumber) {
    if (!m_directionalLight) {
        return;
    }
    
    glm::vec3 newDir;
    std::string presetName;
    
    switch (presetNumber) {
        case 1: // Sun high preset
            newDir = glm::normalize(glm::vec3(0.2f, -0.8f, -0.3f));
            presetName = "Sun High";
            break;
        case 2: // Sun low preset
            newDir = glm::normalize(glm::vec3(0.5f, -0.3f, -0.5f));
            presetName = "Sun Low";
            break;
        case 3: // Overhead preset
            newDir = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f));
            presetName = "Overhead";
            break;
        case 4: // Side light preset
            newDir = glm::normalize(glm::vec3(1.0f, -0.2f, 0.0f));
            presetName = "Side";
            break;
        case 5: // Back light preset
            newDir = glm::normalize(glm::vec3(0.0f, -0.3f, 1.0f));
            presetName = "Back";
            break;
        default:
            return;
    }
    
    m_directionalLight->SetLightDirection(newDir);
    std::cout << "[InputIntegration] Applied " << presetName << " lighting preset" << std::endl;
}