#include "InputSystem.h"
#include "../json.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>

using json = nlohmann::json;

namespace Input {

// InputContext Implementation
void InputContext::BindKeyAction(const std::string& actionName, SDL_Keycode key, ActionType type) {
    ActionBinding binding;
    binding.actionName = actionName;
    binding.type = type;
    binding.device = InputDevice::KEYBOARD;
    binding.key = key;
    
    // Remove existing binding for this action if it exists
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
            [&](const ActionBinding& b) { 
                return b.actionName == actionName && 
                       b.device == InputDevice::KEYBOARD && 
                       b.key == key && 
                       b.type == type; 
            }),
        m_bindings.end()
    );
    
    m_bindings.push_back(binding);
}

void InputContext::BindMouseAction(const std::string& actionName, MouseButton button, ActionType type) {
    ActionBinding binding;
    binding.actionName = actionName;
    binding.type = type;
    binding.device = InputDevice::MOUSE;
    binding.mouseButton = button;
    
    // Remove existing binding
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
            [&](const ActionBinding& b) { 
                return b.actionName == actionName && 
                       b.device == InputDevice::MOUSE && 
                       b.mouseButton == button && 
                       b.type == type; 
            }),
        m_bindings.end()
    );
    
    m_bindings.push_back(binding);
}

void InputContext::BindGamepadAction(const std::string& actionName, GamepadButton button, ActionType type) {
    ActionBinding binding;
    binding.actionName = actionName;
    binding.type = type;
    binding.device = InputDevice::GAMEPAD;
    binding.gamepadButton = button;
    
    // Remove existing binding
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
            [&](const ActionBinding& b) { 
                return b.actionName == actionName && 
                       b.device == InputDevice::GAMEPAD && 
                       b.gamepadButton == button && 
                       b.type == type; 
            }),
        m_bindings.end()
    );
    
    m_bindings.push_back(binding);
}

void InputContext::BindAxisAction(const std::string& actionName, GamepadAxis axis) {
    ActionBinding binding;
    binding.actionName = actionName;
    binding.type = ActionType::AXIS;
    binding.device = InputDevice::GAMEPAD;
    binding.gamepadAxis = axis;
    
    // Remove existing binding
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
            [&](const ActionBinding& b) { 
                return b.actionName == actionName && 
                       b.device == InputDevice::GAMEPAD && 
                       b.gamepadAxis == axis && 
                       b.type == ActionType::AXIS; 
            }),
        m_bindings.end()
    );
    
    m_bindings.push_back(binding);
}

void InputContext::BindMouseAxisAction(const std::string& actionName) {
    ActionBinding binding;
    binding.actionName = actionName;
    binding.type = ActionType::AXIS;
    binding.device = InputDevice::MOUSE;
    
    // Remove existing mouse axis binding for this action
    m_bindings.erase(
        std::remove_if(m_bindings.begin(), m_bindings.end(),
            [&](const ActionBinding& b) { 
                return b.actionName == actionName && 
                       b.device == InputDevice::MOUSE && 
                       b.type == ActionType::AXIS; 
            }),
        m_bindings.end()
    );
    
    m_bindings.push_back(binding);
}

void InputContext::RegisterActionCallback(const std::string& actionName, std::function<void(float)> callback) {
    m_actionCallbacks[actionName] = callback;
}

void InputContext::RegisterAxisCallback(const std::string& actionName, std::function<void(float, float)> callback) {
    m_axisCallbacks[actionName] = callback;
}

bool InputContext::ProcessKeyEvent(SDL_Keycode key, ActionType type, float deltaTime) {
    if (!m_enabled) return false;
    
    bool handled = false;
    for (const auto& binding : m_bindings) {
        if (binding.device == InputDevice::KEYBOARD && 
            binding.key == key && 
            binding.type == type) {
            
            auto it = m_actionCallbacks.find(binding.actionName);
            if (it != m_actionCallbacks.end()) {
                it->second(type == ActionType::HOLD ? deltaTime : 1.0f);
                handled = true;
            }
        }
    }
    return handled;
}

bool InputContext::ProcessMouseEvent(MouseButton button, ActionType type, float deltaTime) {
    if (!m_enabled) return false;
    
    bool handled = false;
    for (const auto& binding : m_bindings) {
        if (binding.device == InputDevice::MOUSE && 
            binding.mouseButton == button && 
            binding.type == type) {
            
            auto it = m_actionCallbacks.find(binding.actionName);
            if (it != m_actionCallbacks.end()) {
                it->second(type == ActionType::HOLD ? deltaTime : 1.0f);
                handled = true;
            }
        }
    }
    return handled;
}

