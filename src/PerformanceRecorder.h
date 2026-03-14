#pragma once

#include <vector>
#include <string>
#include <chrono>
#include <cstdint>

/**
 * @brief Records per-frame performance metrics for export and analysis
 * 
 * This class captures frame timing data during runtime and can export it
 * to CSV or JSON formats for external analysis (e.g., in Python).
 * 
 * Features:
 * - Per-frame timing with timestamps
 * - Efficient circular buffer to avoid memory bloat
 * - CSV export for quick analysis in Excel/Python
 * - JSON export for structured data processing
 * - Rendering mode tracking (standard vs path-traced)
 * - Minimal performance overhead
 */
class PerformanceRecorder {
public:
    struct PassMetrics {
        std::string name;
        float cpuTimeMs = 0.0f;
        float gpuTimeMs = 0.0f;
        uint64_t drawCalls = 0;
        uint64_t dispatchCount = 0;
        uint64_t bufferUploadBytes = 0;
        float cpuWaitSyncMs = 0.0f;
    };

    struct ECSMetrics {
        float transformSystemMs = 0.0f;
        float animationSystemMs = 0.0f;
        float physicsStepMs = 0.0f;
    };

    struct FrameData {
        double timestamp;        // Time since recording start (seconds)
        float frameTime;         // Total frame time (ms)
        float fps;               // Frames per second
        uint64_t frameIndex;     // Sequential frame number
        std::string renderMode;  // Rendering mode (e.g., "Standard" or "Path-Traced")
        
        float cpuTime = 0.0f;    // CPU aggregate time (ms)
        float gpuTime = 0.0f;    // GPU aggregate time (ms)
        float cpuWaitSyncMs = 0.0f;
        uint64_t drawCalls = 0;
        uint64_t dispatchCount = 0;
        uint64_t bufferUploadBytes = 0;

        ECSMetrics ecsMetrics;
        std::vector<PassMetrics> passMetrics;
    };

    PerformanceRecorder();
    ~PerformanceRecorder();

    /**
     * @brief Start recording performance data
     */
    void StartRecording();

    /**
     * @brief Stop recording performance data
     */
    void StopRecording();

    /**
     * @brief Check if currently recording
     */
    bool IsRecording() const { return m_isRecording; }

    /**
     * @brief Record a frame's timing data
     * @param frameTime Frame time in milliseconds
     * @param fps Frames per second
     */
    void RecordFrame(float frameTime, float fps);

    void SetCurrentFrameMetrics(const std::vector<PassMetrics>& passMetrics,
                                const ECSMetrics& ecsMetrics,
                                float cpuWaitSyncMs,
                                uint64_t bufferUploadBytes = 0);

    /**
     * @brief Set the current rendering mode
     * @param mode "Standard" for deferred rendering or "Path-Traced" for path tracing
     */
    void SetRenderingMode(const std::string& mode) { m_currentRenderMode = mode; }

    /**
     * @brief Export recorded data to CSV format
     * @param filepath Path to output CSV file
     * @return true if export succeeded, false otherwise
     */
    bool ExportToCSV(const std::string& filepath);

    /**
     * @brief Export recorded data to JSON format
     * @param filepath Path to output JSON file
     * @return true if export succeeded, false otherwise
     */
    bool ExportToJSON(const std::string& filepath);

    /**
     * @brief Get the number of recorded frames
     */
    size_t GetFrameCount() const { return m_frameCount; }

    /**
     * @brief Clear all recorded data
     */
    void Clear();

    /**
     * @brief Set maximum number of frames to record (circular buffer)
     * Default is 10000 frames (~3 minutes at 60 FPS)
     */
    void SetMaxFrames(size_t maxFrames);

private:
    bool m_isRecording;
    std::vector<FrameData> m_frameData;
    size_t m_maxFrames;
    size_t m_frameWriteIndex;
    size_t m_frameCount;
    uint64_t m_frameIndex;
    std::string m_currentRenderMode = "Standard";
    std::chrono::high_resolution_clock::time_point m_startTime;

    std::vector<PassMetrics> m_pendingPassMetrics;
    ECSMetrics m_pendingECSMetrics;
    float m_pendingCpuWaitSyncMs = 0.0f;
    uint64_t m_pendingBufferUploadBytes = 0;

    const FrameData& GetFrameAtLogicalIndex(size_t logicalIndex) const;

    /**
     * @brief Get elapsed time since recording started (in seconds)
     */
    double GetElapsedTime() const;
};
