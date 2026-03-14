#include "PerformanceRecorder.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include "json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// Helper function to create directories if they don't exist
static bool EnsureDirectoryExists(const std::string& filepath) {
    std::string dirPath = filepath.substr(0, filepath.find_last_of("/\\"));
    if (dirPath.empty() || dirPath == filepath) {
        return true; // No directory specified, file is in current directory
    }

    try {
        fs::path dir(dirPath);
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
            std::cout << "[PerformanceRecorder] Created directory: " << dirPath << std::endl;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[PerformanceRecorder] Failed to create directory: " << e.what() << std::endl;
        return false;
    }
}

PerformanceRecorder::PerformanceRecorder()
    : m_isRecording(false)
    , m_maxFrames(10000)  // Default: ~3 minutes at 60 FPS
    , m_frameWriteIndex(0)
    , m_frameCount(0)
    , m_frameIndex(0)
{
    m_frameData.resize(m_maxFrames);
}

PerformanceRecorder::~PerformanceRecorder() {
}


void PerformanceRecorder::SetMaxFrames(size_t maxFrames) {
    if (maxFrames == 0) {
        return;
    }

    m_maxFrames = maxFrames;
    m_frameData.clear();
    m_frameData.resize(m_maxFrames);
    m_frameWriteIndex = 0;
    m_frameCount = 0;
}

const PerformanceRecorder::FrameData& PerformanceRecorder::GetFrameAtLogicalIndex(size_t logicalIndex) const {
    const size_t oldestIndex = (m_frameCount == m_maxFrames) ? m_frameWriteIndex : 0;
    const size_t physicalIndex = (oldestIndex + logicalIndex) % m_maxFrames;
    return m_frameData[physicalIndex];
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
              << m_frameCount << " frames" << std::endl;
}

void PerformanceRecorder::SetCurrentFrameMetrics(const std::vector<PassMetrics>& passMetrics,
                                               const ECSMetrics& ecsMetrics,
                                               float cpuWaitSyncMs,
                                               uint64_t bufferUploadBytes) {
    m_pendingPassMetrics = passMetrics;
    m_pendingECSMetrics = ecsMetrics;
    m_pendingCpuWaitSyncMs = cpuWaitSyncMs;
    m_pendingBufferUploadBytes = bufferUploadBytes;
}

void PerformanceRecorder::RecordFrame(float frameTime, float fps) {
    if (!m_isRecording || m_maxFrames == 0) {
        return;
    }

    FrameData frame;
    frame.timestamp = GetElapsedTime();
    frame.frameTime = frameTime;
    frame.fps = fps;
    frame.frameIndex = m_frameIndex++;
    frame.renderMode = m_currentRenderMode;

    frame.passMetrics = m_pendingPassMetrics;
    frame.ecsMetrics = m_pendingECSMetrics;
    frame.cpuWaitSyncMs = m_pendingCpuWaitSyncMs;
    frame.bufferUploadBytes = m_pendingBufferUploadBytes;

    for (const auto& pass : frame.passMetrics) {
        frame.cpuTime += pass.cpuTimeMs;
        frame.gpuTime += pass.gpuTimeMs;
        frame.drawCalls += pass.drawCalls;
        frame.dispatchCount += pass.dispatchCount;
        frame.bufferUploadBytes += pass.bufferUploadBytes;
        frame.cpuWaitSyncMs += pass.cpuWaitSyncMs;
    }

    m_frameData[m_frameWriteIndex] = std::move(frame);
    m_frameWriteIndex = (m_frameWriteIndex + 1) % m_maxFrames;
    if (m_frameCount < m_maxFrames) {
        m_frameCount++;
    }
}

bool PerformanceRecorder::ExportToCSV(const std::string& filepath) {
    if (m_frameCount == 0) {
        std::cerr << "[PerformanceRecorder] No data to export" << std::endl;
        return false;
    }

    // Ensure directory exists
    if (!EnsureDirectoryExists(filepath)) {
        std::cerr << "[PerformanceRecorder] Failed to create output directory for: " << filepath << std::endl;
        return false;
    }

    std::ofstream outFile(filepath);
    if (!outFile.is_open()) {
        std::cerr << "[PerformanceRecorder] Failed to open file for writing: " << filepath << std::endl;
        return false;
    }

    // Write CSV header
    outFile << "FrameIndex,Timestamp,FrameTime_ms,FPS,RenderMode,CPUTime_ms,GPUTime_ms,CPUWaitSync_ms,DrawCalls,DispatchCount,BufferUploadBytes,TransformSystem_ms,AnimationSystem_ms,PhysicsStep_ms,Passes\n";

    // Write data rows
    outFile << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < m_frameCount; ++i) {
        const auto& frame = GetFrameAtLogicalIndex(i);
        std::ostringstream passes;
        for (size_t p = 0; p < frame.passMetrics.size(); ++p) {
            const auto& pass = frame.passMetrics[p];
            if (p > 0) passes << "|";
            passes << pass.name << ":"
                   << pass.cpuTimeMs << "/"
                   << pass.gpuTimeMs << "/"
                   << pass.drawCalls << "/"
                   << pass.dispatchCount << "/"
                   << pass.bufferUploadBytes << "/"
                   << pass.cpuWaitSyncMs;
        }

        outFile << frame.frameIndex << ","
                << frame.timestamp << ","
                << frame.frameTime << ","
                << frame.fps << ","
                << frame.renderMode << ","
                << frame.cpuTime << ","
                << frame.gpuTime << ","
                << frame.cpuWaitSyncMs << ","
                << frame.drawCalls << ","
                << frame.dispatchCount << ","
                << frame.bufferUploadBytes << ","
                << frame.ecsMetrics.transformSystemMs << ","
                << frame.ecsMetrics.animationSystemMs << ","
                << frame.ecsMetrics.physicsStepMs << ",\""
                << passes.str() << "\"\n";
    }

    outFile.close();
    std::cout << "[PerformanceRecorder] Exported " << m_frameCount
              << " frames to CSV: " << filepath << std::endl;
    return true;
}