bool InputContext::ProcessGamepadEvent(GamepadButton button, ActionType type, float deltaTime) {
    if (!m_enabled) return false;
    
    bool handled = false;
    for (const auto& binding : m_bindings) {
        if (binding.device == InputDevice::GAMEPAD && 
            binding.gamepadButton == button && 
            binding.type == type) {
            
            auto it = m_actionCallbacks.find(binding.actionName);
            if (it != m_actionCallbacks.end()) {
                it->second(type == ActionType::HOLD ? deltaTime : 1.0f);
                handled = true;
            }
        }
    }
    return handled;
}

bool InputContext::ProcessMouseAxis(float deltaX, float deltaY) {
    if (!m_enabled) return false;
    
    bool handled = false;
    for (const auto& binding : m_bindings) {
        if (binding.device == InputDevice::MOUSE && 
            binding.type == ActionType::AXIS) {
            
            auto it = m_axisCallbacks.find(binding.actionName);
            if (it != m_axisCallbacks.end()) {
                it->second(deltaX, deltaY);
                handled = true;
            }
        }
    }
    return handled;
}

bool InputContext::ProcessGamepadAxis(GamepadAxis axis, float value) {
    if (!m_enabled) return false;
    
    bool handled = false;
    for (const auto& binding : m_bindings) {
        if (binding.device == InputDevice::GAMEPAD && 
            binding.type == ActionType::AXIS &&
            binding.gamepadAxis == axis) {
            
            auto it = m_axisCallbacks.find(binding.actionName);
            if (it != m_axisCallbacks.end()) {
                // For single axis, pass value as X and 0 as Y
                it->second(value, 0.0f);
                handled = true;
            }
        }
    }
    return handled;
}

// InputManager Implementation
InputManager::InputManager() 
    : m_initialized(false)
    , m_mouseSensitivity(1.0f) {
}

InputManager::~InputManager() {
    Shutdown();
}

bool InputManager::Initialize() {
    if (m_initialized) {
        return true;
    }
    
    // Initialize SDL gamepad subsystem if not already initialized
    if (SDL_WasInit(SDL_INIT_JOYSTICK) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) {
            std::cerr << "[InputManager] Failed to initialize SDL joystick subsystem: "
                      << SDL_GetError() << std::endl;
            return false;
        }
    }
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
            std::cerr << "[InputManager] Failed to initialize SDL gamecontroller subsystem: " 
                      << SDL_GetError() << std::endl;
            return false;
        }
    }

    // Ensure controller events are enabled
    SDL_GameControllerEventState(SDL_ENABLE);
    std::cout << "[InputManager] Controller events enabled" << std::endl;

    InitializeGamepads();
    
    m_initialized = true;
    
    std::cout << "[InputManager] Initialized successfully" << std::endl;
    std::cout << "[InputManager] Active gamepads: " << m_gamepadStates.size() << std::endl;
    for (size_t i = 0; i < m_gamepadStates.size(); ++i) {
        if (m_gamepadStates[i].connected) {
            std::cout << "[InputManager] Gamepad " << i << " connected with instance ID " 
                      << m_gamepadStates[i].instanceId << std::endl;
        }
    }
    
    return true;
}

void InputManager::Shutdown() {
    if (!m_initialized) {
        return;
    }
    
    CleanupGamepads();
    
    m_contexts.clear();
    m_contextStack.clear();
    
    m_initialized = false;
    
    std::cout << "[InputManager] Shutdown complete" << std::endl;
}

std::shared_ptr<InputContext> InputManager::CreateContext(const std::string& name) {
    auto context = std::make_shared<InputContext>(name);
    m_contexts[name] = context;
    
    std::cout << "[InputManager] Created input context: " << name << std::endl;
    return context;
}

void InputManager::PushContext(const std::string& name) {
    auto it = m_contexts.find(name);
    if (it != m_contexts.end()) {
        m_contextStack.push_back(name);
        std::cout << "[InputManager] Pushed context: " << name 
                  << " (stack size: " << m_contextStack.size() << ")" << std::endl;
    } else {
        std::cerr << "[InputManager] Attempted to push non-existent context: " << name << std::endl;
    }
}

void InputManager::PopContext() {
    if (!m_contextStack.empty()) {
        std::string poppedContext = m_contextStack.back();
        m_contextStack.pop_back();
        std::cout << "[InputManager] Popped context: " << poppedContext 
                  << " (stack size: " << m_contextStack.size() << ")" << std::endl;
    } else {
        std::cerr << "[InputManager] Attempted to pop from empty context stack" << std::endl;
    }
}

void InputManager::SetContextEnabled(const std::string& name, bool enabled) {
    auto it = m_contexts.find(name);
    if (it != m_contexts.end()) {
        it->second->SetEnabled(enabled);
        std::cout << "[InputManager] Set context '" << name << "' enabled: " << enabled << std::endl;
    }
}

std::shared_ptr<InputContext> InputManager::GetContext(const std::string& name) {
    auto it = m_contexts.find(name);
    return (it != m_contexts.end()) ? it->second : nullptr;
}

