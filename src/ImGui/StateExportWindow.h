#pragma once

#include "BaseWindow.h"
#include <functional>
#include <string>
#include <chrono>

/**
 * @brief Window for scene state management and performance data export
 */
class StateExportWindow : public BaseWindow {
public:
    StateExportWindow();

    void Render() override;

    // Callbacks
    void SetSaveSceneStateCallback(const std::function<bool(const std::string&)>& callback) { 
        m_saveSceneStateCallback = callback; 
    }
    void SetLoadSceneStateCallback(const std::function<bool(const std::string&)>& callback) { 
        m_loadSceneStateCallback = callback; 
    }
    void SetQuickSaveCallback(const std::function<bool()>& callback) { 
        m_quickSaveCallback = callback; 
    }
    void SetExportCSVCallback(const std::function<bool(const std::string&)>& callback) { 
        m_exportCSVCallback = callback; 
    }
    void SetExportJSONCallback(const std::function<bool(const std::string&)>& callback) { 
        m_exportJSONCallback = callback; 
    }
    void SetStartRecordingCallback(const std::function<void()>& callback) { 
        m_startRecordingCallback = callback; 
    }
    void SetStopRecordingCallback(const std::function<void()>& callback) { 
        m_stopRecordingCallback = callback; 
    }

    // State
    void SetRecording(bool recording) { m_isRecording = recording; }
    bool IsRecording() const { return m_isRecording; }

private:
    // Scene state management
    std::function<bool(const std::string&)> m_saveSceneStateCallback;
    std::function<bool(const std::string&)> m_loadSceneStateCallback;
    std::function<bool()> m_quickSaveCallback;
    
    // Performance export
    std::function<bool(const std::string&)> m_exportCSVCallback;
    std::function<bool(const std::string&)> m_exportJSONCallback;
    
    // Recording control
    std::function<void()> m_startRecordingCallback;
    std::function<void()> m_stopRecordingCallback;

    // State
    bool m_isRecording = false;
    bool m_autoRecordingEnabled = false;
    std::chrono::high_resolution_clock::time_point m_autoRecordingStartTime;
    static constexpr double AUTO_RECORDING_DURATION_SECONDS = 30.0;
    
    // UI state
    char m_saveStateFilepath[256] = "snapshots/scene_state.json";
    char m_loadStateFilepath[256] = "snapshots/scene_state.json";
    char m_exportCSVFilepath[256] = "performance_data/performance_data.csv";
    char m_exportJSONFilepath[256] = "performance_data/performance_data.json";
    
    // Helper to read state file info
    std::string GetStateFileInfo(const std::string& filepath);
};
