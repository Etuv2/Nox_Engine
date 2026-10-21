#include "InputIntegration.h"
#include "../Core.h"
#include "../Camera.h"
#include "../SceneGraph.h"
#include "../ImGuiInterface.h"
#include "../DirectionalLight.h"
#include "../LightManager.h"
#include <iostream>

InputIntegration::InputIntegration()
    : m_inputManager(std::make_unique<Input::InputManager>())
    , m_core(nullptr)
    , m_imguiInterface(nullptr)
    , m_lightManager(nullptr)
    , m_initialized(false)
    , m_mouseLocked(true)
    , m_cameraSpeed(2.5f)
    , m_mouseSensitivity(0.1f)
    , m_gamepadLookSensitivity(100) {
}

InputIntegration::~InputIntegration() {
    Shutdown();
}

bool InputIntegration::Initialize(Core* core) {
    if (m_initialized) {
        return true;
    }
    
    if (!core) {
        std::cerr << "[InputIntegration] Core pointer is null" << std::endl;
        return false;
    }
    
    m_core = core;
    
    // Initialize the input manager
    if (!m_inputManager->Initialize()) {
        std::cerr << "[InputIntegration] Failed to initialize InputManager" << std::endl;
        return false;
    }
    
    // Set up input contexts and callbacks
    SetupInputContexts();
    ReloadEditorActions();
    
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
    // Editor actions are sampled once per frame; the fly camera only moves while it is active.
    const bool flying = IsFlying();
    if (flying) {
        m_editorActions.Sample(true);
    }
    else {
        m_editorActions.SampleInjected();   // automation input still reaches the map
    }
    if (flying) {
        ApplyEditorFly(deltaTime);
    }
}

void InputIntegration::ReloadEditorActions() {
    std::vector<std::string> errors;
    m_editorActions.LoadFile("config/editor_input.json", errors);
    for (const std::string& error : errors) {
        std::cerr << "[InputIntegration] editor_input.json: " << error << std::endl;
    }
}

bool InputIntegration::IsFlying() const {
    return m_initialized && m_core && m_camera && m_mouseLocked && m_core->IsEditorCameraActive();
}

void InputIntegration::ApplyEditorFly(float deltaTime) {
    if (m_camera) {
        m_camera->ProcessAxes(m_editorActions.Axis("editor_move_forward"), m_editorActions.Axis("editor_move_strafe"),
            m_editorActions.Axis("editor_move_vertical"), deltaTime);
    }
    const float lookX = m_editorActions.Axis("editor_look_x");
    const float lookY = m_editorActions.Axis("editor_look_y");
    if (lookX != 0.0f || lookY != 0.0f) {
        OnCameraLook(lookX * m_gamepadLookSensitivity, -lookY * m_gamepadLookSensitivity);
    }
}

bool InputIntegration::ProcessEvent(const SDL_Event& event) {
    if (!m_initialized || !m_inputManager) {
        return false;
    }
    // Mouse look is relative motion, not a button or axis of the action map, so it is read straight from the event.
    if (event.type == SDL_MOUSEMOTION && IsFlying()) {
        OnCameraLook(static_cast<float>(event.motion.xrel), static_cast<float>(event.motion.yrel));
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

void InputIntegration::OnFocusLost() {
    if (m_inputManager) {
        m_inputManager->ResetHeldState();
    }
    if (m_mouseLocked) {
        SetMouseLocked(false);
    }
}

void InputIntegration::SetupInputContexts() {
    SetupGlobalContext();
    SetupEditorContext();
    SetupLightingContext();
    SetupAnimationContext();
    SetupGamepadContext();
    
    std::cout << "[InputIntegration] Activating default contexts..." << std::endl;
    
    // Activate default contexts
    m_inputManager->PushContext(Input::Contexts::GLOBAL);
    m_inputManager->PushContext("editor");
    m_inputManager->PushContext("lighting");
    m_inputManager->PushContext("animation");
    m_inputManager->PushContext("gamepad"); // Add gamepad context to active stack!
    
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
    if (!m_core) {
        return;
    }
    
    if (action == "exit") {
        // Trigger the SDL_QUIT event
        SDL_Event quitEvent;
        quitEvent.type = SDL_QUIT;
        SDL_PushEvent(&quitEvent);
    } else if (action == "fullscreen") {
        std::cout << "[InputIntegration] Fullscreen toggle requested" << std::endl;
    } else if (action == "wireframe") {
        std::cout << "[InputIntegration] Wireframe toggle requested" << std::endl;
    } else if (action == "mouse_lock") {
        SetMouseLocked(!m_mouseLocked);
    } else if (action == "toggle_physics") {
        std::cout << "[InputIntegration] Physics toggle requested" << std::endl;
        m_core->RequestPhysicsToggle();
    } else if (action == "toggle_shadows") {
        std::cout << "[InputIntegration] Shadows toggle requested" << std::endl;
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
    // Gizmo actions should work regardless of mouse lock state
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
        m_inputManager->SetContextEnabled("editor", false);
    } else {
        // When mouse is unlocked, enable editor controls
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