bool PerformanceRecorder::ExportToJSON(const std::string& filepath) {
    if (m_frameCount == 0) {
        std::cerr << "[PerformanceRecorder] No data to export" << std::endl;
        return false;
    }

    if (!EnsureDirectoryExists(filepath)) {
        return false;
    }

    try {
        json exportData;
        exportData["version"] = "1.0";
        exportData["frame_count"] = m_frameCount;

        // Determine rendering mode(s) used during recording
        std::string recordedRenderMode = "Mixed";
        if (m_frameCount > 0) {
            const std::string& firstMode = GetFrameAtLogicalIndex(0).renderMode;
            bool allSameMode = true;
            for (size_t i = 0; i < m_frameCount; ++i) {
                if (GetFrameAtLogicalIndex(i).renderMode != firstMode) {
                    allSameMode = false;
                    break;
                }
            }
            if (allSameMode) {
                recordedRenderMode = firstMode;
            }
        }

        // Calculate statistics
        float totalTime = 0.0f;
        float minFrameTime = GetFrameAtLogicalIndex(0).frameTime;
        float maxFrameTime = GetFrameAtLogicalIndex(0).frameTime;
        float totalCPU = 0.0f;
        float totalGPU = 0.0f;

        for (size_t i = 0; i < m_frameCount; ++i) {
            const auto& frame = GetFrameAtLogicalIndex(i);
            totalTime += frame.frameTime;
            minFrameTime = std::min(minFrameTime, frame.frameTime);
            maxFrameTime = std::max(maxFrameTime, frame.frameTime);
            totalCPU += frame.cpuTime;
            totalGPU += frame.gpuTime;
        }

        float avgFrameTime = totalTime / static_cast<float>(m_frameCount);

        exportData["statistics"] = {
            {"rendering_mode", recordedRenderMode},
            {"avg_frame_time_ms", avgFrameTime},
            {"min_frame_time_ms", minFrameTime},
            {"max_frame_time_ms", maxFrameTime},
            {"avg_fps", 1000.0f / avgFrameTime},
            {"total_duration_seconds", GetFrameAtLogicalIndex(m_frameCount - 1).timestamp},
            {"avg_cpu_time_ms", totalCPU / static_cast<float>(m_frameCount)},
            {"avg_gpu_time_ms", totalGPU / static_cast<float>(m_frameCount)}
        };

        // Export frame data
        json framesArray = json::array();
        for (size_t i = 0; i < m_frameCount; ++i) {
            const auto& frame = GetFrameAtLogicalIndex(i);
            json frameJson;
            frameJson["frame_index"] = frame.frameIndex;
            frameJson["timestamp"] = frame.timestamp;
            frameJson["frame_time_ms"] = frame.frameTime;
            frameJson["fps"] = frame.fps;
            frameJson["render_mode"] = frame.renderMode;
            frameJson["cpu_time_ms"] = frame.cpuTime;
            frameJson["gpu_time_ms"] = frame.gpuTime;
            frameJson["cpu_wait_sync_ms"] = frame.cpuWaitSyncMs;
            frameJson["draw_calls"] = frame.drawCalls;
            frameJson["dispatch_count"] = frame.dispatchCount;
            frameJson["buffer_upload_bytes"] = frame.bufferUploadBytes;
            frameJson["ecs_metrics"] = {
                {"transform_system_ms", frame.ecsMetrics.transformSystemMs},
                {"animation_system_ms", frame.ecsMetrics.animationSystemMs},
                {"physics_step_ms", frame.ecsMetrics.physicsStepMs}
            };

            json passes = json::array();
            for (const auto& pass : frame.passMetrics) {
                passes.push_back({
                    {"name", pass.name},
                    {"cpu_time_ms", pass.cpuTimeMs},
                    {"gpu_time_ms", pass.gpuTimeMs},
                    {"draw_calls", pass.drawCalls},
                    {"dispatch_count", pass.dispatchCount},
                    {"buffer_upload_bytes", pass.bufferUploadBytes},
                    {"cpu_wait_sync_ms", pass.cpuWaitSyncMs}
                });
            }
            frameJson["passes"] = passes;
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

        std::cout << "[PerformanceRecorder] Exported " << m_frameCount
                  << " frames to JSON: " << filepath << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[PerformanceRecorder] Error exporting to JSON: " << e.what() << std::endl;
        return false;
    }
}

void PerformanceRecorder::Clear() {
    m_frameData.clear();
    m_frameData.resize(m_maxFrames);
    m_frameWriteIndex = 0;
    m_frameCount = 0;
    m_frameIndex = 0;
    m_pendingPassMetrics.clear();
    m_pendingECSMetrics = ECSMetrics{};
    m_pendingCpuWaitSyncMs = 0.0f;
    m_pendingBufferUploadBytes = 0;
}

double PerformanceRecorder::GetElapsedTime() const {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - m_startTime);
    return duration.count() / 1000000.0;  // Convert to seconds
}