void InputManager::Update(float deltaTime) {
    if (!m_initialized) return;
    
    UpdateKeyStates();
    UpdateMouseStates();
    UpdateGamepadStates();
    
    // Process PRESS/RELEASE actions first (these were set by ProcessEvent in this frame)
    // DON'T process them here - they're already processed in HandleKeyEvent/HandleGamepadButtonEvent!
    
    // Process HOLD actions for currently active contexts
    for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
        auto context = GetContext(*it);
        if (!context || !context->IsEnabled()) continue;
        
        // Check held keys
        for (const auto& keyState : m_keyStates) {
            if (keyState.second.held) {
                context->ProcessKeyEvent(keyState.first, ActionType::HOLD, deltaTime);
            }
        }
        
        // Check held mouse buttons
        for (int i = 0; i < 5; ++i) {
            if (m_mouseState.held[i]) {
                context->ProcessMouseEvent(static_cast<MouseButton>(i), ActionType::HOLD, deltaTime);
            }
        }
        
        // Check held gamepad buttons
        for (size_t gamepadIndex = 0; gamepadIndex < m_gamepadStates.size(); ++gamepadIndex) {
            const auto& gamepad = m_gamepadStates[gamepadIndex];
            if (!gamepad.connected) continue;
            
            for (int i = 0; i <= static_cast<int>(GamepadButton::DPAD_RIGHT); ++i) {
                if (gamepad.buttonHeld[i]) {
                    context->ProcessGamepadEvent(static_cast<GamepadButton>(i), ActionType::HOLD, deltaTime);
                }
            }
            
            // Process gamepad axes continuously during Update, not just on events
            // This ensures smooth movement even with SDL's axis event throttling
            for (int axisIdx = 0; axisIdx <= static_cast<int>(GamepadAxis::TRIGGER_RIGHT); ++axisIdx) {
                float value = gamepad.axisValues[axisIdx];
                if (std::abs(value) > 0.001f) { // Only process non-zero axes
                    GamepadAxis axis = static_cast<GamepadAxis>(axisIdx);
                    context->ProcessGamepadAxis(axis, value);
                }
            }
        }
    }
    
    // Clear frame states at the VERY END after all processing
    ClearFrameStates();
}

bool InputManager::ProcessEvent(const SDL_Event& event) {
    if (!m_initialized) return false;
    
    bool handled = false;
    
    switch (event.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            HandleKeyEvent(event.key);
            handled = true;
            break;
            
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            HandleMouseButtonEvent(event.button);
            handled = true;
            break;
            
        case SDL_MOUSEMOTION:
            HandleMouseMotionEvent(event.motion);
            handled = true;
            break;
            
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            HandleGamepadButtonEvent(event.cbutton);
            handled = true;
            break;
            
        case SDL_CONTROLLERAXISMOTION:
            HandleGamepadAxisEvent(event.caxis);
            handled = true;
            break;
            
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERDEVICEREMAPPED:
            HandleGamepadDeviceEvent(event.cdevice);
            handled = true;
            break;
    }
    
    return handled;
}

bool InputManager::IsKeyPressed(SDL_Keycode key) const {
    auto it = m_keyStates.find(key);
    return (it != m_keyStates.end()) ? it->second.pressed : false;
}

bool InputManager::IsKeyHeld(SDL_Keycode key) const {
    auto it = m_keyStates.find(key);
    return (it != m_keyStates.end()) ? it->second.held : false;
}

bool InputManager::IsKeyReleased(SDL_Keycode key) const {
    auto it = m_keyStates.find(key);
    return (it != m_keyStates.end()) ? it->second.released : false;
}

bool InputManager::IsMouseButtonPressed(MouseButton button) const {
    int index = static_cast<int>(button);
    return (index >= 0 && index < 5) ? m_mouseState.pressed[index] : false;
}

bool InputManager::IsMouseButtonHeld(MouseButton button) const {
    int index = static_cast<int>(button);
    return (index >= 0 && index < 5) ? m_mouseState.held[index] : false;
}

bool InputManager::IsMouseButtonReleased(MouseButton button) const {
    int index = static_cast<int>(button);
    return (index >= 0 && index < 5) ? m_mouseState.released[index] : false;
}

void InputManager::GetMousePosition(int& x, int& y) const {
    x = m_mouseState.x;
    y = m_mouseState.y;
}

void InputManager::GetMouseDelta(float& deltaX, float& deltaY) const {
    deltaX = m_mouseState.deltaX;
    deltaY = m_mouseState.deltaY;
}

bool InputManager::IsGamepadConnected(int gamepadIndex) const {
    return (gamepadIndex >= 0 && gamepadIndex < static_cast<int>(m_gamepadStates.size())) 
           ? m_gamepadStates[gamepadIndex].connected : false;
}

