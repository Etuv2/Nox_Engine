#pragma once

#include "BaseWindow.h"

/**
 * @brief Camera controls window for movement settings and camera information
 */
class CameraWindow : public BaseWindow {
public:
    CameraWindow();
    
    void Render() override;
};