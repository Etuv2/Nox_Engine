#pragma once

// ProcessRunner: runs one external command at a time in the background for editor operations
// (Build, Run Standalone, Package). Output is captured line by line on a worker thread.
//
// Usage (UI thread):
//   ProcessRunner runner;
//   runner.Start("Build", "powershell -File tools\\Build.ps1", repoRoot);
//   // every frame:
//   if (runner.IsRunning()) { size_t n = runner.LineCount(); auto lines = runner.LinesSince(seen); ... }
//   // after finish:
//   runner.Succeeded(); runner.ExitCode(); runner.LastError();
//
// Threading: Start/Cancel/LaunchDetached are meant for the UI thread. IsRunning, ExitCode,
// Succeeded, ElapsedSeconds, Title, LineCount, LinesSince, Progress and LastError may be called from
// any thread (they take an internal lock or read atomics).
//
// Process tree: the child is created suspended and assigned to a Job Object with
// KILL_ON_JOB_CLOSE, so Cancel() (TerminateJobObject) kills compilers and other grandchildren, and
// the tree also dies if the editor exits. Children run with CREATE_NO_WINDOW; stdin is NUL.
// Output is read as bytes and any invalid UTF-8 is replaced with '?', so every returned line is
// valid UTF-8.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ProcessRunner {
public:
    ProcessRunner() = default;
    ~ProcessRunner();

    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    // Starts commandLine (UTF-8, passed to CreateProcess as-is, so quote paths with spaces) in
    // workingDir (UTF-8, may be empty for the current directory). Returns false if a process is
    // already running (overlap protection) or if the process could not be created; in the latter
    // case LastError() describes the failure and LineCount() gets one explanatory line.
    bool Start(const std::string& title, const std::string& commandLine, const std::string& workingDir);

    // True from a successful Start() until the child has exited and its output was drained.
    bool IsRunning() const;

    // Process exit code once finished; -1 while running or if the process never started. A cancelled
    // process reports the code given by TerminateJobObject (1).
    int ExitCode() const;

    // True when the process finished with exit code 0 and was not cancelled.
    bool Succeeded() const;

    // Wall time since Start(); frozen when the process finishes. 0 if never started.
    float ElapsedSeconds() const;

    // Title passed to the last Start() call.
    std::string Title() const;

    // Number of captured output lines (stdout and stderr merged, without line endings).
    size_t LineCount() const;

    // Copy of the captured lines from index onward (thread-safe). Empty if index >= LineCount().
    std::vector<std::string> LinesSince(size_t index) const;

    // Kills the whole process tree. No-op when nothing is running. Output already captured is kept.
    void Cancel();

    // Best-effort progress in [0, 1]. Parses lines like "[NOX-PROGRESS] 0.42" printed by the
    // scripts (the last such line wins). Returns -1 when unknown.
    float Progress() const;

    // First output line that looks like a failure (contains "error" case-insensitively, ignoring
    // summary lines such as "0 Error(s)", or "FAILED"). If the process exited non-zero without such
    // a line, a generic "exit code N" message is returned. Empty when there is nothing to report.
    std::string LastError() const;

    // Fire-and-forget launch of an unrelated program (e.g. Run Standalone). Inherits no handles and
    // is not tied to a Job Object, so it outlives the editor. Returns false if CreateProcess fails.
    // hideConsole adds CREATE_NO_WINDOW (for console tools started from the editor, e.g. a custom script editor command).
    static bool LaunchDetached(const std::string& commandLine, const std::string& workingDir, bool hideConsole = false);

private:
    using Clock = std::chrono::steady_clock;

    void WorkerMain(void* readPipe, void* process, void* job);
    void AppendLine(std::string line);
    void Finish();

    mutable std::mutex m_mutex;     // guards everything below that is not atomic
    std::vector<std::string> m_lines;
    std::string m_title;
    std::string m_commandLine;
    std::string m_lastError;
    std::string m_firstErrorLine;
    float m_progress = -1.0f;
    Clock::time_point m_startTime{};
    Clock::time_point m_endTime{};
    bool m_hasEndTime = false;
    void* m_job = nullptr;          // Job Object while running (worker closes it), guarded by m_mutex
    void* m_process = nullptr;      // child process handle while running (worker closes it), guarded by m_mutex

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_started{false};
    std::atomic<int> m_exitCode{-1};

    std::thread m_worker;
};