bool InputManager::IsGamepadButtonPressed(GamepadButton button, int gamepadIndex) const {
    if (gamepadIndex < 0 || gamepadIndex >= static_cast<int>(m_gamepadStates.size())) return false;
    if (!m_gamepadStates[gamepadIndex].connected) return false;
    
    int buttonIndex = static_cast<int>(button);
    return (buttonIndex >= 0 && buttonIndex <= static_cast<int>(GamepadButton::DPAD_RIGHT)) 
           ? m_gamepadStates[gamepadIndex].buttonPressed[buttonIndex] : false;
}

bool InputManager::IsGamepadButtonHeld(GamepadButton button, int gamepadIndex) const {
    if (gamepadIndex < 0 || gamepadIndex >= static_cast<int>(m_gamepadStates.size())) return false;
    if (!m_gamepadStates[gamepadIndex].connected) return false;
    
    int buttonIndex = static_cast<int>(button);
    return (buttonIndex >= 0 && buttonIndex <= static_cast<int>(GamepadButton::DPAD_RIGHT)) 
           ? m_gamepadStates[gamepadIndex].buttonHeld[buttonIndex] : false;
}

float InputManager::GetGamepadAxis(GamepadAxis axis, int gamepadIndex) const {
    if (gamepadIndex < 0 || gamepadIndex >= static_cast<int>(m_gamepadStates.size())) return 0.0f;
    if (!m_gamepadStates[gamepadIndex].connected) return 0.0f;
    
    int axisIndex = static_cast<int>(axis);
    return (axisIndex >= 0 && axisIndex <= static_cast<int>(GamepadAxis::TRIGGER_RIGHT)) 
           ? m_gamepadStates[gamepadIndex].axisValues[axisIndex] : 0.0f;
}

// Private Methods Implementation

void InputManager::UpdateKeyStates() {
    // SDL automatically handles key repeat and state tracking
    // Our state is updated through events
}

void InputManager::UpdateMouseStates() {
    // Get current mouse position
    SDL_GetMouseState(&m_mouseState.x, &m_mouseState.y);
}

void InputManager::UpdateGamepadStates() {
    // Gamepad states are updated through events
    // We could add periodic polling here if needed
}

void InputManager::ClearFrameStates() {
    // Clear pressed/released states for next frame
    for (auto& keyState : m_keyStates) {
        keyState.second.pressed = false;
        keyState.second.released = false;
    }
    
    for (int i = 0; i < 5; ++i) {
        m_mouseState.pressed[i] = false;
        m_mouseState.released[i] = false;
    }
    
    // Clear mouse delta
    m_mouseState.deltaX = 0.0f;
    m_mouseState.deltaY = 0.0f;
    
    for (auto& gamepad : m_gamepadStates) {
        for (int i = 0; i <= static_cast<int>(GamepadButton::DPAD_RIGHT); ++i) {
            gamepad.buttonPressed[i] = false;
            gamepad.buttonReleased[i] = false;
        }
    }
}

void InputManager::HandleKeyEvent(const SDL_KeyboardEvent& keyEvent) {
    SDL_Keycode key = keyEvent.keysym.sym;
    bool isPressed = (keyEvent.state == SDL_PRESSED);
    
    // Debug logging for key events
    static int keyEventCount = 0;
    if (keyEventCount < 5) { // Only log first few events to avoid spam
        std::cout << "[InputManager] Key event: " << SDL_GetKeyName(key) 
                  << " " << (isPressed ? "PRESSED" : "RELEASED") << std::endl;
        keyEventCount++;
    }
    
    auto& keyState = m_keyStates[key];
    
    if (isPressed && !keyState.held) {
        keyState.pressed = true;
        keyState.held = true;
        
        // Process PRESS action through active contexts
        for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
            auto context = GetContext(*it);
            if (context && context->IsEnabled()) {
                if (context->ProcessKeyEvent(key, ActionType::PRESS, 0.0f)) {
                    break; // Stop if context handled the event
                }
            }
        }
    } else if (!isPressed && keyState.held) {
        keyState.released = true;
        keyState.held = false;
        
        // Process RELEASE action through active contexts
        for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
            auto context = GetContext(*it);
            if (context && context->IsEnabled()) {
                if (context->ProcessKeyEvent(key, ActionType::RELEASE, 0.0f)) {
                    break; // Stop if context handled the event
                }
            }
        }
    }
}

