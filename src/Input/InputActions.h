#pragma once

/**
 * @brief Common input action definitions for the NOX Engine
 * 
 * This file defines standardized action names that can be used throughout
 * the application. This provides consistency and makes it easier to manage
 * input bindings across different systems.
 */

namespace Input {
namespace Actions {

// Camera Controls
constexpr const char* CAMERA_MOVE_FORWARD = "camera_move_forward";
constexpr const char* CAMERA_MOVE_BACKWARD = "camera_move_backward";
constexpr const char* CAMERA_MOVE_LEFT = "camera_move_left";
constexpr const char* CAMERA_MOVE_RIGHT = "camera_move_right";
constexpr const char* CAMERA_MOVE_UP = "camera_move_up";
constexpr const char* CAMERA_MOVE_DOWN = "camera_move_down";
constexpr const char* CAMERA_LOOK = "camera_look";
constexpr const char* CAMERA_ZOOM = "camera_zoom";

// Application Controls
constexpr const char* APP_EXIT = "app_exit";
constexpr const char* APP_FULLSCREEN = "app_fullscreen";
constexpr const char* APP_TOGGLE_WIREFRAME = "app_toggle_wireframe";
constexpr const char* APP_TOGGLE_MOUSE_LOCK = "app_toggle_mouse_lock";

// Scene Controls
constexpr const char* SCENE_RELOAD = "scene_reload";
constexpr const char* SCENE_TOGGLE_PHYSICS = "scene_toggle_physics";

// Object Manipulation (Gizmo)
constexpr const char* GIZMO_TOGGLE = "gizmo_toggle";
constexpr const char* GIZMO_TRANSLATE = "gizmo_translate";
constexpr const char* GIZMO_ROTATE = "gizmo_rotate";
constexpr const char* GIZMO_SCALE = "gizmo_scale";
constexpr const char* OBJECT_SELECT = "object_select";

// Lighting Controls
constexpr const char* LIGHT_TOGGLE_ALL = "light_toggle_all";
constexpr const char* LIGHT_INCREASE_INTENSITY = "light_increase_intensity";
constexpr const char* LIGHT_DECREASE_INTENSITY = "light_decrease_intensity";
constexpr const char* LIGHT_PRESET_1 = "light_preset_1";
constexpr const char* LIGHT_PRESET_2 = "light_preset_2";
constexpr const char* LIGHT_PRESET_3 = "light_preset_3";
constexpr const char* LIGHT_PRESET_4 = "light_preset_4";
constexpr const char* LIGHT_PRESET_5 = "light_preset_5";
constexpr const char* SHADOWS_TOGGLE = "shadows_toggle";

// Animation Controls
constexpr const char* ANIMATION_PLAY = "animation_play";
constexpr const char* ANIMATION_PAUSE = "animation_pause";
constexpr const char* ANIMATION_STOP = "animation_stop";

// Debug Controls
constexpr const char* DEBUG_LIGHT_INFO = "debug_light_info";
constexpr const char* DEBUG_LIGHT_VALIDATION = "debug_light_validation";

// UI Controls
constexpr const char* UI_TOGGLE_MENU = "ui_toggle_menu";
constexpr const char* UI_CONFIRM = "ui_confirm";
constexpr const char* UI_CANCEL = "ui_cancel";

} // namespace Actions

/**
 * @brief Input context names for different application states
 */
namespace Contexts {

constexpr const char* GLOBAL = "global";           // Always active
constexpr const char* CAMERA = "camera";           // Camera movement controls
constexpr const char* EDITOR = "editor";           // Editor/gizmo controls
constexpr const char* MENU = "menu";              // UI menu navigation
constexpr const char* GAME = "game";              // Game-specific controls

} // namespace Contexts

} // namespace Input