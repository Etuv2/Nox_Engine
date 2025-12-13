#pragma once

#include <vector>
#include <string>
#include <chrono>

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
 * - Minimal performance overhead
 */
class PerformanceRecorder {
public:
    struct FrameData {
        double timestamp;        // Time since recording start (seconds)
        float frameTime;         // Total frame time (ms)
        float fps;               // Frames per second
        uint64_t frameIndex;     // Sequential frame number
        
        // Optional pass breakdown (can be extended)
        float cpuTime = 0.0f;    // CPU time (ms)
        float gpuTime = 0.0f;    // GPU time (ms)
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
    size_t GetFrameCount() const { return m_frameData.size(); }

    /**
     * @brief Clear all recorded data
     */
    void Clear();

    /**
     * @brief Set maximum number of frames to record (circular buffer)
     * Default is 10000 frames (~3 minutes at 60 FPS)
     */
    void SetMaxFrames(size_t maxFrames) { m_maxFrames = maxFrames; }

private:
    bool m_isRecording;
    std::vector<FrameData> m_frameData;
    size_t m_maxFrames;
    uint64_t m_frameIndex;
    std::chrono::high_resolution_clock::time_point m_startTime;

    // Helper to calculate elapsed time since recording start
    double GetElapsedTime() const;
};