void InputManager::HandleMouseButtonEvent(const SDL_MouseButtonEvent& mouseEvent) {
    int buttonIndex = -1;
    
    switch (mouseEvent.button) {
        case SDL_BUTTON_LEFT:   buttonIndex = 0; break;
        case SDL_BUTTON_RIGHT:  buttonIndex = 1; break;
        case SDL_BUTTON_MIDDLE: buttonIndex = 2; break;
        case SDL_BUTTON_X1:     buttonIndex = 3; break;
        case SDL_BUTTON_X2:     buttonIndex = 4; break;
        default: return; // Unknown button
    }
    
    bool isPressed = (mouseEvent.state == SDL_PRESSED);
    MouseButton button = static_cast<MouseButton>(buttonIndex);
    
    if (isPressed && !m_mouseState.held[buttonIndex]) {
        m_mouseState.pressed[buttonIndex] = true;
        m_mouseState.held[buttonIndex] = true;
        
        // Process PRESS action through active contexts
        for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
            auto context = GetContext(*it);
            if (context && context->IsEnabled()) {
                if (context->ProcessMouseEvent(button, ActionType::PRESS, 0.0f)) {
                    break;
                }
            }
        }
    } else if (!isPressed && m_mouseState.held[buttonIndex]) {
        m_mouseState.released[buttonIndex] = true;
        m_mouseState.held[buttonIndex] = false;
        
        // Process RELEASE action through active contexts
        for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
            auto context = GetContext(*it);
            if (context && context->IsEnabled()) {
                if (context->ProcessMouseEvent(button, ActionType::RELEASE, 0.0f)) {
                    break;
                }
            }
        }
    }
}

void InputManager::HandleMouseMotionEvent(const SDL_MouseMotionEvent& motionEvent) {
    m_mouseState.deltaX = static_cast<float>(motionEvent.xrel) * m_mouseSensitivity;
    m_mouseState.deltaY = static_cast<float>(motionEvent.yrel) * m_mouseSensitivity;
    
    // Process mouse axis through active contexts
    for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
        auto context = GetContext(*it);
        if (context && context->IsEnabled()) {
            if (context->ProcessMouseAxis(m_mouseState.deltaX, m_mouseState.deltaY)) {
                break;
            }
        }
    }
}

void InputManager::HandleGamepadButtonEvent(const SDL_ControllerButtonEvent& buttonEvent) {
    int gamepadIndex = GetGamepadIndexByInstanceId(buttonEvent.which);
    
    std::cout << "[InputManager] Gamepad button event: instance ID=" << buttonEvent.which 
              << ", mapped index=" << gamepadIndex 
              << ", button=" << static_cast<int>(buttonEvent.button)
              << ", state=" << (buttonEvent.state == SDL_PRESSED ? "PRESSED" : "RELEASED") << std::endl;
    
    if (gamepadIndex < 0 || gamepadIndex >= static_cast<int>(m_gamepadStates.size())) {
        std::cerr << "[InputManager] Invalid gamepad index " << gamepadIndex 
                  << " for instance ID " << buttonEvent.which << std::endl;
        return;
    }
    
    if (!m_gamepadStates[gamepadIndex].connected) {
        std::cerr << "[InputManager] Gamepad at index " << gamepadIndex << " not connected" << std::endl;
        return;
    }
    
    // Map SDL button to our GamepadButton enum
    GamepadButton button;
    switch (buttonEvent.button) {
        case SDL_CONTROLLER_BUTTON_A: button = GamepadButton::A; break;
        case SDL_CONTROLLER_BUTTON_B: button = GamepadButton::B; break;
        case SDL_CONTROLLER_BUTTON_X: button = GamepadButton::X; break;
        case SDL_CONTROLLER_BUTTON_Y: button = GamepadButton::Y; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: button = GamepadButton::LEFT_SHOULDER; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: button = GamepadButton::RIGHT_SHOULDER; break;
        case SDL_CONTROLLER_BUTTON_BACK: button = GamepadButton::BACK; break;
        case SDL_CONTROLLER_BUTTON_START: button = GamepadButton::START; break;
        case SDL_CONTROLLER_BUTTON_GUIDE: button = GamepadButton::GUIDE; break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: button = GamepadButton::LEFT_STICK; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: button = GamepadButton::RIGHT_STICK; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: button = GamepadButton::DPAD_UP; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: button = GamepadButton::DPAD_DOWN; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: button = GamepadButton::DPAD_LEFT; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: button = GamepadButton::DPAD_RIGHT; break;
        default: 
            std::cerr << "[InputManager] Unknown gamepad button: " << buttonEvent.button << std::endl;
            return;
    }
    
    int buttonIndex = static_cast<int>(button);
    bool isPressed = (buttonEvent.state == SDL_PRESSED);
    auto& gamepad = m_gamepadStates[gamepadIndex];
    
    if (isPressed && !gamepad.buttonHeld[buttonIndex]) {
        gamepad.buttonPressed[buttonIndex] = true;
        gamepad.buttonHeld[buttonIndex] = true;
        
        std::cout << "[InputManager] Processing gamepad PRESS for button " << buttonIndex << std::endl;
        
        // Process PRESS action through active contexts
        for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
            auto context = GetContext(*it);
            if (context && context->IsEnabled()) {
                if (context->ProcessGamepadEvent(button, ActionType::PRESS, 0.0f)) {
                    break;
                }
            }
        }
    } else if (!isPressed && gamepad.buttonHeld[buttonIndex]) {
        gamepad.buttonReleased[buttonIndex] = true;
        gamepad.buttonHeld[buttonIndex] = false;
        
        std::cout << "[InputManager] Processing gamepad RELEASE for button " << buttonIndex << std::endl;
        
        // Process RELEASE action through active contexts
        for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
            auto context = GetContext(*it);
            if (context && context->IsEnabled()) {
                if (context->ProcessGamepadEvent(button, ActionType::RELEASE, 0.0f)) {
                    break;
                }
            }
        }
    }
}

