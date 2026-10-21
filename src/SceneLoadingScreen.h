#pragma once
#include <functional>
#include <string>

// What the loading screen shows. Core fills it every frame while a scene loads or a load has failed.
struct SceneLoadingScreenState {
    std::string sceneName;          // "scene_name" of the scene once parsed, else the file name
    std::string sceneFile;          // logical path as requested
    float progress = 0.0f;          // 0..1, real progress of the load
    std::string stage;              // current step, e.g. "Decoding models (12/120)"
    std::string error;              // non-empty: the load failed and waits for the user (see Dismiss)
    bool startupFailure = false;    // no scene is loaded: the button quits instead of continuing
    bool overlay = false;           // editor: dimmed overlay over the editor UI instead of a full-screen screen
    double timeSeconds = 0.0;       // ImGui time, drives the spinner
};

enum class SceneLoadingScreenAction { None, Dismiss };

// A game may replace the engine's loading screen. The hook is called instead of the default drawing: it draws
// with ImGui inside the open frame and returns Dismiss when the user accepts a failed load.
using SceneLoadingScreenHook = std::function<SceneLoadingScreenAction(const SceneLoadingScreenState&)>;

class SceneLoadingScreen {
public:
    static void SetHook(SceneLoadingScreenHook hook);
    // Draws the loading screen for this frame. Returns Dismiss when the user pressed Continue (or Quit).
    static SceneLoadingScreenAction Draw(const SceneLoadingScreenState& state);
};
