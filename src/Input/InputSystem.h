#pragma once

#include <SDL/SDL.h>
#include <functional>
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>

/**
 * @brief Unified Input System for SDL2-based applications
 * 
 * This system provides a high-level abstraction over SDL2 input handling,
 * allowing for easy configuration of key bindings, mouse handling, and
 * event callbacks without hardcoding input logic in the main application.
 * 
 * Features:
 * - Configurable key bindings via JSON
 * - Action-based input system
 * - Mouse and gamepad support
 * - Event callbacks for custom handling
 * - State tracking for held keys/buttons
 * - Delta time integration for movement
 */

namespace Input {

// Forward declarations
class InputManager;
class ActionBinding;

/**
 * @brief Input action types
 */
enum class ActionType {
    PRESS,      // Triggered once when key is pressed
    RELEASE,    // Triggered once when key is released
    HOLD,       // Triggered continuously while key is held
    AXIS        // Analog input (mouse movement, gamepad sticks)
};

/**
 * @brief Input device types
 */
enum class InputDevice {
    KEYBOARD,
    MOUSE,
    GAMEPAD
};

/**
 * @brief Mouse button enumeration
 */
enum class MouseButton {
    LEFT = 0,
    RIGHT = 1,
    MIDDLE = 2,
    X1 = 3,
    X2 = 4
};

/**
 * @brief Gamepad button enumeration (mapped to SDL controller buttons)
 */
enum class GamepadButton {
    A, B, X, Y,
    LEFT_SHOULDER, RIGHT_SHOULDER,
    BACK, START, GUIDE,
    LEFT_STICK, RIGHT_STICK,
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT
};

/**
 * @brief Gamepad axis enumeration
 */
enum class GamepadAxis {
    LEFT_X, LEFT_Y,
    RIGHT_X, RIGHT_Y,
    TRIGGER_LEFT, TRIGGER_RIGHT
};

/**
 * @brief Input context for different application states
 * Allows different key bindings for different modes (e.g., menu vs gameplay)
 */
class InputContext {
public:
    InputContext(const std::string& name) : m_name(name), m_enabled(true) {}
    
    const std::string& GetName() const { return m_name; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }
    
    // Action binding methods
    void BindKeyAction(const std::string& actionName, SDL_Keycode key, ActionType type = ActionType::PRESS);
    void BindMouseAction(const std::string& actionName, MouseButton button, ActionType type = ActionType::PRESS);
    void BindGamepadAction(const std::string& actionName, GamepadButton button, ActionType type = ActionType::PRESS);
    void BindAxisAction(const std::string& actionName, GamepadAxis axis);
    void BindMouseAxisAction(const std::string& actionName); // For mouse movement
    
    // Callback registration
    void RegisterActionCallback(const std::string& actionName, std::function<void(float)> callback);
    void RegisterAxisCallback(const std::string& actionName, std::function<void(float, float)> callback);
    
    // Internal methods used by InputManager
    bool ProcessKeyEvent(SDL_Keycode key, ActionType type, float deltaTime);
    bool ProcessMouseEvent(MouseButton button, ActionType type, float deltaTime);
    bool ProcessGamepadEvent(GamepadButton button, ActionType type, float deltaTime);
    bool ProcessMouseAxis(float deltaX, float deltaY);
    bool ProcessGamepadAxis(GamepadAxis axis, float value);

private:
    struct ActionBinding {
        std::string actionName;
        ActionType type;
        InputDevice device;
        union {
            SDL_Keycode key;
            MouseButton mouseButton;
            GamepadButton gamepadButton;
            GamepadAxis gamepadAxis;
        };
        std::function<void(float)> callback;
        std::function<void(float, float)> axisCallback;
        
        ActionBinding() : key(SDLK_UNKNOWN) {}
    };
    
    std::string m_name;
    bool m_enabled;
    std::vector<ActionBinding> m_bindings;
    std::unordered_map<std::string, std::function<void(float)>> m_actionCallbacks;
    std::unordered_map<std::string, std::function<void(float, float)>> m_axisCallbacks;
};

/**
 * @brief Main Input Manager class
 * 
 * Handles SDL2 events and dispatches them to the appropriate input contexts.
 * Manages input state and provides utility functions for input queries.
 */
class InputManager {
public:
    InputManager();
    ~InputManager();
    
    // Initialization and cleanup
    bool Initialize();
    void Shutdown();
    