void InputManager::HandleGamepadAxisEvent(const SDL_ControllerAxisEvent& axisEvent) {
    int gamepadIndex = GetGamepadIndexByInstanceId(axisEvent.which);
    
    // Only log axis events occasionally to avoid spam
    static int axisEventCount = 0;
    if (axisEventCount++ % 100 == 0) {
        std::cout << "[InputManager] Gamepad axis event: instance ID=" << axisEvent.which 
                  << ", mapped index=" << gamepadIndex 
                  << ", axis=" << static_cast<int>(axisEvent.axis)
                  << ", value=" << axisEvent.value << std::endl;
    }
    
    if (gamepadIndex < 0 || gamepadIndex >= static_cast<int>(m_gamepadStates.size())) return;
    if (!m_gamepadStates[gamepadIndex].connected) return;
    
    // Map SDL axis to our GamepadAxis enum
    GamepadAxis axis;
    switch (axisEvent.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: axis = GamepadAxis::LEFT_X; break;
        case SDL_CONTROLLER_AXIS_LEFTY: axis = GamepadAxis::LEFT_Y; break;
        case SDL_CONTROLLER_AXIS_RIGHTX: axis = GamepadAxis::RIGHT_X; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: axis = GamepadAxis::RIGHT_Y; break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: axis = GamepadAxis::TRIGGER_LEFT; break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: axis = GamepadAxis::TRIGGER_RIGHT; break;
        default: return; // Unknown axis
    }
    
    int axisIndex = static_cast<int>(axis);
    float value = static_cast<float>(axisEvent.value) / 32767.0f; // Normalize to [-1, 1] or [0, 1] for triggers
    
    // Apply deadzone for sticks
    if (axis == GamepadAxis::LEFT_X || axis == GamepadAxis::LEFT_Y ||
        axis == GamepadAxis::RIGHT_X || axis == GamepadAxis::RIGHT_Y) {
        const float deadzone = 0.1f;
        if (std::abs(value) < deadzone) {
            value = 0.0f;
        }
    }
    
    m_gamepadStates[gamepadIndex].axisValues[axisIndex] = value;
    
    // Process axis input through active contexts
    for (auto it = m_contextStack.rbegin(); it != m_contextStack.rend(); ++it) {
        auto context = GetContext(*it);
        if (context && context->IsEnabled()) {
            if (context->ProcessGamepadAxis(axis, value)) {
                break;
            }
        }
    }
}

