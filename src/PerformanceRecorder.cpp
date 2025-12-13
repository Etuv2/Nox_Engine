#include "PerformanceRecorder.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include "json.hpp"

using json = nlohmann::json;

PerformanceRecorder::PerformanceRecorder()
    : m_isRecording(false)
    , m_maxFrames(10000)  // Default: ~3 minutes at 60 FPS
    , m_frameIndex(0)
{
    m_frameData.reserve(m_maxFrames);
}

PerformanceRecorder::~PerformanceRecorder() {
}

void PerformanceRecorder::StartRecording() {
    if (m_isRecording) {
        std::cout << "[PerformanceRecorder] Already recording, ignoring start request" << std::endl;
        return;
    }

    Clear();
    m_isRecording = true;
    m_frameIndex = 0;
    m_startTime = std::chrono::high_resolution_clock::now();
    
    std::cout << "[PerformanceRecorder] Started recording performance data" << std::endl;
}

void PerformanceRecorder::StopRecording() {
    if (!m_isRecording) {
        return;
    }

    m_isRecording = false;
    std::cout << "[PerformanceRecorder] Stopped recording. Captured " 
              << m_frameData.size() << " frames" << std::endl;
}

void PerformanceRecorder::RecordFrame(float frameTime, float fps) {
    if (!m_isRecording) {
        return;
    }

    FrameData frame;
    frame.timestamp = GetElapsedTime();
    frame.frameTime = frameTime;
    frame.fps = fps;
    frame.frameIndex = m_frameIndex++;

    // If we've reached max capacity, use circular buffer behavior
    if (m_frameData.size() >= m_maxFrames) {
        // Remove oldest frame
        m_frameData.erase(m_frameData.begin());
    }

    m_frameData.push_back(frame);
}

bool PerformanceRecorder::ExportToCSV(const std::string& filepath) {
    if (m_frameData.empty()) {
        std::cerr << "[PerformanceRecorder] No data to export" << std::endl;
        return false;
    }

    std::ofstream outFile(filepath);
    if (!outFile.is_open()) {
        std::cerr << "[PerformanceRecorder] Failed to open file for writing: " << filepath << std::endl;
        return false;
    }

    // Write CSV header
    outFile << "FrameIndex,Timestamp,FrameTime_ms,FPS\n";

    // Write data rows
    outFile << std::fixed << std::setprecision(6);
    for (const auto& frame : m_frameData) {
        outFile << frame.frameIndex << ","
                << frame.timestamp << ","
                << frame.frameTime << ","
                << frame.fps << "\n";
    }

    outFile.close();
    std::cout << "[PerformanceRecorder] Exported " << m_frameData.size() 
              << " frames to CSV: " << filepath << std::endl;
    return true;
}

bool PerformanceRecorder::ExportToJSON(const std::string& filepath) {
    if (m_frameData.empty()) {
        std::cerr << "[PerformanceRecorder] No data to export" << std::endl;
        return false;
    }

    try {
        json exportData;
        exportData["version"] = "1.0";
        exportData["frame_count"] = m_frameData.size();
        
        // Calculate statistics
        float totalTime = 0.0f;
        float minFrameTime = m_frameData[0].frameTime;
        float maxFrameTime = m_frameData[0].frameTime;
        
        for (const auto& frame : m_frameData) {
            totalTime += frame.frameTime;
            minFrameTime = std::min(minFrameTime, frame.frameTime);
            maxFrameTime = std::max(maxFrameTime, frame.frameTime);
        }
        
        float avgFrameTime = totalTime / m_frameData.size();
        
        exportData["statistics"] = {
            {"avg_frame_time_ms", avgFrameTime},
            {"min_frame_time_ms", minFrameTime},
            {"max_frame_time_ms", maxFrameTime},
            {"avg_fps", 1000.0f / avgFrameTime},
            {"total_duration_seconds", m_frameData.back().timestamp}
        };

        // Export frame data
        json framesArray = json::array();
        for (const auto& frame : m_frameData) {
            json frameJson;
            frameJson["frame_index"] = frame.frameIndex;
            frameJson["timestamp"] = frame.timestamp;
            frameJson["frame_time_ms"] = frame.frameTime;
            frameJson["fps"] = frame.fps;
            framesArray.push_back(frameJson);
        }
        
        exportData["frames"] = framesArray;

        // Write to file
        std::ofstream outFile(filepath);
        if (!outFile.is_open()) {
            std::cerr << "[PerformanceRecorder] Failed to open file for writing: " << filepath << std::endl;
            return false;
        }

        outFile << exportData.dump(2);  // Pretty print with 2-space indent
        outFile.close();

        std::cout << "[PerformanceRecorder] Exported " << m_frameData.size() 
                  << " frames to JSON: " << filepath << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[PerformanceRecorder] Error exporting to JSON: " << e.what() << std::endl;
        return false;
    }
}

void PerformanceRecorder::Clear() {
    m_frameData.clear();
    m_frameIndex = 0;
}

double PerformanceRecorder::GetElapsedTime() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - m_startTime);
    return duration.count() / 1000000.0;  // Convert to seconds
}
