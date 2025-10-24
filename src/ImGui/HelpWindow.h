#pragma once

#include "BaseWindow.h"

/**
 * @brief Help window displaying controls and keybindings
 */
class HelpWindow : public BaseWindow {
public:
    HelpWindow();
    
    void Render() override;

private:
    void RenderControlsSection();
    void RenderKeyBindingsSection();
    void RenderAnimationSection();
    void RenderLightingSection();
};