void InputManager::HandleGamepadDeviceEvent(const SDL_ControllerDeviceEvent& deviceEvent) {
    switch (deviceEvent.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            // which is the device index
            const int deviceIndex = deviceEvent.which;
            std::cout << "[InputManager] Controller device ADDED at device index " << deviceIndex << std::endl;
            
            if (SDL_IsGameController(deviceIndex)) {
                SDL_GameController* controller = SDL_GameControllerOpen(deviceIndex);
                if (controller) {
                    SDL_Joystick* js = SDL_GameControllerGetJoystick(controller);
                    SDL_JoystickID instanceId = SDL_JoystickInstanceID(js);

                    int freeIndex = -1;
                    for (int i = 0; i < static_cast<int>(m_gamepadStates.size()); ++i) {
                        if (!m_gamepadStates[i].connected) { freeIndex = i; break; }
                    }
                    if (freeIndex == -1) {
                        m_gamepadStates.emplace_back();
                        freeIndex = static_cast<int>(m_gamepadStates.size()) - 1;
                    }

                    auto& gp = m_gamepadStates[freeIndex];
                    gp.controller = controller;
                    gp.connected = true;
                    gp.instanceId = instanceId;
                    std::fill(std::begin(gp.buttonPressed), std::end(gp.buttonPressed), false);
                    std::fill(std::begin(gp.buttonHeld), std::end(gp.buttonHeld), false);
                    std::fill(std::begin(gp.buttonReleased), std::end(gp.buttonReleased), false);
                    std::fill(std::begin(gp.axisValues), std::end(gp.axisValues), 0.0f);

                    m_gamepadIdToIndex[instanceId] = freeIndex;

                    const char* name = SDL_GameControllerName(controller);
                    std::cout << "[InputManager] *** Gamepad connected (idx " << freeIndex << ") instance "
                              << instanceId << ": " << (name ? name : "Unknown") << " ***" << std::endl;
                } else {
                    std::cerr << "[InputManager] Failed to open gamepad device index " << deviceIndex 
                              << ": " << SDL_GetError() << std::endl;
                }
            } else {
                std::cout << "[InputManager] Device at index " << deviceIndex << " is not a game controller" << std::endl;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            SDL_JoystickID instanceId = deviceEvent.which; // instance id
            std::cout << "[InputManager] Controller device REMOVED with instance ID " << instanceId << std::endl;
            
            auto itMap = m_gamepadIdToIndex.find(instanceId);
            if (itMap != m_gamepadIdToIndex.end()) {
                int idx = itMap->second;
                if (idx >= 0 && idx < static_cast<int>(m_gamepadStates.size())) {
                    auto& gamepad = m_gamepadStates[idx];
                    std::cout << "[InputManager] *** Gamepad disconnected (idx " << idx << ") instance "
                              << instanceId << " ***" << std::endl;
                    if (gamepad.controller) {
                        SDL_GameControllerClose(gamepad.controller);
                        gamepad.controller = nullptr;
                    }
                    gamepad.connected = false;
                    gamepad.instanceId = -1;
                    std::fill(std::begin(gamepad.buttonPressed), std::end(gamepad.buttonPressed), false);
                    std::fill(std::begin(gamepad.buttonHeld), std::end(gamepad.buttonHeld), false);
                    std::fill(std::begin(gamepad.buttonReleased), std::end(gamepad.buttonReleased), false);
                    std::fill(std::begin(gamepad.axisValues), std::end(gamepad.axisValues), 0.0f);
                }
                m_gamepadIdToIndex.erase(itMap);
            } else {
                std::cout << "[InputManager] Gamepad disconnected with unknown instance id: " << instanceId << std::endl;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEREMAPPED: {
            std::cout << "[InputManager] Gamepad remapped: instance " << deviceEvent.which << std::endl;
            break;
        }
    }
}

void InputManager::InitializeGamepads() {
    // Clean up existing gamepads first
    CleanupGamepads();

    m_gamepadIdToIndex.clear();
    m_gamepadStates.clear();
    
    int numJoysticks = SDL_NumJoysticks();
    std::cout << "[InputManager] === Initializing Gamepads ===" << std::endl;
    std::cout << "[InputManager] Found " << numJoysticks << " joystick device(s)" << std::endl;
    
    for (int deviceIndex = 0; deviceIndex < numJoysticks; ++deviceIndex) {
        std::cout << "[InputManager] Checking device " << deviceIndex << "..." << std::endl;
        
        if (SDL_IsGameController(deviceIndex)) {
            std::cout << "[InputManager] Device " << deviceIndex << " IS a game controller" << std::endl;
            
            SDL_GameController* controller = SDL_GameControllerOpen(deviceIndex);
            if (controller) {
                SDL_Joystick* js = SDL_GameControllerGetJoystick(controller);
                SDL_JoystickID instanceId = SDL_JoystickInstanceID(js);

                GamepadState state;
                state.controller = controller;
                state.connected = true;
                state.instanceId = instanceId;
                std::fill(std::begin(state.buttonPressed), std::end(state.buttonPressed), false);
                std::fill(std::begin(state.buttonHeld), std::end(state.buttonHeld), false);
                std::fill(std::begin(state.buttonReleased), std::end(state.buttonReleased), false);
                std::fill(std::begin(state.axisValues), std::end(state.axisValues), 0.0f);

                m_gamepadStates.push_back(state);
                m_gamepadIdToIndex[instanceId] = static_cast<int>(m_gamepadStates.size()) - 1;
                
                const char* name = SDL_GameControllerName(controller);
                std::cout << "[InputManager] *** Successfully initialized gamepad idx " 
                          << (m_gamepadStates.size() - 1) << ": " 
                          << (name ? name : "Unknown")
                          << " (instance ID " << instanceId << ") ***" << std::endl;
            } else {
                std::cerr << "[InputManager] Failed to open gamepad " << deviceIndex << ": " 
                          << SDL_GetError() << std::endl;
            }
        } else {
            std::cout << "[InputManager] Device " << deviceIndex << " is NOT a game controller" << std::endl;
        }
    }
    
    std::cout << "[InputManager] === Gamepad Initialization Complete ===" << std::endl;
    std::cout << "[InputManager] Total controllers connected: " << m_gamepadStates.size() << std::endl;
}

// Configuration Loading/Saving (TODO: Implement JSON serialization)
bool InputManager::LoadConfiguration(const std::string& configPath) {
    m_configPath = configPath;
    
    try {
        std::ifstream file(configPath);
        if (!file.is_open()) {
            std::cerr << "[InputManager] Could not open input config file: " << configPath << std::endl;
            return false;
        }
        
        json config;
        file >> config;
        
        // TODO: Parse and load input bindings from JSON
        // This would involve parsing contexts and their bindings
        
        std::cout << "[InputManager] Loaded configuration from: " << configPath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[InputManager] Error loading configuration: " << e.what() << std::endl;
        return false;
    }
}

bool InputManager::SaveConfiguration(const std::string& configPath) const {
    try {
        json config;
        
        // TODO: Serialize input bindings to JSON
        // This would involve saving all contexts and their bindings
        
        std::ofstream file(configPath);
        if (!file.is_open()) {
            std::cerr << "[InputManager] Could not create input config file: " << configPath << std::endl;
            return false;
        }
        
        file << config.dump(4);
        
        std::cout << "[InputManager] Saved configuration to: " << configPath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[InputManager] Error saving configuration: " << e.what() << std::endl;
        return false;
    }
}

void InputManager::CleanupGamepads() {
    for (auto& gamepad : m_gamepadStates) {
        if (gamepad.controller) {
            SDL_GameControllerClose(gamepad.controller);
            gamepad.controller = nullptr;
        }
        gamepad.connected = false;
        gamepad.instanceId = -1;
    }
    m_gamepadIdToIndex.clear();
}

SDL_GameControllerButton InputManager::MapGamepadButton(GamepadButton button) const {
    switch (button) {
        case GamepadButton::A: return SDL_CONTROLLER_BUTTON_A;
        case GamepadButton::B: return SDL_CONTROLLER_BUTTON_B;
        case GamepadButton::X: return SDL_CONTROLLER_BUTTON_X;
        case GamepadButton::Y: return SDL_CONTROLLER_BUTTON_Y;
        case GamepadButton::LEFT_SHOULDER: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
        case GamepadButton::RIGHT_SHOULDER: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
        case GamepadButton::BACK: return SDL_CONTROLLER_BUTTON_BACK;
        case GamepadButton::START: return SDL_CONTROLLER_BUTTON_START;
        case GamepadButton::GUIDE: return SDL_CONTROLLER_BUTTON_GUIDE;
        case GamepadButton::LEFT_STICK: return SDL_CONTROLLER_BUTTON_LEFTSTICK;
        case GamepadButton::RIGHT_STICK: return SDL_CONTROLLER_BUTTON_RIGHTSTICK;
        case GamepadButton::DPAD_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
        case GamepadButton::DPAD_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        case GamepadButton::DPAD_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        case GamepadButton::DPAD_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        default: return SDL_CONTROLLER_BUTTON_INVALID;
    }
}

SDL_GameControllerAxis InputManager::MapGamepadAxis(GamepadAxis axis) const {
    switch (axis) {
        case GamepadAxis::LEFT_X: return SDL_CONTROLLER_AXIS_LEFTX;
        case GamepadAxis::LEFT_Y: return SDL_CONTROLLER_AXIS_LEFTY;
        case GamepadAxis::RIGHT_X: return SDL_CONTROLLER_AXIS_RIGHTX;
        case GamepadAxis::RIGHT_Y: return SDL_CONTROLLER_AXIS_RIGHTY;
        case GamepadAxis::TRIGGER_LEFT: return SDL_CONTROLLER_AXIS_TRIGGERLEFT;
        case GamepadAxis::TRIGGER_RIGHT: return SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        default: return SDL_CONTROLLER_AXIS_INVALID;
    }
}

int InputManager::GetGamepadIndexByInstanceId(SDL_JoystickID id) const {
    auto it = m_gamepadIdToIndex.find(id);
    if (it != m_gamepadIdToIndex.end()) return it->second;
    return -1;
}

// Static utility functions
std::string InputManager::KeycodeToString(SDL_Keycode key) {
    const char* name = SDL_GetKeyName(key);
    return name ? std::string(name) : "Unknown";
}

SDL_Keycode InputManager::StringToKeycode(const std::string& keyString) {
    return SDL_GetKeyFromName(keyString.c_str());
}

std::string InputManager::MouseButtonToString(MouseButton button) {
    switch (button) {
        case MouseButton::LEFT: return "Left";
        case MouseButton::RIGHT: return "Right";
        case MouseButton::MIDDLE: return "Middle";
        case MouseButton::X1: return "X1";
        case MouseButton::X2: return "X2";
        default: return "Unknown";
    }
}

MouseButton InputManager::StringToMouseButton(const std::string& buttonString) {
    if (buttonString == "Left") return MouseButton::LEFT;
    if (buttonString == "Right") return MouseButton::RIGHT;
    if (buttonString == "Middle") return MouseButton::MIDDLE;
    if (buttonString == "X1") return MouseButton::X1;
    if (buttonString == "X2") return MouseButton::X2;
    return MouseButton::LEFT; // Default
}

} // namespace Input