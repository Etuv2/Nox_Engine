#pragma once

#include "BaseWindow.h"
#include <vector>
#include <memory>

// Forward declarations
class SceneGraph;

/**
 * @brief Performance monitoring window showing frame times and system statistics
 */
class PerformanceWindow : public BaseWindow {
public:
    PerformanceWindow();
    
    void Render() override;
    
    // Performance data
    void SetFrameTimeData(const std::vector<float>& data) { m_frameTimeData = data; }
    void SetFPS(float fps) { m_fps = fps; }
    void SetSceneGraph(const std::shared_ptr<SceneGraph>& sceneGraph) { m_sceneGraph = sceneGraph; }

private:
    std::vector<float> m_frameTimeData;
    float m_fps = 60.0f;
    std::shared_ptr<SceneGraph> m_sceneGraph;
};