    // Context management
    std::shared_ptr<InputContext> CreateContext(const std::string& name);
    void PushContext(const std::string& name);
    void PopContext();
    void SetContextEnabled(const std::string& name, bool enabled);
    std::shared_ptr<InputContext> GetContext(const std::string& name);
    
    // Main update function - call this every frame
    void Update(float deltaTime);
    
    // Event processing - call this with SDL events
    bool ProcessEvent(const SDL_Event& event);
    
    // Direct input queries (for immediate input checking)
    bool IsKeyPressed(SDL_Keycode key) const;
    bool IsKeyHeld(SDL_Keycode key) const;
    bool IsKeyReleased(SDL_Keycode key) const;
    
    bool IsMouseButtonPressed(MouseButton button) const;
    bool IsMouseButtonHeld(MouseButton button) const;
    bool IsMouseButtonReleased(MouseButton button) const;
    
    // Mouse state
    void GetMousePosition(int& x, int& y) const;
    void GetMouseDelta(float& deltaX, float& deltaY) const;
    void SetMouseSensitivity(float sensitivity) { m_mouseSensitivity = sensitivity; }
    float GetMouseSensitivity() const { return m_mouseSensitivity; }
    
    // Gamepad support
    bool IsGamepadConnected(int gamepadIndex = 0) const;
    bool IsGamepadButtonPressed(GamepadButton button, int gamepadIndex = 0) const;
    bool IsGamepadButtonHeld(GamepadButton button, int gamepadIndex = 0) const;
    float GetGamepadAxis(GamepadAxis axis, int gamepadIndex = 0) const;
    
    // Configuration
    bool LoadConfiguration(const std::string& configPath);
    bool SaveConfiguration(const std::string& configPath) const;
    
    // Utility functions
    static std::string KeycodeToString(SDL_Keycode key);
    static SDL_Keycode StringToKeycode(const std::string& keyString);
    static std::string MouseButtonToString(MouseButton button);
    static MouseButton StringToMouseButton(const std::string& buttonString);
    
private:
    // Internal state tracking
    struct KeyState {
        bool pressed = false;
        bool held = false;
        bool released = false;
    };
    
    struct MouseState {
        bool pressed[5] = {false}; // Support for 5 mouse buttons
        bool held[5] = {false};
        bool released[5] = {false};
        int x = 0, y = 0;
        float deltaX = 0.0f, deltaY = 0.0f;
        float sensitivity = 1.0f;
    };
    
    struct GamepadState {
        SDL_GameController* controller = nullptr;
        bool connected = false;
        bool buttonPressed[static_cast<int>(GamepadButton::DPAD_RIGHT) + 1] = {false};
        bool buttonHeld[static_cast<int>(GamepadButton::DPAD_RIGHT) + 1] = {false};
        bool buttonReleased[static_cast<int>(GamepadButton::DPAD_RIGHT) + 1] = {false};
        float axisValues[static_cast<int>(GamepadAxis::TRIGGER_RIGHT) + 1] = {0.0f};
    };
    
    // State management
    void UpdateKeyStates();
    void UpdateMouseStates();
    void UpdateGamepadStates();
    void ClearFrameStates(); // Clear pressed/released states after processing
    
    // Event handlers
    void HandleKeyEvent(const SDL_KeyboardEvent& keyEvent);
    void HandleMouseButtonEvent(const SDL_MouseButtonEvent& mouseEvent);
    void HandleMouseMotionEvent(const SDL_MouseMotionEvent& motionEvent);
    void HandleGamepadButtonEvent(const SDL_ControllerButtonEvent& buttonEvent);
    void HandleGamepadAxisEvent(const SDL_ControllerAxisEvent& axisEvent);
    void HandleGamepadDeviceEvent(const SDL_ControllerDeviceEvent& deviceEvent);
    
    // Gamepad management
    void InitializeGamepads();
    void CleanupGamepads();
    SDL_GameControllerButton MapGamepadButton(GamepadButton button) const;
    SDL_GameControllerAxis MapGamepadAxis(GamepadAxis axis) const;
    
    // Member variables
    bool m_initialized;
    float m_mouseSensitivity;
    
    // Input state
    std::unordered_map<SDL_Keycode, KeyState> m_keyStates;
    MouseState m_mouseState;
    std::vector<GamepadState> m_gamepadStates;
    
    // Context management
    std::unordered_map<std::string, std::shared_ptr<InputContext>> m_contexts;
    std::vector<std::string> m_contextStack;
    
    // Configuration
    std::string m_configPath;
};

} // namespace Input